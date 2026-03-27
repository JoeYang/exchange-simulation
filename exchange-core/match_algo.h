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

// ThresholdProRataMatch -- pro-rata with minimum allocation threshold.
//
// Works like ProRataMatch but with an additional filter: any order whose
// proportional allocation falls below MinThreshold gets zero in the base
// pass. The unallocated quantity from zeroed orders is collected into a
// remainder pool and distributed via FIFO across ALL eligible orders
// (those that received a base allocation plus any orders that still have
// remaining quantity).
//
// This matches CME's threshold pro-rata algorithm where small orders
// must meet a minimum allocation size to participate in the proportional
// distribution.
//
// Template parameter:
//   MinThreshold -- minimum allocation in quantity units. Orders whose
//                   proportional share is strictly less than this value
//                   receive zero base allocation. Set to 0 for standard
//                   pro-rata behavior (no threshold).
//
// Three-phase algorithm:
//   Phase 1: Compute proportional allocations, zero those below threshold.
//   Phase 2: Distribute remainder (rounding + zeroed orders) via FIFO.
//   Phase 3: Apply fills to order state.

template <Quantity MinThreshold>
struct ThresholdProRataMatch {
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count) {
        if (remaining <= 0 || level.head == nullptr) return;

        Quantity to_fill = std::min(remaining, level.total_quantity);
        size_t start_count = count;

        // ---------------------------------------------------------------
        // Phase 1: Proportional allocation with threshold filter
        // ---------------------------------------------------------------
        Quantity allocated = 0;
        Order* order = level.head;
        while (order != nullptr) {
            Quantity alloc = order->remaining_quantity * to_fill
                             / level.total_quantity;

            if (alloc >= MinThreshold && alloc > 0) {
                results[count].resting_order = order;
                results[count].price         = level.price;
                results[count].quantity      = alloc;
                allocated += alloc;
                ++count;
            }
            // Orders below threshold: skipped (get zero base allocation).
            order = order->next;
        }

        // ---------------------------------------------------------------
        // Phase 2: Distribute remainder via FIFO
        // ---------------------------------------------------------------
        // Remainder includes both rounding residual and zeroed-order shares.
        Quantity fifo_remainder = to_fill - allocated;
        order = level.head;
        while (fifo_remainder > 0 && order != nullptr) {
            Quantity fifo_fill = std::min(fifo_remainder,
                                          order->remaining_quantity);

            // Check if this order already has a result from Phase 1.
            bool found = false;
            for (size_t i = start_count; i < count; ++i) {
                if (results[i].resting_order == order) {
                    // Cap: don't exceed the order's remaining quantity.
                    Quantity cap = order->remaining_quantity
                                   - results[i].quantity;
                    Quantity add = std::min(fifo_fill, cap);
                    results[i].quantity += add;
                    fifo_remainder -= add;
                    found = true;
                    break;
                }
            }
            if (!found && fifo_fill > 0) {
                results[count].resting_order = order;
                results[count].price         = level.price;
                results[count].quantity      = fifo_fill;
                fifo_remainder -= fifo_fill;
                ++count;
            }
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

// AllocationMatch -- pro-rata with remainder to largest resting order.
//
// Like ProRataMatch but remainder lots (from integer floor rounding) are
// distributed one at a time to the order with the largest remaining_quantity
// (original, pre-fill). Ties on size break to FIFO (oldest order wins).
//
// This is the CME "allocation" algorithm used for certain products where
// size priority is preferred over time priority for remainder distribution.
struct AllocationMatch {
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count) {
        if (remaining <= 0 || level.head == nullptr) return;

        Quantity to_fill = std::min(remaining, level.total_quantity);
        size_t start_count = count;

        // Phase 1: Proportional base allocation (same as ProRata).
        Quantity allocated = 0;
        Order* order = level.head;
        while (order != nullptr) {
            Quantity alloc = order->remaining_quantity * to_fill
                             / level.total_quantity;
            if (alloc > 0) {
                results[count].resting_order = order;
                results[count].price         = level.price;
                results[count].quantity      = alloc;
                allocated += alloc;
                ++count;
            }
            order = order->next;
        }

        // Phase 2: Distribute remainder to largest orders.
        // Build a temporary index sorted by remaining_quantity desc (FIFO tiebreak).
        // We walk the level list and for each remainder lot, find the order with
        // the largest original remaining_quantity that still has capacity.
        Quantity remainder = to_fill - allocated;
        while (remainder > 0) {
            // Find order with largest remaining_quantity (pre-fill, i.e. original).
            // FIFO tiebreak: first in list wins among equals.
            Order* best = nullptr;
            Quantity best_qty = -1;
            order = level.head;
            while (order != nullptr) {
                // Compute how much this order has been allocated so far.
                Quantity already = 0;
                for (size_t i = start_count; i < count; ++i) {
                    if (results[i].resting_order == order) {
                        already = results[i].quantity;
                        break;
                    }
                }
                Quantity capacity = order->remaining_quantity - already;
                if (capacity > 0 && order->remaining_quantity > best_qty) {
                    best = order;
                    best_qty = order->remaining_quantity;
                }
                order = order->next;
            }
            if (best == nullptr) break;

            // Give one lot to the best order.
            bool found = false;
            for (size_t i = start_count; i < count; ++i) {
                if (results[i].resting_order == best) {
                    results[i].quantity += 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                results[count].resting_order = best;
                results[count].price         = level.price;
                results[count].quantity      = 1;
                ++count;
            }
            --remainder;
        }

        // Phase 3: Apply fills to order state.
        for (size_t i = start_count; i < count; ++i) {
            Order* o = results[i].resting_order;
            o->filled_quantity    += results[i].quantity;
            o->remaining_quantity -= results[i].quantity;
            results[i].resting_remaining = o->remaining_quantity;
        }

        remaining -= to_fill;
    }
};

// SplitFifoProRataMatch -- configurable FIFO/ProRata split.
//
// Divides the matchable quantity into a FIFO portion and a ProRata portion
// based on FifoPct (compile-time percentage, 0-100).
//
//   fifo_qty    = floor(to_fill * FifoPct / 100)
//   prorata_qty = to_fill - fifo_qty
//
// The FIFO portion is filled first (head-to-tail, time priority). Then the
// ProRata portion is filled proportionally across orders that still have
// remaining quantity. Remainder from ProRata rounding goes via FIFO.
//
// Special cases:
//   FifoPct=0   -> pure ProRata
//   FifoPct=100 -> pure FIFO
//
// Template parameter:
//   FifoPct -- percentage of aggressor quantity filled by FIFO (0-100).

template <int FifoPct>
struct SplitFifoProRataMatch {
    static_assert(FifoPct >= 0 && FifoPct <= 100,
                  "FifoPct must be between 0 and 100");

    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count) {
        if (remaining <= 0 || level.head == nullptr) return;

        Quantity to_fill = std::min(remaining, level.total_quantity);

        Quantity fifo_qty    = to_fill * FifoPct / 100;
        Quantity prorata_qty = to_fill - fifo_qty;

        // ---------------------------------------------------------------
        // Phase 1: FIFO portion
        // ---------------------------------------------------------------
        Quantity fifo_left = fifo_qty;
        Order* order = level.head;
        while (order != nullptr && fifo_left > 0) {
            Quantity fill = std::min(fifo_left, order->remaining_quantity);
            if (fill > 0) {
                results[count].resting_order = order;
                results[count].price         = level.price;
                results[count].quantity      = fill;

                order->filled_quantity    += fill;
                order->remaining_quantity -= fill;
                results[count].resting_remaining = order->remaining_quantity;

                fifo_left -= fill;
                ++count;
            }
            order = order->next;
        }

        // Update level total for the ProRata phase.
        Quantity level_remaining = 0;
        order = level.head;
        while (order != nullptr) {
            level_remaining += order->remaining_quantity;
            order = order->next;
        }

        // ---------------------------------------------------------------
        // Phase 2: ProRata portion on remaining quantities
        // ---------------------------------------------------------------
        if (prorata_qty > 0 && level_remaining > 0) {
            Quantity pr_to_fill = std::min(prorata_qty, level_remaining);
            Quantity pr_allocated = 0;
            size_t pr_start = count;

            // Base proportional allocation.
            order = level.head;
            while (order != nullptr) {
                if (order->remaining_quantity > 0) {
                    Quantity alloc = order->remaining_quantity * pr_to_fill
                                     / level_remaining;
                    if (alloc > 0) {
                        results[count].resting_order = order;
                        results[count].price         = level.price;
                        results[count].quantity      = alloc;
                        pr_allocated += alloc;
                        ++count;
                    }
                }
                order = order->next;
            }

            // ProRata remainder via FIFO.
            Quantity pr_remainder = pr_to_fill - pr_allocated;
            order = level.head;
            while (pr_remainder > 0 && order != nullptr) {
                if (order->remaining_quantity > 0) {
                    // Find existing result for this order in ProRata phase.
                    Quantity already = 0;
                    size_t found_idx = count;
                    for (size_t i = pr_start; i < count; ++i) {
                        if (results[i].resting_order == order) {
                            already = results[i].quantity;
                            found_idx = i;
                            break;
                        }
                    }
                    Quantity cap = order->remaining_quantity - already;
                    Quantity add = std::min(pr_remainder, cap);
                    if (add > 0) {
                        if (found_idx < count) {
                            results[found_idx].quantity += add;
                        } else {
                            results[count].resting_order = order;
                            results[count].price         = level.price;
                            results[count].quantity      = add;
                            ++count;
                        }
                        pr_remainder -= add;
                    }
                }
                order = order->next;
            }

            // Apply ProRata fills to order state.
            for (size_t i = pr_start; i < count; ++i) {
                Order* o = results[i].resting_order;
                o->filled_quantity    += results[i].quantity;
                o->remaining_quantity -= results[i].quantity;
                results[i].resting_remaining = o->remaining_quantity;
            }
        }

        remaining -= to_fill;
    }
};

}  // namespace exchange
