#pragma once

#include "exchange-core/events.h"
#include "exchange-core/listeners.h"
#include "exchange-core/object_pool.h"
#include "exchange-core/orderbook.h"
#include "exchange-core/types.h"
#include "exchange-sim/spread_book/implied_price_engine.h"
#include "exchange-sim/spread_book/spread_strategy.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace exchange {

// --- Spread order request ---

struct SpreadOrderRequest {
    uint64_t client_order_id{0};
    uint64_t account_id{0};
    Side     side{Side::Buy};
    TimeInForce tif{TimeInForce::GTC};
    Price    price{0};          // spread price (may be negative for calendars)
    Quantity quantity{0};       // in spread-lots (fixed-point)
    Timestamp timestamp{0};
};

struct SpreadCancelRequest {
    OrderId   order_id{0};
    Timestamp timestamp{0};
};

struct SpreadModifyRequest {
    OrderId   order_id{0};
    uint64_t  client_order_id{0};
    Price     new_price{0};
    Quantity  new_quantity{0};
    Timestamp timestamp{0};
};

// --- Callback types ---

// Retrieves the LegBBO for a given outright instrument ID.
using LegBBOProvider = std::function<LegBBO(uint32_t instrument_id)>;

// Retrieves the best order ID for a given instrument + side.
using BestOrderIdProvider =
    std::function<std::optional<OrderId>(uint32_t instrument_id, Side side)>;

// Applies implied fills atomically to an outright engine.
// Returns true if all fills succeeded.
using ImpliedFillApplier =
    std::function<bool(uint32_t instrument_id,
                       std::span<const LegFill> fills, Timestamp ts)>;

// --- SpreadBook ---
//
// Manages the spread order book for a single spread instrument.
// Handles order entry, cancellation, and modification.
//
// The SpreadBook owns an OrderBook for spread orders and delegates to
// outright MatchingEngines via callbacks (LegBBOProvider, BestOrderIdProvider,
// ImpliedFillApplier) to avoid template coupling.
//
// Single-threaded: all operations are called from the same thread as the
// outright engines.
//
// Template parameters control pool sizes for the spread book:
//   MaxSpreadOrders:  order pool capacity
//   MaxSpreadLevels:  price level pool capacity
//   MaxSpreadOrderIds: order index array size
template <
    size_t MaxSpreadOrders = 10000,
    size_t MaxSpreadLevels = 1000,
    size_t MaxSpreadOrderIds = 100000
>
class SpreadBook {
public:
    SpreadBook(const SpreadStrategy& strategy,
               uint32_t spread_instrument_id)
        : strategy_(strategy)
        , spread_instrument_id_(spread_instrument_id) {
        order_index_.fill(nullptr);
    }

    // Non-copyable, non-movable.
    SpreadBook(const SpreadBook&) = delete;
    SpreadBook& operator=(const SpreadBook&) = delete;
    SpreadBook(SpreadBook&&) = delete;
    SpreadBook& operator=(SpreadBook&&) = delete;

    // --- Callback registration ---

    void set_bbo_provider(LegBBOProvider provider) {
        bbo_provider_ = std::move(provider);
    }
    void set_best_order_provider(BestOrderIdProvider provider) {
        best_order_provider_ = std::move(provider);
    }
    void set_fill_applier(ImpliedFillApplier applier) {
        fill_applier_ = std::move(applier);
    }

    // --- Order entry ---
    //
    // Returns the assigned spread OrderId, or 0 if rejected.
    // Validates: quantity alignment, tick alignment, pool capacity.
    // For GTC/DAY: rests in the spread book.
    // For IOC: attempts immediate match, cancels remainder.
    // For FOK: attempts full match, rejects if not possible.

