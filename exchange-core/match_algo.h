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

// ProRataMatch -- static policy class implementing pro-rata (proportional)
// matching.
//
// At each matchable price level, allocates the aggressor quantity
// proportionally across all resting orders based on their remaining quantity:
//
//   allocation[i] = floor(order[i].remaining_quantity * to_fill / level.total_quantity)
//
// Multiply first, divide second to avoid premature truncation. Orders
// receiving a zero allocation in the proportional pass are skipped in the
// base allocation but remain eligible for remainder lots.
//
// Remainder lots (due to integer floor rounding) are distributed one lot at
// a time in FIFO order (time priority as secondary sort) across ALL orders
// in the level, regardless of whether they received a base allocation.
//
// Two-phase apply:
//   Phase 1 -- Proportional base allocation (skipping zero-allocation orders).
//   Phase 2 -- Distribute remainder one lot at a time in FIFO order; if an
//              order already has a fill result, increment its quantity; if not
//              (zero-allocation order receiving remainder), create a new result.
//   Phase 3 -- Apply fills to order state (filled_quantity, remaining_quantity)
//              and populate resting_remaining in each FillResult.
//
// Caller responsibilities after match() returns: same as FifoMatch.
struct ProRataMatch {
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count) {
        if (remaining <= 0 || level.head == nullptr) return;

        Quantity to_fill = std::min(remaining, level.total_quantity);
        size_t start_count = count;

        // ---------------------------------------------------------------
        // Phase 1: Proportional (base) allocation
        // ---------------------------------------------------------------
        // For each order, compute floor(order.remaining * to_fill / level.total).
        // Only create a result entry for orders with a non-zero allocation.
        Quantity allocated = 0;
        Order* order = level.head;
        while (order != nullptr) {
            Quantity alloc = order->remaining_quantity * to_fill / level.total_quantity;
            if (alloc > 0) {
                results[count].resting_order = order;
                results[count].price         = level.price;
                results[count].quantity      = alloc;
                allocated += alloc;
                ++count;
            }
            order = order->next;
        }

        // ---------------------------------------------------------------
        // Phase 2: Distribute remainder one lot at a time in FIFO order
        // ---------------------------------------------------------------
        // Walk the level list from head (FIFO). For each order, if there is
        // remainder left, give it one lot. Search existing results first; if
        // not found the order had zero base allocation, so create a new entry.
        Quantity remainder = to_fill - allocated;
        order = level.head;
        while (remainder > 0 && order != nullptr) {
            // Find whether this order already has a result entry.
            bool found = false;
            for (size_t i = start_count; i < count; ++i) {
                if (results[i].resting_order == order) {
                    results[i].quantity += 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Zero-base-allocation order receiving a remainder lot.
                results[count].resting_order = order;
                results[count].price         = level.price;
                results[count].quantity      = 1;
                ++count;
            }
            --remainder;
            order = order->next;
        }

        // ---------------------------------------------------------------
        // Phase 3: Apply fills to order state
        // ---------------------------------------------------------------
        for (size_t i = start_count; i < count; ++i) {
            Order* o = results[i].resting_order;
            o->filled_quantity    += results[i].quantity;
            o->remaining_quantity -= results[i].quantity;
            results[i].resting_remaining = o->remaining_quantity;
        }

        remaining -= to_fill;
    }
};

}  // namespace exchange
