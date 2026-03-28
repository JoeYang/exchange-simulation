#pragma once

#include "exchange-core/types.h"
#include "exchange-sim/spread_book/spread_strategy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>

namespace exchange {

// --- BBO snapshot for one outright leg ---

struct LegBBO {
    Price bid_price{0};
    Quantity bid_qty{0};
    Price ask_price{0};
    Quantity ask_qty{0};

    bool has_bid() const noexcept { return bid_qty > 0; }
    bool has_ask() const noexcept { return ask_qty > 0; }
};

// --- Source of an implied level ---

enum class ImpliedSource : uint8_t {
    ImpliedOut,  // outright BBOs -> synthetic spread price
    ImpliedIn,   // spread order + outright BBOs -> synthetic outright price
};

// --- Computed implied price/quantity at one level ---

struct ImpliedLevel {
    Price price{0};
    Quantity quantity{0};
    Side side{Side::Buy};
    ImpliedSource source{ImpliedSource::ImpliedOut};
};

// --- ImpliedPriceEngine ---
//
// Pure-computation engine.  All methods are static and stateless.
// Takes StrategyLeg definitions and per-leg BBOs, produces implied levels.
//
// BBOs are indexed by position in the legs span (bbos[i] corresponds to
// legs[i]).  The caller is responsible for gathering BBOs from the correct
// outright instruments.
//
// Allocation-free, single-threaded, designed for the hot path.
//
// Implied-out (outright BBOs -> synthetic spread BBO):
//   spread_bid = sum_i(ratio_i > 0 ? ratio_i * bid_i : ratio_i * ask_i)
//   spread_ask = sum_i(ratio_i > 0 ? ratio_i * ask_i : ratio_i * bid_i)
//   qty = min_i(available_qty_i / abs(ratio_i))
//
// Implied-in (spread order + N-1 outright BBOs -> synthetic outright):
//   target_price = (spread_price - other_legs_cost) / target_ratio
//   where other_legs_cost uses the execution price of each non-target leg.

class ImpliedPriceEngine {
public:
    // --- Implied-out bid ---
    //
    // Synthetic spread bid from outright leg BBOs.
    // For a 2-leg calendar spread (+1/-1):
    //   implied_bid = leg0_bid - leg1_ask
    //   qty = min(leg0_bid_qty, leg1_ask_qty)
    //
    // spread_tick_size: round result to valid tick (0 = no rounding).
    // Returns nullopt if any leg lacks the needed BBO side.

    static std::optional<ImpliedLevel> compute_implied_out_bid(
        std::span<const StrategyLeg> legs,
        std::span<const LegBBO> bbos,
        Price spread_tick_size) noexcept
    {
        if (legs.size() != bbos.size() || legs.empty()) return std::nullopt;

        Price price = 0;
        Quantity min_qty = std::numeric_limits<Quantity>::max();

        for (size_t i = 0; i < legs.size(); ++i) {
            const auto& leg = legs[i];
            const auto& bbo = bbos[i];
            const int32_t abs_r = std::abs(static_cast<int32_t>(leg.ratio));
            if (abs_r == 0) return std::nullopt;

            if (leg.ratio > 0) {
                // Buy leg: use outright bid.
                if (!bbo.has_bid()) return std::nullopt;
                price += static_cast<Price>(leg.ratio) * bbo.bid_price;
                min_qty = std::min(min_qty, bbo.bid_qty / abs_r);
            } else {
                // Sell leg: use outright ask.
                if (!bbo.has_ask()) return std::nullopt;
                price += static_cast<Price>(leg.ratio) * bbo.ask_price;
                min_qty = std::min(min_qty, bbo.ask_qty / abs_r);
            }
        }

        if (min_qty <= 0) return std::nullopt;

        return ImpliedLevel{
            .price = normalize_tick(price, spread_tick_size),
            .quantity = min_qty,
            .side = Side::Buy,
            .source = ImpliedSource::ImpliedOut,
        };
    }

    // --- Implied-out ask ---
    //
    // Synthetic spread ask from outright leg BBOs.
    // For a 2-leg calendar spread (+1/-1):
    //   implied_ask = leg0_ask - leg1_bid
    //   qty = min(leg0_ask_qty, leg1_bid_qty)