    template <typename OrderListenerT>
    OrderId new_order(const SpreadOrderRequest& req, OrderListenerT& listener) {
        // Validate quantity: must be positive, multiple of spread lot_size
        if (req.quantity <= 0) {
            listener.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::InvalidQuantity});
            return 0;
        }
        if (strategy_.lot_size() > 0 &&
            (req.quantity % strategy_.lot_size()) != 0) {
            listener.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::InvalidQuantity});
            return 0;
        }

        // Validate price: must be multiple of spread tick_size.
        // Spread prices can be negative (calendar), zero (butterfly), or positive.
        // We validate tick alignment on the absolute value.
        if (strategy_.tick_size() > 0 && req.price != 0) {
            Price abs_price = req.price >= 0 ? req.price : -req.price;
            if ((abs_price % strategy_.tick_size()) != 0) {
                listener.on_order_rejected(OrderRejected{
                    req.client_order_id, req.timestamp,
                    RejectReason::InvalidPrice});
                return 0;
            }
        }

        // Validate TIF
        if (req.tif != TimeInForce::GTC && req.tif != TimeInForce::DAY &&
            req.tif != TimeInForce::IOC && req.tif != TimeInForce::FOK) {
            listener.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::InvalidTif});
            return 0;
        }

        // Allocate order
        Order* order = order_pool_.allocate();
        if (!order) {
            listener.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::PoolExhausted});
            return 0;
        }

        OrderId id = next_order_id_++;
        if (id >= MaxSpreadOrderIds) {
            order_pool_.deallocate(order);
            listener.on_order_rejected(OrderRejected{
                req.client_order_id, req.timestamp,
                RejectReason::PoolExhausted});
            return 0;
        }

        *order = Order{};
        order->id = id;
        order->client_order_id = req.client_order_id;
        order->account_id = req.account_id;
        order->price = req.price;
        order->quantity = req.quantity;
        order->remaining_quantity = req.quantity;
        order->side = req.side;
        order->type = OrderType::Limit;
        order->tif = req.tif;
        order->timestamp = req.timestamp;

        order_index_[id] = order;

        listener.on_order_accepted(OrderAccepted{id, req.client_order_id,
                                                  req.timestamp});

        // Attempt direct spread-vs-spread matching for all TIFs.
        // GTC/DAY: match what's available, rest remainder.
        // IOC: match what's available, cancel remainder.
        // FOK: match all or nothing.
        if (req.tif == TimeInForce::FOK) {
            // Check if full quantity is available before matching.
            Quantity available = available_crossing_qty(order);
            if (available < order->remaining_quantity) {
                // Cannot fill entirely -- reject FOK.
                remove_order_no_book(order, listener, CancelReason::FOKFailed,
                                     req.timestamp);
                return id;
            }
        }

        match_direct(order, listener, req.timestamp);

        if (order->remaining_quantity > 0) {
            if (req.tif == TimeInForce::IOC) {
                remove_order_no_book(order, listener, CancelReason::IOCRemainder,
                                     req.timestamp);
            } else {
                // GTC/DAY: rest remainder
                rest_order(order);
            }
        } else {
            // Fully filled -- clean up
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
        }

        return id;
    }

    // --- Cancel ---

    template <typename OrderListenerT>
    bool cancel_order(const SpreadCancelRequest& req,
                      OrderListenerT& listener) {
        if (req.order_id == 0 || req.order_id >= MaxSpreadOrderIds)
            return false;
        Order* order = order_index_[req.order_id];
        if (!order) {
            listener.on_order_cancel_rejected(OrderCancelRejected{
                req.order_id, 0, req.timestamp, RejectReason::UnknownOrder});
            return false;
        }

        remove_order(order, listener, CancelReason::UserRequested,
                     req.timestamp);
        return true;
    }

    // --- Modify (cancel-replace) ---
    // Implemented in T14.

    template <typename OrderListenerT>
    bool modify_order(const SpreadModifyRequest& req,
                      OrderListenerT& listener) {
        if (req.order_id == 0 || req.order_id >= MaxSpreadOrderIds)
            return false;
        Order* order = order_index_[req.order_id];
        if (!order) {
            listener.on_order_modify_rejected(OrderModifyRejected{
                req.order_id, req.client_order_id, req.timestamp,
                RejectReason::UnknownOrder});
            return false;
        }

        // Validate new price tick alignment
        if (strategy_.tick_size() > 0 && req.new_price != 0) {
            Price abs_price = req.new_price >= 0 ? req.new_price : -req.new_price;
            if ((abs_price % strategy_.tick_size()) != 0) {
                listener.on_order_modify_rejected(OrderModifyRejected{
                    req.order_id, req.client_order_id, req.timestamp,
                    RejectReason::InvalidPrice});
                return false;
            }
        }

        // Validate new quantity
        if (req.new_quantity <= 0 ||
            (strategy_.lot_size() > 0 &&
             (req.new_quantity % strategy_.lot_size()) != 0)) {
            listener.on_order_modify_rejected(OrderModifyRejected{
                req.order_id, req.client_order_id, req.timestamp,
                RejectReason::InvalidQuantity});
            return false;
        }

        // Cancel-replace: remove from book, update fields, re-insert
        PriceLevel* freed = book_.remove_order(order);
        if (freed) level_pool_.deallocate(freed);

        order->price = req.new_price;
        order->quantity = req.new_quantity;
        order->remaining_quantity = req.new_quantity - order->filled_quantity;
        if (order->remaining_quantity <= 0) {
            // New quantity <= already filled: fully done
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            listener.on_order_cancelled(OrderCancelled{
                req.order_id, req.timestamp, CancelReason::UserRequested});
            return true;
        }

        order->timestamp = req.timestamp;  // loses time priority (cancel-replace)
        rest_order(order);

        listener.on_order_modified(OrderModified{
            order->id, req.client_order_id,
            req.new_price, req.new_quantity, req.timestamp});
        return true;
    }

    // --- Accessors ---

    const SpreadStrategy& strategy() const { return strategy_; }
    uint32_t spread_instrument_id() const { return spread_instrument_id_; }

    const OrderBook& book() const { return book_; }

    const Order* get_order(OrderId id) const {
        if (id == 0 || id >= MaxSpreadOrderIds) return nullptr;
        return order_index_[id];
    }

    // Gather current outright BBOs for all legs via the bbo_provider_.
    // Returns empty vector if bbo_provider_ is not set.
    std::vector<LegBBO> gather_leg_bbos() const {
        if (!bbo_provider_) return {};
        std::vector<LegBBO> bbos;
        bbos.reserve(strategy_.legs().size());
        for (const auto& leg : strategy_.legs()) {
            bbos.push_back(bbo_provider_(leg.instrument_id));
        }
        return bbos;
    }

