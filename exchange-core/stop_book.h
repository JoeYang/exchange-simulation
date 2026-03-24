#pragma once

#include "exchange-core/intrusive_list.h"
#include "exchange-core/types.h"

namespace exchange {

// StopBook manages stop orders for one instrument.
//
// Stop orders are kept in two separate sorted intrusive doubly-linked lists of
// PriceLevel nodes — one per side:
//
//   Buy stops  — sorted ASCENDING  (lowest stop price = head)
//     Triggered when last_trade_price >= stop_price.
//     The lowest-priced level triggers first.
//
//   Sell stops — sorted DESCENDING (highest stop price = head)
//     Triggered when last_trade_price <= stop_price.
//     The highest-priced level triggers first.
//
// Each PriceLevel holds an intrusive FIFO list of Order nodes.
//
// Allocation policy (mirrors OrderBook):
//   insert_stop: caller supplies a pre-allocated PriceLevel. If a level at
//                that price already exists the supplied node is not consumed;
//                the caller is responsible for deallocating it.
//   remove_stop: if the last order is removed, the now-empty level is unlinked
//                and returned to the caller for deallocation. Returns nullptr
//                when the level still has remaining orders.
class StopBook {
public:
    StopBook() = default;

    // Non-copyable, non-movable — owns pointer state to live nodes.
    StopBook(const StopBook&)            = delete;
    StopBook& operator=(const StopBook&) = delete;
    StopBook(StopBook&&)                 = delete;
    StopBook& operator=(StopBook&&)      = delete;

    // Insert a stop order into the appropriate side (determined by order->side).
    //
    // If a PriceLevel for order->price already exists on that side,
    // new_level_if_needed is not used — the caller must deallocate it.
    // The existing level is returned.
    //
    // If no level exists, new_level_if_needed is initialised and linked into
    // the correct sorted position. It is returned as the owning level.
    //
    // Sets order->level to the owning PriceLevel on success.
    PriceLevel* insert_stop(Order* order, PriceLevel* new_level_if_needed);

    // Remove a stop order (e.g. on cancel). order->level must be set.
    //
    // Updates level totals and unlinks the order. If the level becomes empty
    // it is unlinked and returned for deallocation. Returns nullptr otherwise.
    PriceLevel* remove_stop(Order* order);

    // Returns true when at least one stop would be triggered by last_trade_price.
    //   Buy  stop triggers if buy_stops_->price  <= last_trade_price
    //   Sell stop triggers if sell_stops_->price >= last_trade_price
    bool has_triggered_stops(Price last_trade_price) const noexcept;

    // Returns the head order of the best-priority triggered stop level, or
    // nullptr if no stops are triggered at last_trade_price.
    //
    // For buy  stops: best priority = lowest  stop price (head of ascending list)
    // For sell stops: best priority = highest stop price (head of descending list)
    //
    // When both sides trigger simultaneously, buy stops are returned first
    // (implementation-defined tie-break; the engine drives the cascade loop).
    //
    // The caller is responsible for removing the order via remove_stop() and
    // converting it to a Market or Limit order before re-submitting.
    Order* next_triggered_stop(Price last_trade_price) const noexcept;

    PriceLevel* buy_stops()  const noexcept { return buy_stops_; }
    PriceLevel* sell_stops() const noexcept { return sell_stops_; }
    bool        empty()      const noexcept { return !buy_stops_ && !sell_stops_; }

private:
    // Find an existing PriceLevel at the given price on the given side.
    PriceLevel* find_level(Side side, Price price) const noexcept;

    // Link a new, initialised PriceLevel into the correct sorted position.
    void link_level(Side side, PriceLevel* lv) noexcept;

    // Unlink a PriceLevel from its side list.
    void unlink_level(Side side, PriceLevel* lv) noexcept;

    // Buy stops: head = lowest price (triggers first), tail = highest price.
    PriceLevel* buy_stops_{nullptr};
    PriceLevel* buy_stops_tail_{nullptr};

    // Sell stops: head = highest price (triggers first), tail = lowest price.
    PriceLevel* sell_stops_{nullptr};
    PriceLevel* sell_stops_tail_{nullptr};
};

}  // namespace exchange
