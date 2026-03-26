#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "exchange-core/types.h"

namespace exchange {
namespace ice {

// GtbprMatch -- static policy class implementing ICE's Gradual Time-Based
// Pro Rata (GTBPR) matching algorithm for STIR products (Euribor, SONIA, SOFR).
//
// Algorithm at each price level:
//   1. Identify the priority order: the first (oldest) order at the level
//      whose remaining_quantity >= collar. If found, fill it up to cap.
//   2. Distribute remaining quantity pro-rata across ALL resting orders,
//      weighted by: remaining_quantity * time_weight(age, time_weight_factor)
//      where time_weight = 1.0 + factor * (age_ns / 1e9).
//   3. Remainder after integer truncation goes to the oldest order (FIFO).
//
// The priority order participates in the pro-rata phase as well (on its
// post-priority remaining quantity).
struct GtbprMatch {
    struct Config {
        Quantity collar{50000};         // min size for priority (e.g. 5 lots)
        Quantity cap{2000000};          // max priority fill quantity
        double time_weight_factor{0.1}; // age bonus: 1.0 + factor * age_seconds
    };

    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count,
                      Timestamp now, const Config& cfg) {
        if (remaining <= 0 || level.head == nullptr) return;

        Quantity to_fill = std::min(remaining, level.total_quantity);
        Quantity pool = to_fill;
        size_t start_count = count;

        // ------------------------------------------------------------------
        // Phase 1: Priority order — first order with size >= collar
        // ------------------------------------------------------------------
        Order* priority_order = nullptr;
        Quantity priority_fill = 0;

        for (Order* o = level.head; o != nullptr; o = o->next) {
            if (o->remaining_quantity >= cfg.collar) {
                priority_order = o;
                break;
            }
        }

        if (priority_order != nullptr) {
            priority_fill = std::min({cfg.cap, pool,
                                      priority_order->remaining_quantity});
            if (priority_fill > 0) {
                results[count].resting_order = priority_order;
                results[count].price         = level.price;
                results[count].quantity      = priority_fill;
                ++count;
                pool -= priority_fill;
            }
        }

        // ------------------------------------------------------------------
        // Phase 2: Time-weighted pro-rata allocation on remaining pool
        // ------------------------------------------------------------------
        if (pool > 0) {
            // Compute weighted sizes for all orders (using post-priority qty
            // for the priority order).
            double total_weight = 0.0;

            for (Order* o = level.head; o != nullptr; o = o->next) {
                Quantity effective_qty = o->remaining_quantity;
                if (o == priority_order) {
                    effective_qty -= priority_fill;
                }
                if (effective_qty <= 0) continue;

                double age_s = static_cast<double>(now - o->timestamp) / 1e9;
                if (age_s < 0.0) age_s = 0.0;
                double tw = 1.0 + cfg.time_weight_factor * age_s;
                double w = static_cast<double>(effective_qty) * tw;
                total_weight += w;
            }

            // Proportional allocation (floor).
            Quantity allocated = 0;
            if (total_weight > 0.0) {
                for (Order* o = level.head; o != nullptr; o = o->next) {
                    Quantity effective_qty = o->remaining_quantity;
                    if (o == priority_order) {
                        effective_qty -= priority_fill;
                    }
                    if (effective_qty <= 0) continue;

                    double age_s = static_cast<double>(now - o->timestamp) / 1e9;
                    if (age_s < 0.0) age_s = 0.0;
                    double tw = 1.0 + cfg.time_weight_factor * age_s;
                    double w = static_cast<double>(effective_qty) * tw;

                    auto alloc = static_cast<Quantity>(
                        std::floor(w / total_weight * static_cast<double>(pool)));
                    // Clamp to effective qty available.
                    alloc = std::min(alloc, effective_qty);
                    if (alloc <= 0) continue;

                    // Merge into existing result if priority order already has
                    // an entry, otherwise create a new one.
                    bool merged = false;
                    if (o == priority_order) {
                        for (size_t i = start_count; i < count; ++i) {
                            if (results[i].resting_order == o) {
                                results[i].quantity += alloc;
                                merged = true;
                                break;
                            }
                        }
                    }
                    if (!merged) {
                        results[count].resting_order = o;
                        results[count].price         = level.price;
                        results[count].quantity      = alloc;
                        ++count;
                    }
                    allocated += alloc;
                }
            }

            // --------------------------------------------------------------
            // Phase 3: Remainder — distribute one lot at a time, FIFO order
            // Loop back to head when we reach the end of the list, since
            // pro-rata clamping can leave more remainder than order count.
            // Guard against infinite loop with a progress check per pass.
            // --------------------------------------------------------------
            Quantity remainder = pool - allocated;
            while (remainder > 0) {
                Quantity distributed_this_pass = 0;
                for (Order* o = level.head; o != nullptr && remainder > 0;
                     o = o->next) {
                    // How much has this order been allocated so far?
                    Quantity already = 0;
                    for (size_t i = start_count; i < count; ++i) {
                        if (results[i].resting_order == o) {
                            already = results[i].quantity;
                            break;
                        }
                    }
                    Quantity max_additional = o->remaining_quantity - already;
                    if (max_additional <= 0) continue;

                    // Give one lot.
                    bool found = false;
                    for (size_t i = start_count; i < count; ++i) {
                        if (results[i].resting_order == o) {
                            results[i].quantity += 1;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        results[count].resting_order = o;
                        results[count].price         = level.price;
                        results[count].quantity      = 1;
                        ++count;
                    }
                    --remainder;
                    ++distributed_this_pass;
                }
                // Safety: if no order could accept a lot, break to avoid
                // infinite loop (should not happen with correct accounting).
                if (distributed_this_pass == 0) break;
            }
        }

        // ------------------------------------------------------------------
        // Phase 4: Apply fills to order state
        // ------------------------------------------------------------------
        for (size_t i = start_count; i < count; ++i) {
            Order* o = results[i].resting_order;
            o->filled_quantity    += results[i].quantity;
            o->remaining_quantity -= results[i].quantity;
            results[i].resting_remaining = o->remaining_quantity;
        }

        remaining -= to_fill;
    }
};

}  // namespace ice
}  // namespace exchange
