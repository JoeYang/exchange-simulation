#pragma once

#include "exchange-core/intrusive_list.h"
#include "exchange-core/types.h"

namespace exchange {

// OrderBook manages the bid and ask sides of an instrument.
//
// Each side is an intrusive doubly-linked list of PriceLevel nodes:
//   Bids — sorted descending  (best_bid_ = highest price)
//   Asks — sorted ascending   (best_ask_ = lowest price)
//
// Each PriceLevel contains an intrusive doubly-linked list of Order nodes
// in FIFO (time-priority) order.
//
// Allocation policy: the OrderBook performs no allocation itself.
//   insert_order: caller supplies a pre-allocated PriceLevel. If the price
//                 level already exists, the supplied node is not consumed and
//                 the caller is responsible for deallocating it.
//   remove_order: if the last order is removed from a level the level is
//                 unlinked and returned to the caller for deallocation.
//                 Returns nullptr when the level still has remaining orders.
class OrderBook {
public:
    OrderBook() = default;

    // Non-copyable, non-movable — owns pointer state to live nodes.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    // Insert order into the book at order->price.
    //
    // If a PriceLevel for order->price already exists, new_level_if_needed is
    // not used — the caller must deallocate it. The existing level is returned.
    //
    // If no level exists, new_level_if_needed is initialised and linked into
    // the correct sorted position. It is returned as the owning level.
    //
    // Sets order->level to the owning PriceLevel on success.
    PriceLevel* insert_order(Order* order, PriceLevel* new_level_if_needed);

    // Remove order from its current price level (order->level must be set).
    //
    // Updates level totals and unlinks the order from the level's order list.
    // If the level becomes empty it is unlinked from the bid/ask list and
    // returned — the caller must deallocate it. Returns nullptr otherwise.
    PriceLevel* remove_order(Order* order);

    PriceLevel* best_bid() const noexcept { return best_bid_; }
    PriceLevel* best_ask() const noexcept { return best_ask_; }
    bool        empty()    const noexcept { return !best_bid_ && !best_ask_; }

private:
    // Find an existing PriceLevel for the given side and price.
    PriceLevel* find_level(Side side, Price price) noexcept;

    // Link a new, initialised PriceLevel into the correct sorted position.
    // Side determines which list (bid/ask) and sort direction.
    void link_level(Side side, PriceLevel* lv) noexcept;

    // Unlink a PriceLevel from its side.
    // Side is derived from whether lv lives in the bid or ask list.
    void unlink_level(Side side, PriceLevel* lv) noexcept;

    // Head (best) and tail (worst) pointers for each side.
    // Bids: head = best_bid_ (highest price), tail = worst bid (lowest).
    // Asks: head = best_ask_ (lowest  price), tail = worst ask (highest).
    PriceLevel* best_bid_{nullptr};
    PriceLevel* bid_tail_{nullptr};
    PriceLevel* best_ask_{nullptr};
    PriceLevel* ask_tail_{nullptr};
};

}  // namespace exchange
