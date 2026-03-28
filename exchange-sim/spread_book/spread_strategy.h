#pragma once

#include "exchange-core/types.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace exchange {

// Classification of spread strategy types.
// Used for exchange-specific validation and reporting, not matching logic.
enum class StrategyType : uint8_t {
    CalendarSpread,    // same product, different expiry (e.g. ES Jun-Sep)
    Butterfly,         // 3-leg: +1 near, -2 mid, +1 far
    Condor,            // 4-leg: +1, -1, -1, +1
    InterCommodity,    // different products (e.g. crack spread CL vs RB)
    Strip,             // multiple consecutive months
    Custom             // exchange-defined or user-defined
};

// A single leg within a spread strategy.
//
// ratio: signed multiplier for this leg.
//   +1 = buy 1 lot per spread, -1 = sell 1 lot per spread.
//   Butterfly: +1, -2, +1.  Convention: first leg ratio is always positive.
//
// price_multiplier: weight applied to leg price when computing the spread's
//   theoretical price.  Usually abs(ratio) but can differ for inter-commodity
//   spreads where contract multipliers differ.
//
// tick_size / lot_size: copied from the leg instrument's EngineConfig at
//   strategy construction time.  Used for spread tick/lot normalization.
struct StrategyLeg {
    uint32_t instrument_id{0};
    int8_t   ratio{0};             // signed leg ratio (e.g. +1, -2)
    int16_t  price_multiplier{1};  // weight for spread price calculation
    Price    tick_size{0};         // leg instrument tick size (fixed-point)
    Quantity lot_size{0};          // leg instrument lot size (fixed-point)
};

// SpreadStrategy defines a multi-leg spread instrument.
//
// Invariants after construction with valid legs:
// - At least 2 legs.
// - First leg ratio > 0 (convention).
// - tick_size() > 0, lot_size() > 0.
//
// Tick normalization:
//   spread_tick = GCD of (leg_tick_i * abs(price_multiplier_i)) across all legs.
//   This ensures every valid leg price combination maps to a valid spread price.
//
// Lot normalization (min_trade_qty):
//   For spread qty Q, leg i needs Q * abs(ratio_i) outright lots.
//   For this to be a whole multiple of leg_i.lot_size:
//     Q * abs(ratio_i) % leg_i.lot_size == 0
//     => Q % (leg_i.lot_size / gcd(leg_i.lot_size, abs(ratio_i))) == 0
//   min_trade_qty = LCM of all (leg_i.lot_size / gcd(leg_i.lot_size, abs(ratio_i))).
class SpreadStrategy {
public:
    SpreadStrategy() = default;

    // Construct with explicit tick/lot override (0 = auto-compute from legs).
    SpreadStrategy(StrategyType type, std::vector<StrategyLeg> legs,
                   Price tick_size_override = 0, Quantity lot_size_override = 0)
        : type_(type), legs_(std::move(legs)) {
        tick_size_ = tick_size_override > 0
            ? tick_size_override
            : compute_tick_size();
        lot_size_ = lot_size_override > 0
            ? lot_size_override
            : compute_min_trade_qty();
    }

    // --- Accessors ---

    StrategyType type() const { return type_; }
    const std::vector<StrategyLeg>& legs() const { return legs_; }
    size_t leg_count() const { return legs_.size(); }

    // Minimum price increment for the spread instrument.
    Price tick_size() const { return tick_size_; }

    // Minimum tradeable spread quantity (in fixed-point spread-lots).
    Quantity lot_size() const { return lot_size_; }

    // Compute theoretical spread price from outright leg prices.
    // spread_price = sum(leg_price_i * price_multiplier_i * sign(ratio_i))
    Price compute_spread_price(const Price* leg_prices, size_t count) const {
        if (count != legs_.size()) return 0;
        Price spread_price = 0;
        for (size_t i = 0; i < legs_.size(); ++i) {
            int8_t sign = legs_[i].ratio > 0 ? 1 : -1;
            spread_price += leg_prices[i]
                * static_cast<Price>(legs_[i].price_multiplier)
                * sign;
        }
        return spread_price;
    }

    // Outright quantity needed for leg i given spread_qty spread-lots.
    // Returns spread_qty * abs(ratio_i).
    Quantity leg_quantity(size_t leg_index, Quantity spread_qty) const {
        if (leg_index >= legs_.size()) return 0;
        return spread_qty * static_cast<Quantity>(std::abs(legs_[leg_index].ratio));
    }

    // Side for leg i when the spread is traded on the given side.
    // Buy spread: positive-ratio legs Buy, negative-ratio legs Sell.
    // Sell spread: inverted.
    Side leg_side(size_t leg_index, Side spread_side) const {
        bool leg_is_buy = legs_[leg_index].ratio > 0;
        if (spread_side == Side::Sell) leg_is_buy = !leg_is_buy;
        return leg_is_buy ? Side::Buy : Side::Sell;
    }

private:
    // GCD of (leg_tick_i * abs(price_multiplier_i)) across all legs.
    Price compute_tick_size() const {
        if (legs_.empty()) return 0;
        Price g = 0;
        for (const auto& leg : legs_) {
            Price contribution =
                leg.tick_size * static_cast<Price>(std::abs(leg.price_multiplier));
            g = std::gcd(g, contribution);
        }
        return g > 0 ? g : 1;
    }

    // LCM of (leg_lot_i / gcd(leg_lot_i, abs(ratio_i))) across all legs.
    Quantity compute_min_trade_qty() const {
        if (legs_.empty()) return PRICE_SCALE;
        Quantity result = 1;
        for (const auto& leg : legs_) {
            Quantity abs_ratio = static_cast<Quantity>(std::abs(leg.ratio));
            Quantity divisor = std::gcd(leg.lot_size, abs_ratio);
            Quantity factor = leg.lot_size / divisor;
            result = std::lcm(result, factor);
        }
        return result;
    }

    StrategyType type_{StrategyType::Custom};
    std::vector<StrategyLeg> legs_;
    Price tick_size_{0};
    Quantity lot_size_{0};
};

}  // namespace exchange
