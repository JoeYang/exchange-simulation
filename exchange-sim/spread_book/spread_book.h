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

    // --- Implied-out matching ---
    //
    // Called when an outright instrument's BBO changes. Recomputes implied
    // spread bid/ask from outright BBOs and matches against resting spread
    // orders. If a spread order crosses the implied price, we execute:
    //   1. Fill the spread order (resting in spread book).
    //   2. Atomically fill the outright legs via apply_implied_fills().
    //
    // Returns the number of spread fills executed.

    template <typename OrderListenerT>
    int on_outright_bbo_change(OrderListenerT& listener, Timestamp ts) {
        if (!bbo_provider_ || !best_order_provider_ || !fill_applier_)
            return 0;

        const auto& legs = strategy_.legs();
        auto bbos = gather_leg_bbos();
        if (bbos.size() != legs.size()) return 0;

        int fills_executed = 0;

        // Try implied-out bid (synthetic spread bid) vs resting spread asks.
        fills_executed += match_implied_out_side(
            Side::Buy, legs, bbos, listener, ts);

        // Refresh BBOs after potential fills changed outright books.
        if (fills_executed > 0) bbos = gather_leg_bbos();

        // Try implied-out ask (synthetic spread ask) vs resting spread bids.
        fills_executed += match_implied_out_side(
            Side::Sell, legs, bbos, listener, ts);

        return fills_executed;
    }

    // --- Implied-in matching ---
    //
    // Called when a new spread order arrives (after resting) or when outright
    // BBOs change. For each resting spread order, compute the implied outright
    // price for each leg and attempt atomic fills.
    // Implemented in T16.

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

    // Match implied-out on one side: compute synthetic spread price from
    // outright BBOs and match against resting spread orders on the opposite side.
    //
    // implied_side = Side::Buy means we compute the implied spread BID
    //   (which crosses resting spread ASKs).
    // implied_side = Side::Sell means we compute the implied spread ASK
    //   (which crosses resting spread BIDs).
    //
    // For each crossing spread order, we:
    //   1. Determine fill qty (min of implied qty and spread order remaining).
    //   2. Build LegFill batch for outright engines.
    //   3. Apply implied fills atomically.
    //   4. If successful, fill the spread order.
    template <typename OrderListenerT>
    int match_implied_out_side(
        Side implied_side,
        const std::vector<StrategyLeg>& legs,
        std::vector<LegBBO>& bbos,
        OrderListenerT& listener,
        Timestamp ts)
    {
        int fills = 0;

        while (true) {
            // Compute implied level.
            auto implied = (implied_side == Side::Buy)
                ? ImpliedPriceEngine::compute_implied_out_bid(
                      legs, bbos, strategy_.tick_size())
                : ImpliedPriceEngine::compute_implied_out_ask(
                      legs, bbos, strategy_.tick_size());
            if (!implied) break;

            // Find resting spread orders on the opposite side.
            // Implied bid crosses resting asks; implied ask crosses resting bids.
            PriceLevel* lv = (implied_side == Side::Buy)
                ? book_.best_ask() : book_.best_bid();
            if (!lv) break;

            // Check crossing: implied bid >= resting ask, or
            //                  implied ask <= resting bid.
            bool does_cross = (implied_side == Side::Buy)
                ? implied->price >= lv->price
                : implied->price <= lv->price;
            if (!does_cross) break;

            Order* spread_order = lv->head;
            if (!spread_order) break;

            // Determine fill quantity.
            Quantity fill_qty = std::min(implied->quantity,
                                         spread_order->remaining_quantity);
            // Align to spread lot size.
            if (strategy_.lot_size() > 0) {
                fill_qty = (fill_qty / strategy_.lot_size()) * strategy_.lot_size();
            }
            if (fill_qty <= 0) break;

            // Build leg fills: for each outright leg, get the best resting
            // order on the execution side and build a LegFill.
            bool leg_fills_ok = true;
            std::vector<std::pair<uint32_t, LegFill>> leg_fill_batch;
            leg_fill_batch.reserve(legs.size());

            for (size_t i = 0; i < legs.size(); ++i) {
                const auto& leg = legs[i];
                // For implied-out bid: we execute buy-ratio legs as BUYS
                // and sell-ratio legs as SELLS on the outright.
                // The execution side for each leg:
                Side leg_exec_side = (implied_side == Side::Buy)
                    ? (leg.ratio > 0 ? Side::Buy : Side::Sell)
                    : (leg.ratio > 0 ? Side::Sell : Side::Buy);
                // We need the opposite side's resting order.
                Side resting_side = (leg_exec_side == Side::Buy)
                    ? Side::Sell : Side::Buy;

                auto best_id = best_order_provider_(leg.instrument_id,
                                                     resting_side);
                if (!best_id) { leg_fills_ok = false; break; }

                Quantity leg_qty = strategy_.leg_quantity(i, fill_qty);
                // Execution price from BBO.
                Price leg_price = (resting_side == Side::Buy)
                    ? bbos[i].bid_price : bbos[i].ask_price;

                leg_fill_batch.push_back({leg.instrument_id,
                    LegFill{*best_id, leg_price, leg_qty}});
            }

            if (!leg_fills_ok) break;

            // Apply outright fills atomically, one engine at a time.
            bool all_applied = true;
            for (const auto& [instr_id, fill] : leg_fill_batch) {
                std::array<LegFill, 1> single = {fill};
                if (!fill_applier_(instr_id, single, ts)) {
                    all_applied = false;
                    break;
                }
            }

            if (!all_applied) break;

            // Outright fills succeeded -- fill the spread order.
            Price spread_fill_price = spread_order->price;
            spread_order->filled_quantity += fill_qty;
            spread_order->remaining_quantity -= fill_qty;
            lv->total_quantity -= fill_qty;

            if (spread_order->remaining_quantity == 0) {
                listener.on_order_filled(OrderFilled{
                    spread_order->id, spread_order->id,
                    spread_fill_price, fill_qty, ts});
                PriceLevel* freed = book_.remove_order(spread_order);
                if (freed) level_pool_.deallocate(freed);
                order_index_[spread_order->id] = nullptr;
                order_pool_.deallocate(spread_order);
            } else {
                listener.on_order_partially_filled(
                    OrderPartiallyFilled{
                        spread_order->id, spread_order->id,
                        spread_fill_price, fill_qty,
                        spread_order->remaining_quantity,
                        spread_order->remaining_quantity, ts});
            }

            ++fills;

            // Refresh BBOs for next iteration (outright books changed).
            bbos = gather_leg_bbos();
        }

        return fills;
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
