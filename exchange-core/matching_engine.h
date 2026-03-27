#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "exchange-core/events.h"
#include "exchange-core/intrusive_list.h"
#include "exchange-core/listeners.h"
#include "exchange-core/match_algo.h"
#include "exchange-core/object_pool.h"
#include "exchange-core/order_persistence.h"
#include "exchange-core/orderbook.h"
#include "exchange-core/rate_tracker.h"
#include "exchange-core/stop_book.h"
#include "exchange-core/trade_registry.h"
#include "exchange-core/types.h"

namespace exchange {

// --- Engine configuration ---

struct EngineConfig {
    Price tick_size;              // minimum price increment
    Quantity lot_size;            // minimum quantity increment
    Price price_band_low;         // reject orders below this (0 = no band)
    Price price_band_high;        // reject orders above this (0 = no band)
    Quantity max_order_size{0};   // maximum single-order quantity (0 = no limit)
    Price daily_limit_high{0};    // upper daily price limit (0 = no limit)
    Price daily_limit_low{0};     // lower daily price limit (0 = no limit)
    ThrottleConfig throttle{};    // per-account rate limit (disabled by default)
};

// --- Request structs ---

struct OrderRequest {
    uint64_t client_order_id;
    uint64_t account_id;
    Side side;
    OrderType type;
    TimeInForce tif;
    Price price;              // ignored for Market orders
    Quantity quantity;
    Price stop_price;         // only for Stop/StopLimit
    Timestamp timestamp;
    Timestamp gtd_expiry;     // only for GTD
    Quantity display_qty{0};  // 0 = fully visible, > 0 = iceberg display size
};

struct ModifyRequest {
    OrderId order_id;
    uint64_t client_order_id; // echoed in OrderModified/OrderModifyRejected
    Price new_price;
    Quantity new_quantity;
    Timestamp timestamp;
};

// --- Auction result ---

struct AuctionResult {
    Price price{0};
    Quantity matched_volume{0};
    Quantity buy_surplus{0};   // unfilled buy qty at auction price
    Quantity sell_surplus{0};  // unfilled sell qty at auction price
    bool has_price{false};     // false if book is empty or no crossing
};

// --- Matching Engine ---

template <
    typename Derived,
    typename OrderListenerT,
    typename MdListenerT,
    typename MatchAlgoT = FifoMatch,
    size_t MaxOrders = 100000,
    size_t MaxPriceLevels = 10000,
    size_t MaxOrderIds = 1000000,
    size_t MaxAccounts = 4096
>
class MatchingEngine {
public:
    MatchingEngine(EngineConfig config,
                   OrderListenerT& order_listener,
                   MdListenerT& md_listener)
        : config_(config)
        , rate_tracker_(config.throttle)
        , order_listener_(order_listener)
        , md_listener_(md_listener) {
        order_index_.fill(nullptr);
    }

    // Non-copyable, non-movable.
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    // new_order -- validate, accept, and route an incoming order

    void new_order(const OrderRequest& req) {
        auto reject = [&](RejectReason r) {
            order_listener_.on_order_rejected(
                OrderRejected{req.client_order_id, req.timestamp, r});
        };

        // Rate throttle check (zero overhead when disabled via CRTP hook)
        if (derived().is_rate_check_enabled() &&
            !rate_tracker_.check_and_increment(req.account_id, req.timestamp))
            { reject(RejectReason::RateThrottled); return; }

        if (!derived().on_validate_order(req))
            { reject(RejectReason::ExchangeSpecific); return; }
        if (!derived().is_tif_valid(req.tif))
            { reject(RejectReason::InvalidTif); return; }
        if (req.quantity <= 0)
            { reject(RejectReason::InvalidQuantity); return; }
        if (config_.lot_size > 0 && (req.quantity % config_.lot_size) != 0)
            { reject(RejectReason::InvalidQuantity); return; }

        // Iceberg display_qty validation
        if (req.display_qty > 0) {
            if (req.display_qty > req.quantity)
                { reject(RejectReason::InvalidQuantity); return; }
            if (config_.lot_size > 0 &&
                (req.display_qty % config_.lot_size) != 0)
                { reject(RejectReason::InvalidQuantity); return; }
            Quantity min_display = derived().get_min_display_qty(req);
            if (min_display > 0 && req.display_qty < min_display)
                { reject(RejectReason::InvalidQuantity); return; }
        }

        // Max order size check (0 = no limit)
        if (config_.max_order_size > 0 && req.quantity > config_.max_order_size)
            { reject(RejectReason::MaxOrderSizeExceeded); return; }

        // Price validation for Limit and StopLimit
        if (req.type == OrderType::Limit || req.type == OrderType::StopLimit) {
            if (req.price <= 0)
                { reject(RejectReason::InvalidPrice); return; }
            if (config_.tick_size > 0 && (req.price % config_.tick_size) != 0)
                { reject(RejectReason::InvalidPrice); return; }
            auto [band_low, band_high] =
                derived().calculate_dynamic_bands(last_trade_price_);
            if (band_low > 0 && req.price < band_low)
                { reject(RejectReason::PriceBandViolation); return; }
            if (band_high > 0 && req.price > band_high)
                { reject(RejectReason::PriceBandViolation); return; }
        }

        // Stop price validation
        if ((req.type == OrderType::Stop || req.type == OrderType::StopLimit)
            && req.stop_price <= 0)
            { reject(RejectReason::InvalidPrice); return; }

        // Reject all orders in Closed state
        if (current_state_ == SessionState::Closed)
            { reject(RejectReason::ExchangeSpecific); return; }

        // Phase validation (exchange-specific per-phase rules)
        if (!derived().is_order_allowed_in_phase(req, current_state_))
            { reject(RejectReason::InvalidTif); return; }

        if (next_order_id_ >= MaxOrderIds)
            { reject(RejectReason::PoolExhausted); return; }

        Order* order = order_pool_.allocate();
        if (order == nullptr)
            { reject(RejectReason::PoolExhausted); return; }

        // 9. Initialize order and assign sequential ID
        order->id                 = next_order_id_++;
        order->client_order_id    = req.client_order_id;
        order->account_id         = req.account_id;
        order->price              = (req.type == OrderType::Stop)
                                        ? req.stop_price : req.price;
        order->quantity           = req.quantity;
        order->filled_quantity    = 0;
        order->remaining_quantity = req.quantity;
        order->side               = req.side;
        order->type               = req.type;
        order->tif                = req.tif;
        order->timestamp          = req.timestamp;
        order->gtd_expiry         = req.gtd_expiry;
        order->display_qty        = req.display_qty;
        order->total_qty          = req.quantity;
        order->prev               = nullptr;
        order->next               = nullptr;
        order->level              = nullptr;

        // Iceberg: only show first tranche in the book
        if (req.display_qty > 0) {
            order->remaining_quantity = req.display_qty;
        }

        order_index_[order->id] = order;

        // 10. Fire OrderAccepted
        order_listener_.on_order_accepted(OrderAccepted{
            order->id, order->client_order_id, req.timestamp});

        // 11. Route by order type
        if (req.type == OrderType::Stop || req.type == OrderType::StopLimit) {
            // For stop orders, use stop_price as the price for the stop book
            Price original_price = order->price;
            order->price = req.stop_price;
            insert_into_stop_book(order);
            // Restore the limit price for StopLimit orders
            if (req.type == OrderType::StopLimit) {
                order->price = original_price;
            }
            return;
        }

        // 12. Auction collection phases: insert into book without matching
        bool is_collection_phase =
            (current_state_ == SessionState::PreOpen ||
             current_state_ == SessionState::PreClose ||
             current_state_ == SessionState::VolatilityAuction);

        if (is_collection_phase) {
            if (req.type == OrderType::Limit) {
                insert_into_book(order, req.timestamp);
                fire_top_of_book(req.timestamp);
            } else {
                // Market orders in collection phase: cancel remainder
                OrderId oid = order->id;
                order_index_[oid] = nullptr;
                order_listener_.on_order_cancelled(OrderCancelled{
                    oid, req.timestamp, CancelReason::IOCRemainder});
                order_pool_.deallocate(order);
            }
            return;
        }

        // 13. Normal continuous matching and stop cascade
        match_order(order, req.timestamp);
        check_and_trigger_stops(req.timestamp);
    }