    static std::optional<ImpliedLevel> compute_implied_out_ask(
        std::span<const StrategyLeg> legs,
        std::span<const LegBBO> bbos,
        Price spread_tick_size) noexcept
    {
        if (legs.size() != bbos.size() || legs.empty()) return std::nullopt;

        Price price = 0;
        Quantity min_qty = std::numeric_limits<Quantity>::max();

        for (size_t i = 0; i < legs.size(); ++i) {
            const auto& leg = legs[i];
            const auto& bbo = bbos[i];
            const int32_t abs_r = std::abs(static_cast<int32_t>(leg.ratio));
            if (abs_r == 0) return std::nullopt;

            if (leg.ratio > 0) {
                // Sell leg: use outright ask.
                if (!bbo.has_ask()) return std::nullopt;
                price += static_cast<Price>(leg.ratio) * bbo.ask_price;
                min_qty = std::min(min_qty, bbo.ask_qty / abs_r);
            } else {
                // Buy leg: use outright bid.
                if (!bbo.has_bid()) return std::nullopt;
                price += static_cast<Price>(leg.ratio) * bbo.bid_price;
                min_qty = std::min(min_qty, bbo.bid_qty / abs_r);
            }
        }

        if (min_qty <= 0) return std::nullopt;

        return ImpliedLevel{
            .price = normalize_tick(price, spread_tick_size),
            .quantity = min_qty,
            .side = Side::Sell,
            .source = ImpliedSource::ImpliedOut,
        };
    }

    // --- Implied-in ---
    //
    // Given a spread order (side, price) and outright BBOs for all legs,
    // compute the implied outright price for one target leg.
    //
    // target_leg_index: index into the legs array.
    //
    // The target leg's BBO in bbos[] is IGNORED -- we derive its implied
    // price from the spread price and the other legs' BBOs.
    //
    // For 2-leg calendar (+1/-1), spread Buy@S:
    //   We buy leg0, sell leg1.
    //   Target=leg1: implied sell @ leg0_ask - S  (buy leg0 at ask, sell
    //     leg1 at whatever makes spread = S)
    //   Target=leg0: implied buy @ leg1_bid + S   (sell leg1 at bid, buy
    //     leg0 at whatever makes spread = S)
    //
    // For non-target legs, we use the execution price (ask if we buy, bid
    // if we sell), accounting for the spread side.
    //
    // outright_tick_size: tick size for the target outright (0 = no rounding).

    static std::optional<ImpliedLevel> compute_implied_in(
        std::span<const StrategyLeg> legs,
        std::span<const LegBBO> bbos,
        size_t target_leg_index,
        Side spread_side,
        Price spread_price,
        Price outright_tick_size) noexcept
    {
        if (legs.size() != bbos.size() || legs.empty()) return std::nullopt;
        if (target_leg_index >= legs.size()) return std::nullopt;

        const auto& target = legs[target_leg_index];
        if (target.ratio == 0) return std::nullopt;

        // Target leg side: Buy spread + positive ratio = buy leg.
        const bool tgt_buy_on_spread_buy = (target.ratio > 0);
        const bool tgt_is_buy = (spread_side == Side::Buy)
            ? tgt_buy_on_spread_buy : !tgt_buy_on_spread_buy;
        const Side target_side = tgt_is_buy ? Side::Buy : Side::Sell;

        // Accumulate cost of non-target legs (using execution-side prices).
        Price other_cost = 0;
        Quantity min_qty = std::numeric_limits<Quantity>::max();

        for (size_t i = 0; i < legs.size(); ++i) {
            if (i == target_leg_index) continue;

            const auto& leg = legs[i];
            const auto& bbo = bbos[i];
            const int32_t abs_r = std::abs(static_cast<int32_t>(leg.ratio));
            if (abs_r == 0) return std::nullopt;

            const bool buy_on_spread_buy = (leg.ratio > 0);
            const bool is_buy = (spread_side == Side::Buy)
                ? buy_on_spread_buy : !buy_on_spread_buy;

            if (is_buy) {
                if (!bbo.has_ask()) return std::nullopt;
                other_cost += static_cast<Price>(leg.ratio) * bbo.ask_price;
                min_qty = std::min(min_qty, bbo.ask_qty / abs_r);
            } else {
                if (!bbo.has_bid()) return std::nullopt;
                other_cost += static_cast<Price>(leg.ratio) * bbo.bid_price;
                min_qty = std::min(min_qty, bbo.bid_qty / abs_r);
            }
        }

        if (min_qty <= 0) return std::nullopt;

        // spread_price = sum(ratio_i * leg_price_i)
        // target.ratio * target_price = spread_price - other_cost
        // target_price = (spread_price - other_cost) / target.ratio
        const Price numerator = spread_price - other_cost;
        const Price raw_price = numerator / static_cast<Price>(target.ratio);

        return ImpliedLevel{
            .price = normalize_tick(raw_price, outright_tick_size),
            .quantity = min_qty,
            .side = target_side,
            .source = ImpliedSource::ImpliedIn,
        };
    }

    // --- Tick normalization ---
    //
    // Round price toward zero to the nearest valid tick boundary.
    // tick_size <= 0 disables normalization.

    static constexpr Price normalize_tick(Price price,
                                          Price tick_size) noexcept {
        if (tick_size <= 0) return price;
        if (price >= 0) {
            return (price / tick_size) * tick_size;
        }
        // Negative price: round toward zero (less negative).
        // -7 with tick 5 -> -5 (not -10).
        return -(((-price) / tick_size) * tick_size);
    }
};

}  // namespace exchange
