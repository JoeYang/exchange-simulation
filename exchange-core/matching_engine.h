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
    Price tick_size;           // minimum price increment
    Quantity lot_size;         // minimum quantity increment
    Price price_band_low;     // reject orders below this (0 = no band)
    Price price_band_high;    // reject orders above this (0 = no band)
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

    // -----------------------------------------------------------------
    // new_order -- validate, accept, and route an incoming order
    // -----------------------------------------------------------------
    void new_order(const OrderRequest& req) {
        // 1. CRTP validation hook
        if (!derived().on_validate_order(req)) {
            order_listener_.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::ExchangeSpecific});
            return;
        }

        // 2. TIF validation hook
        if (!derived().is_tif_valid(req.tif)) {
            order_listener_.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::InvalidTif});
            return;
        }

        // 3. Quantity must be positive
        if (req.quantity <= 0) {
            order_listener_.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::InvalidQuantity});
            return;
        }

        // 4. Lot size check
        if (config_.lot_size > 0 && (req.quantity % config_.lot_size) != 0) {
            order_listener_.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::InvalidQuantity});
            return;
        }

        // 5. Price validation for Limit and StopLimit orders
        if (req.type == OrderType::Limit || req.type == OrderType::StopLimit) {
            if (req.price <= 0) {
                order_listener_.on_order_rejected(OrderRejected{
                    req.client_order_id, req.timestamp,
                    RejectReason::InvalidPrice});
                return;
            }

            // Tick size check
            if (config_.tick_size > 0 && (req.price % config_.tick_size) != 0) {
                order_listener_.on_order_rejected(OrderRejected{
                    req.client_order_id, req.timestamp,
                    RejectReason::InvalidPrice});
                return;
            }

            // Price band check
            if (config_.price_band_low > 0 && req.price < config_.price_band_low) {
                order_listener_.on_order_rejected(OrderRejected{
                    req.client_order_id, req.timestamp,
                    RejectReason::PriceBandViolation});
                return;
            }
            if (config_.price_band_high > 0 && req.price > config_.price_band_high) {
                order_listener_.on_order_rejected(OrderRejected{
                    req.client_order_id, req.timestamp,
                    RejectReason::PriceBandViolation});
                return;
            }
        }

        // 6. Stop price validation for Stop/StopLimit
        if (req.type == OrderType::Stop || req.type == OrderType::StopLimit) {
            if (req.stop_price <= 0) {
                order_listener_.on_order_rejected(OrderRejected{
                    req.client_order_id, req.timestamp,
                    RejectReason::InvalidPrice});
                return;
            }
        }

        // 7. ID space check
        if (next_order_id_ >= MaxOrderIds) {
            order_listener_.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::PoolExhausted});
            return;
        }

        // 8. Pool check
        Order* order = order_pool_.allocate();
        if (order == nullptr) {
            order_listener_.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::PoolExhausted});
            return;
        }

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
        order->prev               = nullptr;
        order->next               = nullptr;
        order->level              = nullptr;

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

    // -----------------------------------------------------------------
    // cancel_order -- remove order from book/stop book, fire callbacks
    // -----------------------------------------------------------------
    void cancel_order(OrderId id, Timestamp ts) {
        // Validate order ID range
        if (id == 0 || id >= MaxOrderIds || order_index_[id] == nullptr) {
            order_listener_.on_order_cancel_rejected(
                OrderCancelRejected{id, 0, ts, RejectReason::UnknownOrder});
            return;
        }

        Order* order = order_index_[id];
        Price old_best_bid = book_.best_bid()
                                 ? book_.best_bid()->price : 0;
        Price old_best_ask = book_.best_ask()
                                 ? book_.best_ask()->price : 0;

        // Determine if order is in the stop book or the order book
        bool in_stop_book = (order->type == OrderType::Stop ||
                             order->type == OrderType::StopLimit);

        if (in_stop_book) {
            PriceLevel* freed = stop_book_.remove_stop(order);
            order_index_[order->id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                order->id, ts, CancelReason::UserRequested});
            if (freed) {
                level_pool_.deallocate(freed);
            }
            order_pool_.deallocate(order);
            return;
        }

        // Order is in the order book
        Side order_side = order->side;
        Price order_price = order->price;
        Quantity order_remaining = order->remaining_quantity;
        PriceLevel* freed = book_.remove_order(order);

        order_index_[order->id] = nullptr;

        // 1. OrderCancelled
        order_listener_.on_order_cancelled(OrderCancelled{
            id, ts, CancelReason::UserRequested});

        // 2. L3: OrderBookAction (Cancel)
        md_listener_.on_order_book_action(OrderBookAction{
            id, order_side, order_price,
            order_remaining, OrderBookAction::Cancel, ts});

        // 3. L2: DepthUpdate
        if (freed) {
            md_listener_.on_depth_update(DepthUpdate{
                order_side, order_price,
                0, 0, DepthUpdate::Remove, ts});
            level_pool_.deallocate(freed);
        } else {
            // Find the level (order->level is nullptr after remove_order)
            // but the level still exists with remaining orders
            PriceLevel* lv = find_level_by_price(order_side, order_price);
            if (lv) {
                md_listener_.on_depth_update(DepthUpdate{
                    order_side, order_price,
                    lv->total_quantity, lv->order_count,
                    DepthUpdate::Update, ts});
            }
        }

        // 4. L1: TopOfBook if changed
        fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);

        order_pool_.deallocate(order);
    }

    // -----------------------------------------------------------------
    // modify_order -- cancel-replace or amend-down based on policy
    // -----------------------------------------------------------------
    void modify_order(const ModifyRequest& req) {
        // Validate order ID range
        if (req.order_id == 0 || req.order_id >= MaxOrderIds ||
            order_index_[req.order_id] == nullptr) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp,
                                    RejectReason::UnknownOrder});
            return;
        }

        // Validate new quantity
        if (req.new_quantity <= 0) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp,
                                    RejectReason::InvalidQuantity});
            return;
        }

        // Validate new price
        if (req.new_price <= 0) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp,
                                    RejectReason::InvalidPrice});
            return;
        }

        // Tick size validation
        if (config_.tick_size > 0 &&
            (req.new_price % config_.tick_size) != 0) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp,
                                    RejectReason::InvalidPrice});
            return;
        }

        // Lot size validation
        if (config_.lot_size > 0 &&
            (req.new_quantity % config_.lot_size) != 0) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp,
                                    RejectReason::InvalidQuantity});
            return;
        }

        Order* order = order_index_[req.order_id];
        ModifyPolicy policy = derived().get_modify_policy();

        if (policy == ModifyPolicy::RejectModify) {
            order_listener_.on_order_modify_rejected(
                OrderModifyRejected{req.order_id, req.client_order_id,
                                    req.timestamp,
                                    RejectReason::ExchangeSpecific});
            return;
        }

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
        Price old_best_bid = book_.best_bid()
                                 ? book_.best_bid()->price : 0;
        Price old_best_ask = book_.best_ask()
                                 ? book_.best_ask()->price : 0;

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
        if (freed) {
            md_listener_.on_depth_update(DepthUpdate{
                order_side, old_price,
                0, 0, DepthUpdate::Remove, req.timestamp});
            level_pool_.deallocate(freed);
        } else {
            PriceLevel* lv = find_level_by_price(order_side, old_price);
            if (lv) {
                md_listener_.on_depth_update(DepthUpdate{
                    order_side, old_price,
                    lv->total_quantity, lv->order_count,
                    DepthUpdate::Update, req.timestamp});
            }
        }

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

    // -----------------------------------------------------------------
    // trigger_expiry -- stub (Task 16)
    // -----------------------------------------------------------------
    void trigger_expiry(Timestamp /*now*/, TimeInForce /*tif*/) {
        // TODO: Task 16 -- trigger expiry implementation
    }

    // -----------------------------------------------------------------
    // Status queries
    // -----------------------------------------------------------------
    size_t active_order_count() const {
        return MaxOrders - order_pool_.available();
    }

    size_t available_order_slots() const {
        return order_pool_.available();
    }

    size_t available_level_slots() const {
        return level_pool_.available();
    }

    // -----------------------------------------------------------------
    // CRTP hook defaults
    // -----------------------------------------------------------------
    bool on_validate_order(const OrderRequest&) { return true; }
    bool is_tif_valid(TimeInForce) { return true; }
    bool is_self_match(const Order&, const Order&) { return false; }
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }
    ModifyPolicy get_modify_policy() { return ModifyPolicy::CancelReplace; }
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

    // -----------------------------------------------------------------
    // insert_into_stop_book -- add order to the stop book
    // -----------------------------------------------------------------
    void insert_into_stop_book(Order* order) {
        PriceLevel* new_level = level_pool_.allocate();
        if (new_level == nullptr) {
            // Cancel the order -- no level available
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            order_listener_.on_order_cancelled(OrderCancelled{
                order->id, order->timestamp,
                CancelReason::LevelPoolExhausted});
            return;
        }
        PriceLevel* used = stop_book_.insert_stop(order, new_level);
        if (used != new_level) {
            level_pool_.deallocate(new_level);
        }
    }

    // -----------------------------------------------------------------
    // match_order -- walk opposite side, match, then handle remainder
    // -----------------------------------------------------------------
    void match_order(Order* order, Timestamp ts) {
        // Snapshot best bid/ask before matching for TopOfBook change detection
        Price old_best_bid = book_.best_bid() ? book_.best_bid()->price : 0;
        Price old_best_ask = book_.best_ask() ? book_.best_ask()->price : 0;
        bool aggressor_alive = true;

        // Determine the opposite side to match against
        Side opposite = (order->side == Side::Buy) ? Side::Sell : Side::Buy;

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

    // -----------------------------------------------------------------
    // undo_fills -- reverse MatchAlgo mutations for fills [from..count)
    // -----------------------------------------------------------------
    void undo_fills(Order* aggressor, FillResult* results,
                    size_t from, size_t count) {
        for (size_t j = from; j < count; ++j) {
            FillResult& f = results[j];
            f.resting_order->filled_quantity -= f.quantity;
            f.resting_order->remaining_quantity += f.quantity;
            aggressor->remaining_quantity += f.quantity;
        }
    }

    // -----------------------------------------------------------------
    // post_match -- handle order remainder after matching
    // -----------------------------------------------------------------
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
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            order_listener_.on_order_cancelled(OrderCancelled{
                order->id, ts, CancelReason::IOCRemainder});
            fire_top_of_book_if_changed(ts, old_best_bid, old_best_ask);
            return;
        }

        // Limit order with remaining quantity -- insert into book
        insert_into_book(order, ts);

        // Always fire TopOfBook since the book changed
        fire_top_of_book(ts);
    }

    // -----------------------------------------------------------------
    // insert_into_book -- insert a limit order into the order book
    // -----------------------------------------------------------------
    void insert_into_book(Order* order, Timestamp ts) {
        PriceLevel* new_level = level_pool_.allocate();
        if (new_level == nullptr) {
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            order_listener_.on_order_cancelled(OrderCancelled{
                order->id, ts, CancelReason::LevelPoolExhausted});
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

    // -----------------------------------------------------------------
    // check_and_trigger_stops -- iterative cascade (not recursive)
    // After a trade, triggered stops are converted and matched. Each
    // triggered stop may itself produce trades that trigger more stops.
    // The loop continues until no more stops are triggered.
    // -----------------------------------------------------------------
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

    // -----------------------------------------------------------------
    // apply_smp_action -- returns false if aggressor was cancelled
    // -----------------------------------------------------------------
    bool apply_smp_action(Order* aggressor, Order* resting,
                          FillResult& fill, Timestamp ts) {
        SmpAction action = derived().get_smp_action();

        switch (action) {
        case SmpAction::CancelNewest:
            order_index_[aggressor->id] = nullptr;
            order_pool_.deallocate(aggressor);
            order_listener_.on_order_cancelled(OrderCancelled{
                aggressor->id, ts,
                CancelReason::SelfMatchPrevention});
            return false;

        case SmpAction::CancelOldest: {
            Price resting_price = fill.price;
            Side resting_side = resting->side;
            Quantity resting_qty = resting->remaining_quantity;
            PriceLevel* owning_level = resting->level;
            PriceLevel* freed = book_.remove_order(resting);
            order_index_[resting->id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                resting->id, ts,
                CancelReason::SelfMatchPrevention});
            md_listener_.on_order_book_action(OrderBookAction{
                resting->id, resting_side, resting_price,
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
            PriceLevel* freed = book_.remove_order(resting);
            order_index_[resting->id] = nullptr;
            order_listener_.on_order_cancelled(OrderCancelled{
                resting->id, ts,
                CancelReason::SelfMatchPrevention});
            md_listener_.on_order_book_action(OrderBookAction{
                resting->id, resting_side, resting_price,
                resting_qty, OrderBookAction::Cancel, ts});
            if (freed) {
                md_listener_.on_depth_update(DepthUpdate{
                    resting_side, resting_price,
                    0, 0, DepthUpdate::Remove, ts});
                level_pool_.deallocate(freed);
            }
            order_pool_.deallocate(resting);

            order_index_[aggressor->id] = nullptr;
            order_pool_.deallocate(aggressor);
            order_listener_.on_order_cancelled(OrderCancelled{
                aggressor->id, ts,
                CancelReason::SelfMatchPrevention});
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

    // -----------------------------------------------------------------
    // fire_top_of_book_if_changed -- conditional TopOfBook emit
    // -----------------------------------------------------------------
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

    // -----------------------------------------------------------------
    // fire_top_of_book -- emit TopOfBook with current best bid/ask
    // -----------------------------------------------------------------
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

    // -----------------------------------------------------------------
    // find_level_by_price -- linear search for a price level on a side
    // -----------------------------------------------------------------
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