    // cancel_order -- remove order from book/stop book, fire callbacks

    void cancel_order(OrderId id, Timestamp ts) {
        // Reject cancel in Closed state
        if (current_state_ == SessionState::Closed) {
            order_listener_.on_order_cancel_rejected(
                OrderCancelRejected{id, 0, ts, RejectReason::ExchangeSpecific});
            return;
        }

        if (id == 0 || id >= MaxOrderIds || order_index_[id] == nullptr) {
            order_listener_.on_order_cancel_rejected(
                OrderCancelRejected{id, 0, ts, RejectReason::UnknownOrder});
            return;
        }

        // Rate throttle check (uses account_id from existing order)
        if (derived().is_rate_check_enabled() &&
            !rate_tracker_.check_and_increment(
                order_index_[id]->account_id, ts)) {
            order_listener_.on_order_cancel_rejected(
                OrderCancelRejected{id, 0, ts, RejectReason::RateThrottled});
            return;
        }

        cancel_active_order(order_index_[id], ts,
                            CancelReason::UserRequested);
    }

    // modify_order -- cancel-replace or amend-down based on policy

    void modify_order(const ModifyRequest& req) {
        auto mod_reject = [&](RejectReason r) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp, r});
        };

        // Reject modify in Closed state
        if (current_state_ == SessionState::Closed)
            { mod_reject(RejectReason::ExchangeSpecific); return; }

        if (req.order_id == 0 || req.order_id >= MaxOrderIds ||
            order_index_[req.order_id] == nullptr)
            { mod_reject(RejectReason::UnknownOrder); return; }

        // Rate throttle check (uses account_id from existing order)
        if (derived().is_rate_check_enabled() &&
            !rate_tracker_.check_and_increment(
                order_index_[req.order_id]->account_id, req.timestamp))
            { mod_reject(RejectReason::RateThrottled); return; }

        if (req.new_quantity <= 0)
            { mod_reject(RejectReason::InvalidQuantity); return; }
        if (req.new_price <= 0)
            { mod_reject(RejectReason::InvalidPrice); return; }
        if (config_.tick_size > 0 && (req.new_price % config_.tick_size) != 0)
            { mod_reject(RejectReason::InvalidPrice); return; }
        if (config_.lot_size > 0 && (req.new_quantity % config_.lot_size) != 0)
            { mod_reject(RejectReason::InvalidQuantity); return; }

        Order* order = order_index_[req.order_id];
        ModifyPolicy policy = derived().get_modify_policy();

        if (policy == ModifyPolicy::RejectModify)
            { mod_reject(RejectReason::ExchangeSpecific); return; }

        // Amend-down: reduce qty in-place if price unchanged and qty
        // is reducing (preserves time priority)
        if (policy == ModifyPolicy::AmendDown &&
            req.new_price == order->price &&
            req.new_quantity < order->quantity &&
            req.new_quantity > order->filled_quantity) {
            Quantity old_remaining = order->remaining_quantity;
            Quantity new_remaining = req.new_quantity - order->filled_quantity;
            order->quantity = req.new_quantity;
            order->remaining_quantity = new_remaining;

            // Update level total
            if (order->level) {
                order->level->total_quantity -= (old_remaining - new_remaining);
            }

            order_listener_.on_order_modified(OrderModified{
                order->id, req.client_order_id,
                req.new_price, req.new_quantity, req.timestamp});

            // L2/L1 updates
            if (order->level) {
                md_listener_.on_depth_update(DepthUpdate{
                    order->side, order->level->price,
                    order->level->total_quantity,
                    order->level->order_count,
                    DepthUpdate::Update, req.timestamp});
            }
            return;
        }

        // Cancel-replace: remove old order, re-enter as new
        Side order_side = order->side;
        Price old_price = order->price;

        // 1. Remove old order from book
        PriceLevel* freed = book_.remove_order(order);

        // Fire L3: OrderBookAction (Cancel old)
        md_listener_.on_order_book_action(OrderBookAction{
            order->id, order_side, old_price,
            order->remaining_quantity,
            OrderBookAction::Cancel, req.timestamp});

        // Fire L2: DepthUpdate for old level
        fire_depth_after_remove(freed, order_side, old_price,
                                req.timestamp);

        // 3. Update order fields for the new order
        Quantity already_filled = order->filled_quantity;
        order->price = req.new_price;
        order->quantity = req.new_quantity;
        order->remaining_quantity = req.new_quantity - already_filled;
        order->timestamp = req.timestamp;
        order->prev = nullptr;
        order->next = nullptr;
        order->level = nullptr;

        // Fire OrderModified
        order_listener_.on_order_modified(OrderModified{
            order->id, req.client_order_id,
            req.new_price, req.new_quantity, req.timestamp});

        // 4. Match the modified order (may trigger fills)
        match_order(order, req.timestamp);

        // Trigger stop cascade after any fills
        check_and_trigger_stops(req.timestamp);
    }

    // trigger_expiry -- cancel DAY/GTD orders that have expired
    // O(n) linear scan of order_index_.

    void trigger_expiry(Timestamp now, TimeInForce tif) {
        for (size_t i = 1; i < next_order_id_; ++i) {
            Order* order = order_index_[i];
            if (order == nullptr) continue;
            if (order->tif != tif) continue;

            bool should_expire = false;
            if (tif == TimeInForce::DAY) {
                should_expire = true;
            } else if (tif == TimeInForce::GTD) {
                should_expire = (order->gtd_expiry <= now);
            }
            if (!should_expire) continue;

            cancel_active_order(order, now, CancelReason::Expired);
        }
    }

    // mass_cancel -- cancel all resting orders for a specific account.
    // O(n) linear scan of order_index_. Returns count of cancelled orders.

    size_t mass_cancel(uint64_t account_id, Timestamp ts) {
        size_t count = 0;
        for (size_t i = 1; i < next_order_id_; ++i) {
            Order* order = order_index_[i];
            if (order && order->account_id == account_id) {
                cancel_active_order(order, ts, CancelReason::MassCancelled);
                ++count;
            }
        }
        return count;
    }

    // mass_cancel_all -- cancel all active orders.
    // O(n) linear scan of order_index_. Returns count of cancelled orders.

    size_t mass_cancel_all(Timestamp ts) {
        size_t count = 0;
        for (size_t i = 1; i < next_order_id_; ++i) {
            Order* order = order_index_[i];
            if (order) {
                cancel_active_order(order, ts, CancelReason::MassCancelled);
                ++count;
            }
        }
        return count;
    }

    // restore_order -- re-insert a previously serialized order into the book.
    //
    // Used for GTC cross-session persistence: after a session boundary,
    // the exchange restores resting GTC orders from persistent storage.
    //
    // Validates: price bands, tick/lot size, expired GTD, duplicate ID,
    // pool capacity.  On success, allocates from pool, inserts into book
    // at the original price, and fires OrderAccepted.  On failure, fires
    // OrderRejected.
    //
    // Restored orders are placed directly on the book -- they do NOT
    // participate in matching (they are resting orders, not aggressors).

    void restore_order(const SerializedOrder& s, Timestamp ts) {
        auto reject = [&](RejectReason r) {
            order_listener_.on_order_rejected(
                OrderRejected{s.client_order_id, ts, r});
        };

        // Validate order ID is within range and not already taken.
        if (s.id == 0 || s.id >= MaxOrderIds)
            { reject(RejectReason::ExchangeSpecific); return; }
        if (order_index_[s.id] != nullptr)
            { reject(RejectReason::ExchangeSpecific); return; }

        // Validate quantity.
        if (s.remaining_quantity <= 0)
            { reject(RejectReason::InvalidQuantity); return; }
        if (config_.lot_size > 0 && (s.quantity % config_.lot_size) != 0)
            { reject(RejectReason::InvalidQuantity); return; }

        // Validate price (limit orders only).
        if (s.type == OrderType::Limit || s.type == OrderType::StopLimit) {
            if (s.price <= 0)
                { reject(RejectReason::InvalidPrice); return; }
            if (config_.tick_size > 0 && (s.price % config_.tick_size) != 0)
                { reject(RejectReason::InvalidPrice); return; }
            auto [band_low, band_high] =
                derived().calculate_dynamic_bands(last_trade_price_);
            if (band_low > 0 && s.price < band_low)
                { reject(RejectReason::PriceBandViolation); return; }
            if (band_high > 0 && s.price > band_high)
                { reject(RejectReason::PriceBandViolation); return; }
        }

        // Validate GTD expiry.
        if (s.tif == TimeInForce::GTD && s.gtd_expiry <= ts)
            { reject(RejectReason::ExchangeSpecific); return; }

        // CRTP hook for exchange-specific restore validation.
        if (!derived().on_validate_restore(s))
            { reject(RejectReason::ExchangeSpecific); return; }

        // Allocate from pool.
        Order* order = order_pool_.allocate();
        if (order == nullptr)
            { reject(RejectReason::PoolExhausted); return; }

        // Populate order from serialized data.
        deserialize_order(s, order);

        // Register in index.
        order_index_[s.id] = order;

        // Advance next_order_id_ past the restored ID to avoid collision.
        if (s.id >= next_order_id_) {
            next_order_id_ = s.id + 1;
        }

        // Fire OrderAccepted.
        order_listener_.on_order_accepted(OrderAccepted{
            order->id, order->client_order_id, ts});

        // Insert into book (resting, no matching).
        insert_into_book(order, ts);
        fire_top_of_book(ts);
    }

    // Session state management

    SessionState session_state() const { return current_state_; }

    void set_session_state(SessionState new_state, Timestamp ts) {
        SessionState old = current_state_;
        if (old == new_state) return;  // no-op

        if (!derived().on_session_transition(old, new_state, ts)) {
            return;  // exchange blocked the transition
        }

        current_state_ = new_state;
        md_listener_.on_market_status(MarketStatus{.state = new_state, .ts = ts});
    }

    // calculate_auction_price -- find the equilibrium price for uncrossing.
    //
    // Walks all price levels on both sides, considers each price level as a
    // candidate auction price, and selects the price that:
    //   1. Maximizes matched volume   (primary)
    //   2. Minimizes order imbalance  (secondary tiebreak)
    //   3. Is closest to reference_price (tertiary tiebreak)
    //   4. Is highest price among remaining ties (convention)
    //
    // This is the standard uncrossing algorithm used by major exchanges.
    // O(n^2) in the number of price levels -- acceptable for auction path.

    AuctionResult calculate_auction_price(Price reference_price) const {
        AuctionResult best{};

        // Collect all candidate prices from every price level on both sides.
        std::vector<Price> candidates;
        for (const PriceLevel* lv = book_.best_bid(); lv; lv = lv->next) {
            candidates.push_back(lv->price);
        }
        for (const PriceLevel* lv = book_.best_ask(); lv; lv = lv->next) {
            candidates.push_back(lv->price);
        }

        if (candidates.empty()) return best;

        // Sort ascending and deduplicate.
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(
            std::unique(candidates.begin(), candidates.end()),
            candidates.end());

        // Track best imbalance separately so we can compare correctly.
        Quantity best_imbalance = 0;
        Price best_ref_dist = 0;

        for (Price p : candidates) {
            // Cumulative buy volume: all bids at price >= p.
            Quantity buy_vol = 0;
            for (const PriceLevel* lv = book_.best_bid(); lv; lv = lv->next) {
                if (lv->price >= p) buy_vol += lv->total_quantity;
            }

            // Cumulative sell volume: all asks at price <= p.
            Quantity sell_vol = 0;
            for (const PriceLevel* lv = book_.best_ask(); lv; lv = lv->next) {
                if (lv->price <= p) sell_vol += lv->total_quantity;
            }

            Quantity matched = std::min(buy_vol, sell_vol);
            if (matched == 0) continue;

            Quantity imbalance = std::abs(buy_vol - sell_vol);
            Price ref_dist = std::abs(p - reference_price);

            bool is_better = false;
            if (!best.has_price) {
                is_better = true;
            } else if (matched > best.matched_volume) {
                is_better = true;
            } else if (matched == best.matched_volume) {
                if (imbalance < best_imbalance) {
                    is_better = true;
                } else if (imbalance == best_imbalance) {
                    if (ref_dist < best_ref_dist) {
                        is_better = true;
                    } else if (ref_dist == best_ref_dist) {
                        // Tiebreak 4: higher price wins (convention).
                        if (p > best.price) is_better = true;
                    }
                }
            }

            if (is_better) {
                best.price = p;
                best.matched_volume = matched;
                best.buy_surplus = buy_vol - matched;
                best.sell_surplus = sell_vol - matched;
                best.has_price = true;
                best_imbalance = imbalance;
                best_ref_dist = ref_dist;
            }
        }

        return best;
    }

    // execute_auction -- uncross the book at a single equilibrium price.
    //
    // Calculates the auction price using the reference price, then walks both
    // sides of the book filling all matchable orders at that single price.
    // Handles iceberg tranche reveals, removes fully-filled orders, and fires
    // the full callback sequence: OrderFilled/PartiallyFilled, Trade,
    // OrderBookAction (L3), DepthUpdate (L2), TopOfBook (L1).
    //
    // Convention: in auctions both sides are resting. We use the buy side as
    // the "aggressor" in callbacks by convention.

    void execute_auction(Price reference_price, Timestamp ts) {
        AuctionResult result = calculate_auction_price(reference_price);
        if (!result.has_price || result.matched_volume == 0) return;

        Price auction_price = result.price;
        Quantity remaining_to_fill = result.matched_volume;

        // Walk bids from best (highest) and asks from best (lowest).
        // All bids with price >= auction_price and all asks with
        // price <= auction_price are matchable.
        PriceLevel* bid_level = book_.best_bid();
        PriceLevel* ask_level = book_.best_ask();

        while (remaining_to_fill > 0 && bid_level && ask_level) {
            if (bid_level->price < auction_price) break;
            if (ask_level->price > auction_price) break;

            Order* bid = bid_level->head;
            Order* ask = ask_level->head;

            while (remaining_to_fill > 0 && bid && ask) {
                Quantity fill_qty = std::min({remaining_to_fill,
                                              bid->remaining_quantity,
                                              ask->remaining_quantity});

                // Apply fill to both sides
                bid->filled_quantity    += fill_qty;
                bid->remaining_quantity -= fill_qty;
                ask->filled_quantity    += fill_qty;
                ask->remaining_quantity -= fill_qty;
                remaining_to_fill       -= fill_qty;

                // Update level totals
                bid->level->total_quantity -= fill_qty;
                ask->level->total_quantity -= fill_qty;

                last_trade_price_ = auction_price;

                // Fire order callbacks (buy side is aggressor by convention)
                bool bid_done = (bid->remaining_quantity == 0 &&
                                 !(bid->display_qty > 0 &&
                                   bid->total_qty > bid->filled_quantity));
                bool ask_done = (ask->remaining_quantity == 0 &&
                                 !(ask->display_qty > 0 &&
                                   ask->total_qty > ask->filled_quantity));

                if (bid_done && ask_done) {
                    order_listener_.on_order_filled(OrderFilled{
                        bid->id, ask->id,
                        auction_price, fill_qty, ts});
                } else {
                    Quantity bid_rem = bid->remaining_quantity;
                    if (bid->display_qty > 0 &&
                        bid->remaining_quantity == 0 &&
                        bid->total_qty > bid->filled_quantity) {
                        bid_rem = std::min(bid->display_qty,
                                           bid->total_qty -
                                               bid->filled_quantity);
                    }
                    Quantity ask_rem = ask->remaining_quantity;
                    if (ask->display_qty > 0 &&
                        ask->remaining_quantity == 0 &&
                        ask->total_qty > ask->filled_quantity) {
                        ask_rem = std::min(ask->display_qty,
                                           ask->total_qty -
                                               ask->filled_quantity);
                    }
                    order_listener_.on_order_partially_filled(
                        OrderPartiallyFilled{
                            bid->id, ask->id,
                            auction_price, fill_qty,
                            bid_rem, ask_rem, ts});
                }

                // Fire Trade
                md_listener_.on_trade(Trade{
                    auction_price, fill_qty,
                    bid->id, ask->id,
                    Side::Buy, ts});

                // Fire L3 for both sides
                md_listener_.on_order_book_action(OrderBookAction{
                    bid->id, Side::Buy, auction_price,
                    fill_qty, OrderBookAction::Fill, ts});
                md_listener_.on_order_book_action(OrderBookAction{
                    ask->id, Side::Sell, auction_price,
                    fill_qty, OrderBookAction::Fill, ts});

                // Advance pointers before cleanup
                Order* next_bid = bid->next;
                Order* next_ask = ask->next;

                // Handle bid side cleanup
                if (bid->remaining_quantity == 0) {
                    bid = auction_handle_filled_order(bid, ts);
                    if (!bid) bid = next_bid;
                }
                if (ask->remaining_quantity == 0) {
                    ask = auction_handle_filled_order(ask, ts);
                    if (!ask) ask = next_ask;
                }
            }

            // Advance to next level if current is exhausted or not matchable
            if (bid_level->order_count == 0 ||
                bid_level->head == nullptr) {
                bid_level = book_.best_bid();
            } else if (bid_level->price < auction_price) {
                bid_level = nullptr;
            }
            if (ask_level->order_count == 0 ||
                ask_level->head == nullptr) {
                ask_level = book_.best_ask();
            } else if (ask_level->price > auction_price) {
                ask_level = nullptr;
            }
        }

        // Fire L2 for any modified levels and TopOfBook after all fills
        fire_top_of_book(ts);
    }

    // publish_indicative_price -- calculate and broadcast the indicative
    // auction price during collection phases (PreOpen, PreClose, etc.).
    //
    // Called by the exchange implementation at its discretion (e.g. after
    // every order in PreOpen, or on a timer). The engine itself never calls
    // this automatically.

    void publish_indicative_price(Price reference_price, Timestamp ts) {
        AuctionResult result = calculate_auction_price(reference_price);
        if (result.has_price) {
            md_listener_.on_indicative_price(IndicativePrice{
                .price = result.price,
                .matched_volume = result.matched_volume,
                .buy_surplus = result.buy_surplus,
                .sell_surplus = result.sell_surplus,
                .ts = ts,
            });
        }
    }

    // Status queries

    size_t active_order_count() const {
        return MaxOrders - order_pool_.available();
    }

    size_t available_order_slots() const {
        return order_pool_.available();
    }

    size_t available_level_slots() const {
        return level_pool_.available();
    }

    // CRTP hook defaults

    // Called before state transition. Return false to block.
    bool on_session_transition(SessionState /*old_state*/,
                               SessionState /*new_state*/,
                               Timestamp /*ts*/) {
        return true;
    }

    // Called during new_order validation. Return false to reject order.
    bool is_order_allowed_in_phase(const OrderRequest& /*req*/,
                                   SessionState /*state*/) {
        return true;  // default: all orders allowed in all phases
    }

    bool on_validate_order(const OrderRequest&) { return true; }
    bool on_validate_restore(const SerializedOrder&) { return true; }
    bool is_tif_valid(TimeInForce) { return true; }

    // Rate throttle hook. Default: disabled (zero overhead on hot path).
    // Override to return true in exchange-derived class to enable per-account
    // message rate limiting via ThrottleConfig in EngineConfig.
    bool is_rate_check_enabled() { return false; }
    bool is_self_match(const Order&, const Order&) { return false; }
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }
    ModifyPolicy get_modify_policy() { return ModifyPolicy::CancelReplace; }
    Quantity get_min_display_qty(const OrderRequest&) { return 0; }

    // Dynamic price band hook.  Default: return static bands from config.
    // Override in derived class to compute bands relative to a reference price
    // (e.g. last trade price, reference price from circuit-breaker logic).
    std::pair<Price, Price> calculate_dynamic_bands(Price /*reference_price*/) {
        return {config_.price_band_low, config_.price_band_high};
    }

    bool should_trigger_stop(Price last_trade, const Order& stop) {
        if (stop.side == Side::Buy) {
            return last_trade >= stop.level->price;
        }
        return last_trade <= stop.level->price;
    }

