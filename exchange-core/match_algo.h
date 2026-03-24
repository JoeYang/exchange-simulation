#pragma once

#include <algorithm>
#include <cstddef>

#include "exchange-core/types.h"

namespace exchange {

// FifoMatch -- static policy class implementing FIFO (time priority) matching.
//
// Matches an aggressor against resting orders at a single price level by
// walking from head to tail (oldest to newest). Each resting order is filled
// as much as possible before the algorithm advances to the next one.
//
// Caller responsibilities after match() returns:
//   - Remove fully-filled orders (resting_remaining == 0) from the level list.
//   - Update level.total_quantity and level.order_count to reflect removals.
//   - If remaining > 0 after the call, advance to the next price level.
struct FifoMatch {
    // Match aggressor against resting orders at a single price level.
    //
    // Parameters:
    //   level     - the price level to match against
    //   remaining - aggressor's remaining quantity (decremented in place)
    //   results   - output array of fill results (caller must ensure capacity)
    //   count     - number of fills written to results (incremented in place)
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count) {
        Order* order = level.head;
        while (order != nullptr && remaining > 0) {
            Quantity fill_qty = std::min(remaining, order->remaining_quantity);

            results[count].resting_order     = order;
            results[count].price             = level.price;
            results[count].quantity          = fill_qty;

            order->filled_quantity    += fill_qty;
            order->remaining_quantity -= fill_qty;
            remaining                 -= fill_qty;

            results[count].resting_remaining = order->remaining_quantity;
            ++count;

            order = order->next;
        }
    }
};

}  // namespace exchange
