#pragma once

#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "krx/krx_products.h"

#include <vector>

namespace exchange {
namespace krx {

// KRX spread instrument IDs start at 3001 to avoid collision with CME (1001+)
// and ICE (2001+).
// Convention: 3001-3099 = calendars, 3101-3199 = butterflies,
//             3201-3299 = inter-product spreads.

// get_krx_spread_products -- canonical KRX derivatives spread product table.
//
// Defines four standard KRX spread instruments:
//
//   KS-CAL   KOSPI200 calendar spread     (Buy KS front, Sell KS back)
//   KTB-CAL  KTB 3Y calendar spread       (Buy KTB front, Sell KTB back)
//   KS-BF    KOSPI200 butterfly            (+1 near, -2 mid, +1 far)
//   KS-MKS   KOSPI200/Mini inter-product  (Buy KS, Sell MKS, 5x ratio)
//
// Leg instrument IDs reference the outright product table (get_krx_products):
//   KS=1, MKS=2, KSQ=3, KSO=4, ..., KTB=9, LKTB=10.
//
// Calendar spreads use the same instrument_id for both legs (same product,
// different expiry -- expiry selection is handled at the exchange session
// level, not in the strategy definition).
//
// Tick/lot sizes on each leg are copied from the outright product config.
// Spread tick is auto-computed as GCD of leg tick contributions.
inline std::vector<SpreadInstrumentConfig> get_krx_spread_products() {
    // Outright tick/lot constants (4-decimal-place fixed-point).
    // KS  (id=1): 0.05 index pts -> tick 500, lot 10000
    // MKS (id=2): 0.02 index pts -> tick 200, lot 10000
    // KTB (id=9): 0.01 pts       -> tick 100, lot 10000
    constexpr Price    ks_tick  = 500;
    constexpr Quantity ks_lot   = 10000;
    constexpr Price    mks_tick = 200;
    constexpr Quantity mks_lot  = 10000;
    constexpr Price    ktb_tick = 100;
    constexpr Quantity ktb_lot  = 10000;

    auto make_leg = [](uint32_t id, int8_t ratio, int16_t pmul,
                       Price tick, Quantity lot) -> StrategyLeg {
        StrategyLeg leg;
        leg.instrument_id    = id;
        leg.ratio            = ratio;
        leg.price_multiplier = pmul;
        leg.tick_size        = tick;
        leg.lot_size         = lot;
        return leg;
    };

    std::vector<SpreadInstrumentConfig> spreads;
    spreads.reserve(4);

    // KS-CAL: KOSPI200 Calendar Spread -- most traded KRX spread.
    // Buy front-month KS, sell back-month KS.
    // Spread tick = GCD(500*1, 500*1) = 500
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 3001;
        cfg.symbol        = "KS-CAL";
        cfg.strategy_type = StrategyType::CalendarSpread;
        cfg.legs = {
            make_leg(1, +1, 1, ks_tick, ks_lot),
            make_leg(1, -1, 1, ks_tick, ks_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    // KTB-CAL: KTB 3-Year Calendar Spread.
    // Buy front-month KTB, sell back-month KTB.
    // Spread tick = GCD(100*1, 100*1) = 100
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 3002;
        cfg.symbol        = "KTB-CAL";
        cfg.strategy_type = StrategyType::CalendarSpread;
        cfg.legs = {
            make_leg(9, +1, 1, ktb_tick, ktb_lot),
            make_leg(9, -1, 1, ktb_tick, ktb_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    // KS-BF: KOSPI200 Butterfly -- +1 near, -2 mid, +1 far.
    // All three legs are KS (same product, three consecutive expiries).
    // Spread tick = GCD(500*1, 500*2, 500*1) = GCD(500, 1000, 500) = 500
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 3101;
        cfg.symbol        = "KS-BF";
        cfg.strategy_type = StrategyType::Butterfly;
        cfg.legs = {
            make_leg(1, +1, 1, ks_tick, ks_lot),
            make_leg(1, -2, 2, ks_tick, ks_lot),
            make_leg(1, +1, 1, ks_tick, ks_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    // KS-MKS: KOSPI200/Mini inter-product spread.
    // Buy 1 KS, sell 5 MKS (1 KOSPI200 = 5 Mini-KOSPI200 by notional).
    // Spread tick = GCD(500*1, 200*5) = GCD(500, 1000) = 500
    //
    // The price_multiplier on the MKS leg is 5 because the spread theoretical
    // price accounts for the 5x notional ratio:
    //   spread_price = KS_price * 1 - MKS_price * 5
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 3201;
        cfg.symbol        = "KS-MKS";
        cfg.strategy_type = StrategyType::InterCommodity;
        cfg.legs = {
            make_leg(1, +1, 1, ks_tick, ks_lot),
            make_leg(2, -5, 5, mks_tick, mks_lot),
        };
        // Override spread tick to 200 as specified in the task.
        // Auto-computed GCD would be 500, but KRX quotes this spread in
        // Mini-KOSPI200 tick increments for finer granularity.
        cfg.tick_size_override = 200;
        spreads.push_back(std::move(cfg));
    }

    return spreads;
}

}  // namespace krx
}  // namespace exchange