protected:
    EngineConfig config_;
    SessionState current_state_{SessionState::Continuous};  // backward compatible
    OrderBook book_;
    StopBook stop_book_;
    ObjectPool<Order, MaxOrders> order_pool_;
    ObjectPool<PriceLevel, MaxPriceLevels> level_pool_;
    RateTracker<MaxAccounts> rate_tracker_;
    Price last_trade_price_{0};
    OrderId next_order_id_{1};
    OrderListenerT& order_listener_;
    MdListenerT& md_listener_;
    std::array<Order*, MaxOrderIds> order_index_;

private:
    Derived& derived() { return static_cast<Derived&>(*this); }
    const Derived& derived() const {
        return static_cast<const Derived&>(*this);
    }

    // cancel_active_order -- remove from book/stop-book and fire events
    // Shared by cancel_order(), trigger_expiry().

    void cancel_active_order(Order* order, Timestamp ts,
                             CancelReason reason) {
        Price old_best_bid = book_.best_bid()
                                 ? book_.best_bid()->price : 0;
        Price old_best_ask = book_.best_ask()
                                 ? book_.best_ask()->price : 0;

        bool in_stop_book = (order->type == OrderType::Stop ||
                             order->type == OrderType::StopLimit);
        OrderId oid = order->id;

        if (in_stop_book) {
            PriceLevel* freed = stop_book_.remove_stop(order);
            order_index_[oid] = nullptr;
            order_listener_.on_order_cancelled(
                OrderCancelled{oid, ts, reason});
            if (freed) level_pool_.deallocate(freed);
            order_pool_.deallocate(order);
            return;
        }

        Side order_side = order->side;
        Price order_price = order->price;
        Quantity order_remaining = order->remaining_quantity;
        PriceLevel* freed = book_.remove_order(order);
        order_index_[oid] = nullptr;

        order_listener_.on_order_cancelled(
            OrderCancelled{oid, ts, reason});
        md_listener_.on_order_book_action(OrderBookAction{
            oid, order_side, order_price, order_remaining,
            OrderBookAction::Cancel, ts});
        fire_depth_after_remove(freed, order_side, order_price, ts);
        fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
        order_pool_.deallocate(order);
    }

    // fire_depth_after_remove -- L2 event after book_.remove_order()

    void fire_depth_after_remove(PriceLevel* freed, Side side,
                                 Price price, Timestamp ts) {
        if (freed) {
            md_listener_.on_depth_update(DepthUpdate{
                side, price, 0, 0, DepthUpdate::Remove, ts});
            level_pool_.deallocate(freed);
        } else {
            PriceLevel* lv = find_level_by_price(side, price);
            if (lv) {
                md_listener_.on_depth_update(DepthUpdate{
                    side, price, lv->total_quantity,
                    lv->order_count, DepthUpdate::Update, ts});
            }
        }
    }

    // insert_into_stop_book -- add order to the stop book

    void insert_into_stop_book(Order* order) {
        PriceLevel* new_level = level_pool_.allocate();
        if (new_level == nullptr) {
            // Cancel the order -- no level available
            OrderId oid = order->id;
            Timestamp ots = order->timestamp;
            order_index_[oid] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                oid, ots, CancelReason::LevelPoolExhausted});
            order_pool_.deallocate(order);
            return;
        }
        PriceLevel* used = stop_book_.insert_stop(order, new_level);
        if (used != new_level) {
            level_pool_.deallocate(new_level);
        }
    }

    // match_order -- walk opposite side, match, then handle remainder

    void match_order(Order* order, Timestamp ts) {
        // Snapshot best bid/ask before matching for TopOfBook change detection
        Price old_best_bid = book_.best_bid() ? book_.best_bid()->price : 0;
        Price old_best_ask = book_.best_ask() ? book_.best_ask()->price : 0;
        bool aggressor_alive = true;

        // Determine the opposite side to match against
        Side opposite = (order->side == Side::Buy) ? Side::Sell : Side::Buy;

        // FOK pre-check: verify total available qty before matching
        if (order->tif == TimeInForce::FOK) {
            Quantity available = compute_matchable_qty(order);
            if (available < order->remaining_quantity) {
                OrderId oid = order->id;
                order_index_[oid] = nullptr;
                order_listener_.on_order_cancelled(OrderCancelled{
                    oid, ts, CancelReason::FOKFailed});
                order_pool_.deallocate(order);
                fire_top_of_book_if_changed(
                    ts, old_best_bid, old_best_ask);
                return;
            }
        }

        // Walk matchable price levels on the opposite side
        while (aggressor_alive && order->remaining_quantity > 0) {
            PriceLevel* level = (opposite == Side::Buy)
                                    ? book_.best_bid() : book_.best_ask();
            if (level == nullptr) break;

            // Price check: is this level matchable?
            if (order->type == OrderType::Limit) {
                if (order->side == Side::Buy &&
                    level->price > order->price)
                    break;
                if (order->side == Side::Sell &&
                    level->price < order->price)
                    break;
            }

            // Match against this level
            static constexpr size_t kMaxFills = 256;
            FillResult results[kMaxFills];
            size_t fill_count = 0;
            MatchAlgoT::match(*level, order->remaining_quantity,
                              results, fill_count);

            // Process each fill
            for (size_t i = 0; i < fill_count; ++i) {
                FillResult& fill = results[i];
                Order* resting = fill.resting_order;

                // SMP check -- undo fill and remaining fills if triggered
                if (derived().is_self_match(*order, *resting)) {
                    undo_fills(order, results, i, fill_count);
                    aggressor_alive = apply_smp_action(
                        order, resting, fill, ts);
                    break;
                }

                // Update aggressor filled_quantity
                // (remaining_quantity already decremented by MatchAlgoT)
                order->filled_quantity += fill.quantity;
                last_trade_price_ = fill.price;

                // Update level total_quantity to reflect the fill
                // (MatchAlgoT adjusts order state but not level totals)
                resting->level->total_quantity -= fill.quantity;

                // Determine fill type for callbacks
                bool resting_fully_filled =
                    (fill.resting_remaining == 0);

                // Fire OrderFilled or OrderPartiallyFilled
                if (order->remaining_quantity == 0) {
                    order_listener_.on_order_filled(OrderFilled{
                        order->id, resting->id,
                        fill.price, fill.quantity, ts});
                } else {
                    order_listener_.on_order_partially_filled(
                        OrderPartiallyFilled{
                            order->id, resting->id,
                            fill.price, fill.quantity,
                            order->remaining_quantity,
                            fill.resting_remaining, ts});
                }

                // Fire Trade
                md_listener_.on_trade(Trade{
                    fill.price, fill.quantity,
                    order->id, resting->id,
                    order->side, ts});

                // Fire L3: OrderBookAction for resting order fill
                md_listener_.on_order_book_action(OrderBookAction{
                    resting->id, resting->side, fill.price,
                    fill.quantity, OrderBookAction::Fill, ts});

                // Handle resting order removal/update + L2
                if (resting_fully_filled) {
                    // Check iceberg: visible tranche exhausted but
                    // hidden quantity remains
                    if (resting->display_qty > 0 &&
                        resting->total_qty > resting->filled_quantity) {
                        // Reveal next tranche
                        Quantity next = std::min(
                            resting->display_qty,
                            resting->total_qty - resting->filled_quantity);
                        resting->remaining_quantity = next;

                        // Move to back of queue (loses time priority)
                        PriceLevel* lv = resting->level;
                        list_remove(lv->head, lv->tail, resting);
                        list_push_back(lv->head, lv->tail, resting);
                        lv->total_quantity += next;
                        // order_count unchanged — order stays at level

                        // Fire L3: new tranche appears
                        md_listener_.on_order_book_action(OrderBookAction{
                            resting->id, resting->side,
                            resting->price, next,
                            OrderBookAction::Add, ts});
                        // Fire L2: level updated with revealed qty
                        md_listener_.on_depth_update(DepthUpdate{
                            resting->side, lv->price,
                            lv->total_quantity, lv->order_count,
                            DepthUpdate::Update, ts});
                    } else {
                        // Fully filled (non-iceberg or all tranches
                        // consumed) — remove from book
                        PriceLevel* freed = book_.remove_order(resting);
                        md_listener_.on_depth_update(DepthUpdate{
                            resting->side, fill.price,
                            0, 0, DepthUpdate::Remove, ts});
                        if (freed) {
                            level_pool_.deallocate(freed);
                        }
                        order_index_[resting->id] = nullptr;
                        order_pool_.deallocate(resting);
                    }
                } else {
                    PriceLevel* lv = resting->level;
                    md_listener_.on_depth_update(DepthUpdate{
                        resting->side, lv->price,
                        lv->total_quantity, lv->order_count,
                        DepthUpdate::Update, ts});
                }
            }
        }

        // Post-match: handle remainder based on order type
        if (aggressor_alive) {
            post_match(order, ts, old_best_bid, old_best_ask);
        } else {
            fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
        }
    }

    // undo_fills -- reverse MatchAlgo mutations for fills [from..count)

    void undo_fills(Order* aggressor, FillResult* results,
                    size_t from, size_t count) {
        for (size_t j = from; j < count; ++j) {
            FillResult& f = results[j];
            f.resting_order->filled_quantity -= f.quantity;
            f.resting_order->remaining_quantity += f.quantity;
            aggressor->remaining_quantity += f.quantity;
        }
    }

    // post_match -- handle order remainder after matching

    void post_match(Order* order, Timestamp ts,
                    Price old_best_bid, Price old_best_ask) {
        if (order->remaining_quantity == 0) {
            // Fully filled -- clean up
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
            return;
        }

        if (order->type == OrderType::Market) {
            // Market orders cannot rest -- cancel remainder
            OrderId oid = order->id;
            order_index_[oid] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                oid, ts, CancelReason::IOCRemainder});
            order_pool_.deallocate(order);
            fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
            return;
        }

        // IOC: cancel any remaining quantity
        if (order->tif == TimeInForce::IOC) {
            OrderId oid = order->id;
            order_index_[oid] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                oid, ts, CancelReason::IOCRemainder});
            order_pool_.deallocate(order);
            fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
            return;
        }

        // FOK orders that reach here have a remaining qty -- this
        // should not happen (FOK is checked pre-match), but guard anyway.
        if (order->tif == TimeInForce::FOK) {
            OrderId oid = order->id;
            order_index_[oid] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                oid, ts, CancelReason::FOKFailed});
            order_pool_.deallocate(order);
            fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
            return;
        }

        // Limit order with remaining quantity -- insert into book
        insert_into_book(order, ts);

        // Always fire TopOfBook since the book changed
        fire_top_of_book(ts);
    }

    // insert_into_book -- insert a limit order into the order book

    void insert_into_book(Order* order, Timestamp ts) {
        PriceLevel* new_level = level_pool_.allocate();
        if (new_level == nullptr) {
            OrderId oid = order->id;
            order_index_[oid] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                oid, ts, CancelReason::LevelPoolExhausted});
            order_pool_.deallocate(order);
            return;
        }
        PriceLevel* used = book_.insert_order(order, new_level);
        bool is_new_level = (used == new_level);
        if (!is_new_level) {
            level_pool_.deallocate(new_level);
        }

        // Fire L3: OrderBookAction (Add)
        md_listener_.on_order_book_action(OrderBookAction{
            order->id, order->side, order->price,
            order->remaining_quantity,
            OrderBookAction::Add, ts});

        // Fire L2: DepthUpdate
        PriceLevel* lv = order->level;
        md_listener_.on_depth_update(DepthUpdate{
            order->side, lv->price, lv->total_quantity,
            lv->order_count,
            is_new_level ? DepthUpdate::Add : DepthUpdate::Update, ts});
    }

    // check_and_trigger_stops -- iterative cascade (not recursive)
    // After a trade, triggered stops are converted and matched. Each
    // triggered stop may itself produce trades that trigger more stops.
    // The loop continues until no more stops are triggered.

    void check_and_trigger_stops(Timestamp ts) {
        while (last_trade_price_ > 0 &&
               stop_book_.has_triggered_stops(last_trade_price_)) {
            Order* stop = stop_book_.next_triggered_stop(
                last_trade_price_);
            if (stop == nullptr) break;

            // Remove from stop book
            PriceLevel* freed = stop_book_.remove_stop(stop);
            if (freed) {
                level_pool_.deallocate(freed);
            }

            // Convert triggered stop to Market or Limit
            if (stop->type == OrderType::Stop) {
                stop->type = OrderType::Market;
                stop->price = 0;
            } else {
                // StopLimit: type becomes Limit, price is the limit price
                stop->type = OrderType::Limit;
            }

            // Match the triggered order (may update last_trade_price_)
            match_order(stop, ts);
        }
    }

    // apply_smp_action -- returns false if aggressor was cancelled

    bool apply_smp_action(Order* aggressor, Order* resting,
                          FillResult& fill, Timestamp ts) {
        SmpAction action = derived().get_smp_action();

        switch (action) {
        case SmpAction::CancelNewest: {
            OrderId agg_id = aggressor->id;
            order_index_[agg_id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                agg_id, ts,
                CancelReason::SelfMatchPrevention});
            order_pool_.deallocate(aggressor);
            return false;
        }

        case SmpAction::CancelOldest: {
            Price resting_price = fill.price;
            Side resting_side = resting->side;
            Quantity resting_qty = resting->remaining_quantity;
            OrderId rest_id = resting->id;
            PriceLevel* owning_level = resting->level;
            PriceLevel* freed = book_.remove_order(resting);
            order_index_[rest_id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                rest_id, ts,
                CancelReason::SelfMatchPrevention});
            md_listener_.on_order_book_action(OrderBookAction{
                rest_id, resting_side, resting_price,
                resting_qty, OrderBookAction::Cancel, ts});
            if (freed) {
                md_listener_.on_depth_update(DepthUpdate{
                    resting_side, resting_price,
                    0, 0, DepthUpdate::Remove, ts});
                level_pool_.deallocate(freed);
            } else {
                md_listener_.on_depth_update(DepthUpdate{
                    resting_side, resting_price,
                    owning_level->total_quantity,
                    owning_level->order_count,
                    DepthUpdate::Update, ts});
            }
            order_pool_.deallocate(resting);
            return true;  // aggressor lives
        }

        case SmpAction::CancelBoth: {
            Price resting_price = fill.price;
            Side resting_side = resting->side;
            Quantity resting_qty = resting->remaining_quantity;
            OrderId rest_id = resting->id;
            OrderId agg_id = aggressor->id;
            PriceLevel* freed = book_.remove_order(resting);
            order_index_[rest_id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                rest_id, ts,
                CancelReason::SelfMatchPrevention});
            md_listener_.on_order_book_action(OrderBookAction{
                rest_id, resting_side, resting_price,
                resting_qty, OrderBookAction::Cancel, ts});
            if (freed) {
                md_listener_.on_depth_update(DepthUpdate{
                    resting_side, resting_price,
                    0, 0, DepthUpdate::Remove, ts});
                level_pool_.deallocate(freed);
            }
            order_pool_.deallocate(resting);

            order_index_[agg_id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                agg_id, ts,
                CancelReason::SelfMatchPrevention});
            order_pool_.deallocate(aggressor);
            return false;
        }

        case SmpAction::Decrement:
            // Both lose the fill qty but no actual trade
            // Already undone by undo_fills; both continue
            return true;

        case SmpAction::None:
        default:
            return true;
        }
    }

    // fire_top_of_book_if_changed -- conditional TopOfBook emit

    void fire_top_of_book_if_changed(Timestamp ts,
                                     Price old_best_bid,
                                     Price old_best_ask) {
        Price new_best_bid = book_.best_bid()
                                 ? book_.best_bid()->price : 0;
        Price new_best_ask = book_.best_ask()
                                 ? book_.best_ask()->price : 0;
        if (new_best_bid != old_best_bid ||
            new_best_ask != old_best_ask) {
            fire_top_of_book(ts);
        }
    }

    // fire_top_of_book -- emit TopOfBook with current best bid/ask

    void fire_top_of_book(Timestamp ts) {
        Price best_bid = 0;
        Quantity bid_qty = 0;
        Price best_ask = 0;
        Quantity ask_qty = 0;

        if (book_.best_bid()) {
            best_bid = book_.best_bid()->price;
            bid_qty = book_.best_bid()->total_quantity;
        }
        if (book_.best_ask()) {
            best_ask = book_.best_ask()->price;
            ask_qty = book_.best_ask()->total_quantity;
        }

        md_listener_.on_top_of_book(TopOfBook{
            best_bid, bid_qty, best_ask, ask_qty, ts});
    }

    // compute_matchable_qty -- total qty on opposite side at matchable
    // prices (for FOK pre-check)

    Quantity compute_matchable_qty(const Order* order) const {
        Side opposite = (order->side == Side::Buy)
                            ? Side::Sell : Side::Buy;
        PriceLevel* level = (opposite == Side::Buy)
                                ? book_.best_bid() : book_.best_ask();
        Quantity total = 0;
        while (level != nullptr) {
            if (order->type == OrderType::Limit) {
                if (order->side == Side::Buy &&
                    level->price > order->price)
                    break;
                if (order->side == Side::Sell &&
                    level->price < order->price)
                    break;
            }
            total += level->total_quantity;
            level = level->next;
        }
        return total;
    }

    // auction_handle_filled_order -- handle a fully-consumed visible tranche
    // during auction execution. If the order is an iceberg with hidden qty
    // remaining, reveal the next tranche and move to back of queue. Otherwise
    // remove the order from the book entirely. Returns a pointer to the
    // revealed order (still in book) or nullptr if the order was removed.

    Order* auction_handle_filled_order(Order* order, Timestamp ts) {
        if (order->display_qty > 0 &&
            order->total_qty > order->filled_quantity) {
            // Iceberg: reveal next tranche
            Quantity next = std::min(order->display_qty,
                                     order->total_qty -
                                         order->filled_quantity);
            order->remaining_quantity = next;

            PriceLevel* lv = order->level;
            list_remove(lv->head, lv->tail, order);
            list_push_back(lv->head, lv->tail, order);
            lv->total_quantity += next;

            // Fire L3: new tranche appears
            md_listener_.on_order_book_action(OrderBookAction{
                order->id, order->side, order->price, next,
                OrderBookAction::Add, ts});
            // Fire L2
            md_listener_.on_depth_update(DepthUpdate{
                order->side, lv->price,
                lv->total_quantity, lv->order_count,
                DepthUpdate::Update, ts});
            return order;
        }

        // Fully filled — remove from book
        Side side = order->side;
        Price price = order->price;
        PriceLevel* freed = book_.remove_order(order);
        if (freed) {
            md_listener_.on_depth_update(DepthUpdate{
                side, price, 0, 0, DepthUpdate::Remove, ts});
            level_pool_.deallocate(freed);
        } else {
            PriceLevel* lv = find_level_by_price(side, price);
            if (lv) {
                md_listener_.on_depth_update(DepthUpdate{
                    side, lv->price, lv->total_quantity,
                    lv->order_count, DepthUpdate::Update, ts});
            }
        }
        order_index_[order->id] = nullptr;
        order_pool_.deallocate(order);
        return nullptr;
    }

    // find_level_by_price -- linear search for a price level on a side

    PriceLevel* find_level_by_price(Side side, Price price) const {
        PriceLevel* lv = (side == Side::Buy)
                             ? book_.best_bid() : book_.best_ask();
        while (lv != nullptr) {
            if (lv->price == price) return lv;
            lv = lv->next;
        }
        return nullptr;
    }
};

}  // namespace exchange