private:
    // Insert order into spread book at its price level.
    void rest_order(Order* order) {
        PriceLevel* new_level = level_pool_.allocate();
        if (!new_level) {
            // Level pool exhausted -- should not happen in practice
            // but handle gracefully by removing the order
            order_index_[order->id] = nullptr;
            order_pool_.deallocate(order);
            return;
        }
        book_.insert_order(order, new_level);
        if (order->level != new_level) {
            // Level already existed -- return unused allocation
            level_pool_.deallocate(new_level);
        }
    }

    // Remove order from book, emit cancel event, deallocate.
    template <typename OrderListenerT>
    void remove_order(Order* order, OrderListenerT& listener,
                      CancelReason reason, Timestamp ts) {
        PriceLevel* freed = book_.remove_order(order);
        if (freed) level_pool_.deallocate(freed);

        order_index_[order->id] = nullptr;
        listener.on_order_cancelled(OrderCancelled{order->id, ts, reason});
        order_pool_.deallocate(order);
    }

    // Remove order that is NOT in the book (pre-resting cancel for IOC/FOK).
    template <typename OrderListenerT>
    void remove_order_no_book(Order* order, OrderListenerT& listener,
                              CancelReason reason, Timestamp ts) {
        order_index_[order->id] = nullptr;
        listener.on_order_cancelled(OrderCancelled{order->id, ts, reason});
        order_pool_.deallocate(order);
    }

    // Check if aggressor order's price crosses a resting order's price.
    static bool crosses(Side aggressor_side, Price aggressor_price,
                        Price resting_price) {
        // Buy aggressor crosses if resting ask <= aggressor price.
        // Sell aggressor crosses if resting bid >= aggressor price.
        return (aggressor_side == Side::Buy)
            ? resting_price <= aggressor_price
            : resting_price >= aggressor_price;
    }

    // Calculate total available crossing quantity on the opposite side.
    // Used for FOK feasibility check.
    Quantity available_crossing_qty(const Order* aggressor) const {
        const Side opp = (aggressor->side == Side::Buy) ? Side::Sell : Side::Buy;
        const PriceLevel* lv = (opp == Side::Buy) ? book_.best_bid()
                                                   : book_.best_ask();
        Quantity total = 0;
        while (lv && crosses(aggressor->side, aggressor->price, lv->price)) {
            total += lv->total_quantity;
            if (total >= aggressor->remaining_quantity) return total;
            lv = lv->next;
        }
        return total;
    }

    // Direct spread-vs-spread FIFO matching.
    // Matches the aggressor against resting orders on the opposite side.
    // Emits one fill event per trade (matches engine convention).
    template <typename OrderListenerT>
    void match_direct(Order* aggressor, OrderListenerT& listener, Timestamp ts) {
        while (aggressor->remaining_quantity > 0) {
            // Get best opposite level.
            PriceLevel* lv = (aggressor->side == Side::Buy)
                ? book_.best_ask() : book_.best_bid();
            if (!lv) break;
            if (!crosses(aggressor->side, aggressor->price, lv->price)) break;

            Order* resting = lv->head;
            while (resting && aggressor->remaining_quantity > 0) {
                Quantity fill_qty = std::min(aggressor->remaining_quantity,
                                             resting->remaining_quantity);
                Price fill_price = resting->price;

                aggressor->filled_quantity += fill_qty;
                aggressor->remaining_quantity -= fill_qty;
                resting->filled_quantity += fill_qty;
                resting->remaining_quantity -= fill_qty;
                lv->total_quantity -= fill_qty;

                // Single event per fill: fully filled or partial.
                bool both_done = (aggressor->remaining_quantity == 0 &&
                                  resting->remaining_quantity == 0);
                if (both_done) {
                    listener.on_order_filled(OrderFilled{
                        aggressor->id, resting->id,
                        fill_price, fill_qty, ts});
                } else {
                    listener.on_order_partially_filled(
                        OrderPartiallyFilled{
                            aggressor->id, resting->id,
                            fill_price, fill_qty,
                            aggressor->remaining_quantity,
                            resting->remaining_quantity, ts});
                }

                // Clean up fully filled resting order.
                Order* next = resting->next;
                if (resting->remaining_quantity == 0) {
                    PriceLevel* freed = book_.remove_order(resting);
                    if (freed) level_pool_.deallocate(freed);
                    order_index_[resting->id] = nullptr;
                    order_pool_.deallocate(resting);
                }
                resting = next;
            }
        }
    }

    const SpreadStrategy& strategy_;
    uint32_t spread_instrument_id_{0};

    OrderBook book_;
    ObjectPool<Order, MaxSpreadOrders> order_pool_;
    ObjectPool<PriceLevel, MaxSpreadLevels> level_pool_;
    std::array<Order*, MaxSpreadOrderIds> order_index_;
    OrderId next_order_id_{1};

    // Callbacks to outright engines (set during initialization)
    LegBBOProvider bbo_provider_;
    BestOrderIdProvider best_order_provider_;
    ImpliedFillApplier fill_applier_;
};

}  // namespace exchange
