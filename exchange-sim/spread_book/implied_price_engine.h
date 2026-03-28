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
// Price formula (matching SpreadStrategy::compute_spread_price):
//   spread_price = sum_i(sign(ratio_i) * price_multiplier_i * leg_price_i)
//
// For 2-leg calendar (ratio +1/-1, price_multiplier 1/1):
//   spread_price = leg0_price - leg1_price
//
// For butterfly (ratio +1/-2/+1, price_multiplier 1/2/1):
//   spread_price = leg0_price - 2*leg1_price + leg2_price
//
// Quantity uses abs(ratio) — how many outright lots per spread lot:
//   spread_qty = min_i(leg_qty_i / abs(ratio_i))
//
// Implied-in inverts the formula to solve for one target leg price:
//   target_price = (spread_price - other_legs_cost)
//                  / (sign(target_ratio) * target_price_multiplier)
//
// Lot GCD:
//   compute_min_spread_qty() returns the minimum spread quantity that
//   produces whole-number leg lots for all legs, accounting for different
//   leg lot sizes and ratios.

class ImpliedPriceEngine {
public:
    // --- Implied-out bid ---
    //
    // Synthetic spread bid from outright leg BBOs.
    // Buying the spread: buy positive-ratio legs (use their bid), sell
    // negative-ratio legs (use their ask).
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
            const Price pm = static_cast<Price>(std::abs(leg.price_multiplier));

            if (leg.ratio > 0) {
                if (!bbo.has_bid()) return std::nullopt;
                price += pm * bbo.bid_price;
                min_qty = std::min(min_qty, bbo.bid_qty / abs_r);
            } else {
                if (!bbo.has_ask()) return std::nullopt;
                price -= pm * bbo.ask_price;
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
    // Selling the spread: sell positive-ratio legs (use their ask), buy
    // negative-ratio legs (use their bid).

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
            const Price pm = static_cast<Price>(std::abs(leg.price_multiplier));

            if (leg.ratio > 0) {
                if (!bbo.has_ask()) return std::nullopt;
                price += pm * bbo.ask_price;
                min_qty = std::min(min_qty, bbo.ask_qty / abs_r);
            } else {
                if (!bbo.has_bid()) return std::nullopt;
                price -= pm * bbo.bid_price;
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
    // The target leg's BBO in bbos[] is IGNORED.
    //
    // For non-target legs, we use the execution price (ask if we buy, bid
    // if we sell), weighted by price_multiplier and signed by ratio.
    //
    // Solving for target:
    //   spread_price = sum(sign(ratio_i) * pm_i * leg_price_i)
    //   sign(target_ratio) * target_pm * target_price
    //     = spread_price - sum_other(sign(ratio_i) * pm_i * leg_price_i)
    //   target_price = numerator / (sign(target_ratio) * target_pm)
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
        const Price target_pm =
            static_cast<Price>(std::abs(target.price_multiplier));
        if (target_pm == 0) return std::nullopt;

        // Target leg side: Buy spread + positive ratio = buy leg.
        const bool tgt_is_buy = (spread_side == Side::Buy)
            ? (target.ratio > 0) : (target.ratio < 0);
        const Side target_side = tgt_is_buy ? Side::Buy : Side::Sell;

        // Accumulate weighted cost of non-target legs.
        Price other_cost = 0;
        Quantity min_qty = std::numeric_limits<Quantity>::max();

        for (size_t i = 0; i < legs.size(); ++i) {
            if (i == target_leg_index) continue;

            const auto& leg = legs[i];
            const auto& bbo = bbos[i];
            const int32_t abs_r = std::abs(static_cast<int32_t>(leg.ratio));
            if (abs_r == 0) return std::nullopt;
            const Price pm = static_cast<Price>(std::abs(leg.price_multiplier));

            const bool is_buy = (spread_side == Side::Buy)
                ? (leg.ratio > 0) : (leg.ratio < 0);

            if (is_buy) {
                if (!bbo.has_ask()) return std::nullopt;
                // Cost contribution: sign(ratio) * pm * ask_price
                Price sign = (leg.ratio > 0) ? Price{1} : Price{-1};
                other_cost += sign * pm * bbo.ask_price;
                min_qty = std::min(min_qty, bbo.ask_qty / abs_r);
            } else {
                if (!bbo.has_bid()) return std::nullopt;
                Price sign = (leg.ratio > 0) ? Price{1} : Price{-1};
                other_cost += sign * pm * bbo.bid_price;
                min_qty = std::min(min_qty, bbo.bid_qty / abs_r);
            }
        }

        if (min_qty <= 0) return std::nullopt;

        // target_price = (spread_price - other_cost) /
        //                (sign(target_ratio) * target_pm)
        const Price numerator = spread_price - other_cost;
        const Price target_sign = (target.ratio > 0) ? Price{1} : Price{-1};
        const Price divisor = target_sign * target_pm;
        const Price raw_price = numerator / divisor;

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

    // --- Lot GCD / minimum spread quantity ---
    //
    // Computes the minimum spread quantity such that every leg trades a
    // whole number of that leg's lot_size.
    //
    // For leg i, we need: spread_qty * abs(ratio_i) % lot_size_i == 0
    // Per-leg minimum: lot_size_i / gcd(lot_size_i, abs(ratio_i))
    // Overall minimum: LCM of all per-leg minimums.
    //
    // Returns 0 if any leg has zero ratio or invalid lot size.

    static Quantity compute_min_spread_qty(
        std::span<const StrategyLeg> legs) noexcept
    {
        if (legs.empty()) return 0;

        Quantity result = 1;
        for (const auto& leg : legs) {
            const Quantity abs_r =
                static_cast<Quantity>(std::abs(static_cast<int32_t>(leg.ratio)));
            if (abs_r == 0 || leg.lot_size <= 0) return 0;

            const Quantity g = gcd(leg.lot_size, abs_r);
            const Quantity per_leg = leg.lot_size / g;
            result = lcm(result, per_leg);
        }
        return result;
    }

    // --- Quantity rounding ---
    //
    // Round implied quantity down to the nearest multiple of min_spread_qty.
    // Returns 0 if qty < min_spread_qty.

    static constexpr Quantity round_qty_down(Quantity qty,
                                             Quantity min_qty) noexcept {
        if (min_qty <= 0 || qty < min_qty) return 0;
        return (qty / min_qty) * min_qty;
    }

private:
    static constexpr Quantity gcd(Quantity a, Quantity b) noexcept {
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        while (b != 0) {
            Quantity t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    static constexpr Quantity lcm(Quantity a, Quantity b) noexcept {
        if (a == 0 || b == 0) return 0;
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        return (a / gcd(a, b)) * b;
    }
};

}  // namespace exchange
