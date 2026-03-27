#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <unordered_map>
#include <vector>

#include "exchange-core/types.h"

namespace exchange {

// --- Open order tracked by strategy client ---

struct OpenOrder {
    uint64_t cl_ord_id{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity qty{0};
    Timestamp created_at{0};  // nanoseconds, for age-based cancellation
};

// --- Client state passed to strategy tick functions ---

struct ClientState {
    std::unordered_map<uint64_t, OpenOrder> open_orders;
    int64_t position{0};       // net position in lots (positive = long)
    int64_t realized_pnl{0};   // realized P&L in price units
    Price last_fill_price{0};  // 0 = no fills yet
    uint32_t fill_count{0};    // monotonic fill counter

    // Configuration (set once at startup)
    Price ref_price{0};        // reference/mid price
    Price spread{0};           // half-spread from mid for order placement
    Quantity max_position{0};  // absolute position limit in lots
    Quantity lot_size{0};      // order size per lot (fixed-point, e.g. 10000 = 1 contract)
    Price tick_size{0};        // minimum price increment
    uint64_t next_cl_ord_id{1};

    // Current timestamp (nanoseconds) for age-based decisions
    Timestamp now{0};
};

// --- Order action emitted by a strategy ---

struct OrderAction {
    enum Type { New, Cancel, Modify };
    Type type{New};
    Side side{Side::Buy};
    Price price{0};
    Quantity qty{0};
    uint64_t cl_ord_id{0};        // for New: assigned by strategy; for Cancel/Modify: target
    uint64_t orig_cl_ord_id{0};   // for Modify: the original order being replaced
};

// Strategy tick function signature.
// Takes mutable ClientState (strategy may update ref_price) and RNG.
// Returns a vector of actions for the caller to execute.
using StrategyTick = std::function<std::vector<OrderAction>(ClientState&, std::mt19937&)>;

// ---------------------------------------------------------------------------
// Helper: snap a price to the tick grid
// ---------------------------------------------------------------------------

inline Price snap_to_tick(Price price, Price tick_size) {
    if (tick_size <= 0) return price;
    // Round to nearest tick
    Price remainder = price % tick_size;
    if (remainder < 0) remainder += tick_size;
    if (remainder >= tick_size / 2) {
        return price + (tick_size - remainder);
    }
    return price - remainder;
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy
//
// Places 1-3 limit orders on each side within [ref-spread, ref+spread].
// Randomly cancels orders older than 1-5 seconds (80% cancel, 20% modify).
// Adapts ref_price toward last fill: ref = 0.9*ref + 0.1*last_fill.
// ---------------------------------------------------------------------------

inline StrategyTick random_walk_strategy() {
    return [](ClientState& state, std::mt19937& rng) -> std::vector<OrderAction> {
        std::vector<OrderAction> actions;

        // Adapt ref_price toward last fill
        if (state.last_fill_price > 0) {
            state.ref_price = static_cast<Price>(
                0.9 * static_cast<double>(state.ref_price) +
                0.1 * static_cast<double>(state.last_fill_price));
            // Snap adapted ref to tick grid
            state.ref_price = snap_to_tick(state.ref_price, state.tick_size);
        }

        // Count open orders per side
        int buy_count = 0, sell_count = 0;
        for (const auto& [id, order] : state.open_orders) {
            if (order.side == Side::Buy) ++buy_count;
            else ++sell_count;
        }

        // Place new orders if fewer than 3 on a side and position allows
        auto place_orders = [&](Side side, int current_count) {
            std::uniform_int_distribution<int> count_dist(1, 3);
            int target = count_dist(rng);
            int to_place = std::max(0, target - current_count);

            for (int i = 0; i < to_place; ++i) {
                // Check position limit
                int64_t projected = state.position;
                if (side == Side::Buy) projected += state.lot_size;
                else projected -= state.lot_size;

                if (std::abs(projected) > state.max_position) continue;

                // Sample price within [ref - spread, ref + spread]
                Price low = state.ref_price - state.spread;
                Price high = state.ref_price + state.spread;
                std::uniform_int_distribution<Price> price_dist(low, high);
                Price price = snap_to_tick(price_dist(rng), state.tick_size);

                // Ensure buy prices are below ref and sell above (basic sanity)
                if (side == Side::Buy && price > state.ref_price) {
                    price = snap_to_tick(state.ref_price - state.tick_size, state.tick_size);
                }
                if (side == Side::Sell && price < state.ref_price) {
                    price = snap_to_tick(state.ref_price + state.tick_size, state.tick_size);
                }

                if (price <= 0) continue;

                OrderAction action;
                action.type = OrderAction::New;
                action.side = side;
                action.price = price;
                action.qty = state.lot_size;
                action.cl_ord_id = state.next_cl_ord_id++;
                actions.push_back(action);
            }
        };

        place_orders(Side::Buy, buy_count);
        place_orders(Side::Sell, sell_count);

        // Cancel or modify old orders (older than 1-5 seconds)
        constexpr int64_t NS_PER_SEC = 1'000'000'000LL;
        std::uniform_int_distribution<int64_t> age_dist(1 * NS_PER_SEC, 5 * NS_PER_SEC);
        std::uniform_int_distribution<int> action_dist(1, 100);  // 1-80 = cancel, 81-100 = modify

        // Collect ids first to avoid mutating while iterating
        std::vector<uint64_t> old_order_ids;
        int64_t age_threshold = age_dist(rng);
        for (const auto& [id, order] : state.open_orders) {
            if (state.now - order.created_at > age_threshold) {
                old_order_ids.push_back(id);
            }
        }

        for (uint64_t id : old_order_ids) {
            int roll = action_dist(rng);
            if (roll <= 80) {
                // Cancel
                OrderAction action;
                action.type = OrderAction::Cancel;
                action.cl_ord_id = id;
                actions.push_back(action);
            } else {
                // Modify: resample price
                const auto& orig = state.open_orders[id];
                Price low = state.ref_price - state.spread;
                Price high = state.ref_price + state.spread;
                std::uniform_int_distribution<Price> price_dist(low, high);
                Price new_price = snap_to_tick(price_dist(rng), state.tick_size);
                if (new_price <= 0) continue;

                OrderAction action;
                action.type = OrderAction::Modify;
                action.side = orig.side;
                action.price = new_price;
                action.qty = state.lot_size;
                action.cl_ord_id = state.next_cl_ord_id++;
                action.orig_cl_ord_id = id;
                actions.push_back(action);
            }
        }

        return actions;
    };
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy
//
// Always quotes bid/ask around mid. On fill, re-quotes filled side.
// Leans quotes away from net position to reduce inventory risk.
// ---------------------------------------------------------------------------

inline StrategyTick market_maker_strategy() {
    return [prev_fill_count = uint32_t{0}](
               ClientState& state, std::mt19937& /*rng*/) mutable
        -> std::vector<OrderAction> {
        std::vector<OrderAction> actions;

        // Compute position lean: shift quotes away from position
        // lean > 0 when long -> lower bid target, higher ask target
        Price lean = 0;
        if (state.tick_size > 0) {
            lean = state.position * state.tick_size / (10 * state.lot_size);
        }

        Price half_spread = state.spread / 2;
        Price bid_target = snap_to_tick(state.ref_price - half_spread - lean, state.tick_size);
        Price ask_target = snap_to_tick(state.ref_price + half_spread - lean, state.tick_size);

        // Ensure bid < ask (sanity)
        if (bid_target >= ask_target) {
            bid_target = snap_to_tick(state.ref_price - state.tick_size, state.tick_size);
            ask_target = snap_to_tick(state.ref_price + state.tick_size, state.tick_size);
        }
        if (bid_target <= 0) bid_target = state.tick_size;

        // Find current bid/ask orders
        const OpenOrder* current_bid = nullptr;
        const OpenOrder* current_ask = nullptr;
        for (const auto& [id, order] : state.open_orders) {
            if (order.side == Side::Buy) {
                if (!current_bid || order.price > current_bid->price)
                    current_bid = &order;
            } else {
                if (!current_ask || order.price < current_ask->price)
                    current_ask = &order;
            }
        }

        // Detect fills since last tick
        bool had_fill = (state.fill_count != prev_fill_count);
        prev_fill_count = state.fill_count;

        // Re-quote bid if missing, price changed, or fill occurred
        bool need_bid = !current_bid ||
                        current_bid->price != bid_target ||
                        had_fill;
        bool need_ask = !current_ask ||
                        current_ask->price != ask_target ||
                        had_fill;

        // Cancel stale bid and place new one
        if (need_bid) {
            if (current_bid) {
                OrderAction cancel;
                cancel.type = OrderAction::Cancel;
                cancel.cl_ord_id = current_bid->cl_ord_id;
                actions.push_back(cancel);
            }
            // Check position limit before placing buy
            if (state.position + state.lot_size <= state.max_position) {
                OrderAction new_bid;
                new_bid.type = OrderAction::New;
                new_bid.side = Side::Buy;
                new_bid.price = bid_target;
                new_bid.qty = state.lot_size;
                new_bid.cl_ord_id = state.next_cl_ord_id++;
                actions.push_back(new_bid);
            }
        }

        // Cancel stale ask and place new one
        if (need_ask) {
            if (current_ask) {
                OrderAction cancel;
                cancel.type = OrderAction::Cancel;
                cancel.cl_ord_id = current_ask->cl_ord_id;
                actions.push_back(cancel);
            }
            // Check position limit before placing sell
            if (state.position - state.lot_size >= -state.max_position) {
                OrderAction new_ask;
                new_ask.type = OrderAction::New;
                new_ask.side = Side::Sell;
                new_ask.price = ask_target;
                new_ask.qty = state.lot_size;
                new_ask.cl_ord_id = state.next_cl_ord_id++;
                actions.push_back(new_ask);
            }
        }

        return actions;
    };
}

}  // namespace exchange
