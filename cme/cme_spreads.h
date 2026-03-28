#pragma once

#include "cme/cme_products.h"
#include "exchange-sim/spread_book/spread_instrument_config.h"

#include <vector>

namespace exchange {
namespace cme {

// CME spread instrument IDs start at 1001 to avoid collision with outrights.
// Convention: 1001-1099 = calendars, 1101-1199 = butterflies,
//             1201-1299 = condors,   1301-1399 = inter-commodity.

// get_cme_calendar_spreads -- standard 2-leg calendar spreads.
//
// Calendar spread: +1 near month, -1 far month.
// Spread price = near - far (can be negative when far > near).
// Tick size auto-computed from legs (same product => same tick).
//
// Uses instrument IDs from get_cme_products():
//   ES=1, NQ=2, CL=3, GC=4, ZN=5, ZB=6, MES=7, 6E=8
inline std::vector<SpreadInstrumentConfig> get_cme_calendar_spreads() {
    return {
        // ES calendar: ESU5(1) vs ESZ5(2) -- equity index
        // Both legs tick=2500, lot=10000
        {
            .id = 1001,
            .symbol = "ES-CAL-U5Z5",
            .strategy_type = StrategyType::CalendarSpread,
            .legs = {
                {.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
            },
        },
        // ZN calendar: ZNU5(5) vs ZNZ5(6) -- interest rate
        // ZN tick=156, ZB tick=313 (different products, but ZN cal uses ZN legs)
        // For simulator purposes, use ZN(5) and ZB(6) as proxy near/far months
        {
            .id = 1002,
            .symbol = "ZN-CAL",
            .strategy_type = StrategyType::CalendarSpread,
            .legs = {
                {.instrument_id = 5, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 156, .lot_size = 10000},
                {.instrument_id = 6, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 313, .lot_size = 10000},
            },
            // ZN-ZB spread tick = GCD(156*1, 313*1) = GCD(156,313) = 1
            // Override to a sensible minimum: 1 (finest resolution)
        },
        // CL calendar: CL near(3) vs CL far -- using GC(4) as proxy far month
        {
            .id = 1003,
            .symbol = "CL-CAL",
            .strategy_type = StrategyType::CalendarSpread,
            .legs = {
                {.instrument_id = 3, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 100, .lot_size = 10000},
                {.instrument_id = 4, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 1000, .lot_size = 10000},
            },
            // CL-GC spread tick = GCD(100,1000) = 100
        },
    };
}

// get_cme_butterfly_spreads -- 3-leg butterfly: +1 near, -2 mid, +1 far.
//
// Butterfly spread price = near - 2*mid + far.
// Typically same product across three consecutive expirations.
inline std::vector<SpreadInstrumentConfig> get_cme_butterfly_spreads() {
    return {
        // ES butterfly using ES(1), NQ(2), MES(7) as proxy months.
        // All equity index: tick=2500, lot=10000.
        {
            .id = 1101,
            .symbol = "ES-BF",
            .strategy_type = StrategyType::Butterfly,
            .legs = {
                {.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 2, .ratio = -2, .price_multiplier = 2,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 7, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
            },
            // Tick: GCD(2500*1, 2500*2, 2500*1) = GCD(2500,5000,2500) = 2500
        },
    };
}

// get_cme_condor_spreads -- 4-leg condor: +1, -1, -1, +1.
//
// Condor price = leg0 - leg1 - leg2 + leg3.
// Used for volatility trading across four consecutive expirations.
inline std::vector<SpreadInstrumentConfig> get_cme_condor_spreads() {
    return {
        // ES condor using ES(1), NQ(2), MES(7), 6E(8) as proxy months.
        // Mixed products for simulator testing.
        {
            .id = 1201,
            .symbol = "ES-CD",
            .strategy_type = StrategyType::Condor,
            .legs = {
                {.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 7, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 8, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 5, .lot_size = 10000},
            },
            // Tick: GCD(2500, 2500, 2500, 5) = 5
        },
    };
}

// get_cme_crack_spreads -- inter-commodity spreads.
//
// Crack spread: buy crude, sell product (gasoline/heating oil).
// In production: 3:2:1 crack = 3 CL - 2 RB - 1 HO.
// Simulator uses CL(3) vs GC(4) as a proxy inter-commodity pair.
inline std::vector<SpreadInstrumentConfig> get_cme_crack_spreads() {
    return {
        // CL-GC inter-commodity: +1 CL(3), -1 GC(4)
        // CL tick=100, GC tick=1000
        {
            .id = 1301,
            .symbol = "CL-GC-CRACK",
            .strategy_type = StrategyType::InterCommodity,
            .legs = {
                {.instrument_id = 3, .ratio = 1,  .price_multiplier = 1,
                 .tick_size = 100, .lot_size = 10000},
                {.instrument_id = 4, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 1000, .lot_size = 10000},
            },
            // Tick: GCD(100, 1000) = 100
        },
    };
}

// get_all_cme_spreads -- all CME spread definitions combined.
inline std::vector<SpreadInstrumentConfig> get_all_cme_spreads() {
    auto result = get_cme_calendar_spreads();
    auto bf = get_cme_butterfly_spreads();
    auto cd = get_cme_condor_spreads();
    auto cr = get_cme_crack_spreads();
    result.insert(result.end(), bf.begin(), bf.end());
    result.insert(result.end(), cd.begin(), cd.end());
    result.insert(result.end(), cr.begin(), cr.end());
    return result;
}

}  // namespace cme
}  // namespace exchange
