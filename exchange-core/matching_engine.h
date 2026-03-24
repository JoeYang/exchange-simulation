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

        // 12. Match the order
        match_order(order, req.timestamp);
    }

    // -----------------------------------------------------------------
    // cancel_order -- stub (Task 15)
    // -----------------------------------------------------------------
    void cancel_order(OrderId /*id*/, Timestamp /*ts*/) {
        // TODO: Task 15 -- cancel order implementation
    }

    // -----------------------------------------------------------------
    // modify_order -- stub (Task 15)
    // -----------------------------------------------------------------
    void modify_order(const ModifyRequest& /*req*/) {
        // TODO: Task 15 -- modify order implementation
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
    // match_order -- stub for Task 12, implemented in Task 13
    // -----------------------------------------------------------------
    void match_order(Order* order, Timestamp ts) {
        // Task 13 will implement full matching logic.
        // For now, limit orders rest on the book; market orders are
        // cancelled since there is nothing to match against yet.
        if (order->type == OrderType::Market) {
            // Market orders cannot rest -- cancel remainder
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            order_listener_.on_order_cancelled(OrderCancelled{
                order->id, ts, CancelReason::IOCRemainder});
            return;
        }

        // Limit order: insert into book
        PriceLevel* new_level = level_pool_.allocate();
        if (new_level == nullptr) {
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            order_listener_.on_order_cancelled(OrderCancelled{
                order->id, ts, CancelReason::LevelPoolExhausted});
            return;
        }
        PriceLevel* used = book_.insert_order(order, new_level);
        if (used != new_level) {
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
            lv->order_count, DepthUpdate::Add, ts});

        // Fire L1: TopOfBook
        fire_top_of_book(ts);
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
};

}  // namespace exchange
