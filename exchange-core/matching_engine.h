#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "exchange-core/events.h"
#include "exchange-core/intrusive_list.h"
#include "exchange-core/listeners.h"
#include "exchange-core/match_algo.h"
#include "exchange-core/object_pool.h"
#include "exchange-core/orderbook.h"
#include "exchange-core/stop_book.h"
#include "exchange-core/types.h"

namespace exchange {

// --- Engine configuration ---

struct EngineConfig {
    Price tick_size;              // minimum price increment
    Quantity lot_size;            // minimum quantity increment
    Price price_band_low;         // reject orders below this (0 = no band)
    Price price_band_high;        // reject orders above this (0 = no band)
    Quantity max_order_size{0};   // maximum single-order quantity (0 = no limit)
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

// --- Matching Engine ---

template <
    typename Derived,
    typename OrderListenerT,
    typename MdListenerT,
    typename MatchAlgoT = FifoMatch,
    size_t MaxOrders = 100000,
    size_t MaxPriceLevels = 10000,
    size_t MaxOrderIds = 1000000
>
class MatchingEngine {
public:
    MatchingEngine(EngineConfig config,
                   OrderListenerT& order_listener,
                   MdListenerT& md_listener)
        : config_(config)
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

        // 12. Match the order and trigger stop cascade
        match_order(order, req.timestamp);
        check_and_trigger_stops(req.timestamp);
    }

    // cancel_order -- remove order from book/stop book, fire callbacks

    void cancel_order(OrderId id, Timestamp ts) {
        if (id == 0 || id >= MaxOrderIds || order_index_[id] == nullptr) {
            order_listener_.on_order_cancel_rejected(
                OrderCancelRejected{id, 0, ts, RejectReason::UnknownOrder});
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

        if (req.order_id == 0 || req.order_id >= MaxOrderIds ||
            order_index_[req.order_id] == nullptr)
            { mod_reject(RejectReason::UnknownOrder); return; }

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

    bool on_validate_order(const OrderRequest&) { return true; }
    bool is_tif_valid(TimeInForce) { return true; }
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
    OrderBook book_;
    StopBook stop_book_;
    ObjectPool<Order, MaxOrders> order_pool_;
    ObjectPool<PriceLevel, MaxPriceLevels> level_pool_;
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
                    PriceLevel* freed = book_.remove_order(resting);
                    md_listener_.on_depth_update(DepthUpdate{
                        resting->side, fill.price,
                        0, 0, DepthUpdate::Remove, ts});
                    if (freed) {
                        level_pool_.deallocate(freed);
                    }
                    order_index_[resting->id] = nullptr;
                    order_pool_.deallocate(resting);
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
