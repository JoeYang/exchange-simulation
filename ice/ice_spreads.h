#pragma once

#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "ice/ice_products.h"

#include <vector>

namespace exchange {
namespace ice {

// get_ice_spread_products -- canonical ICE Futures spread product table.
//
// Defines four standard ICE spread instruments:
//
//   B-CAL    Brent calendar spread       (Buy front, Sell back)
//   M-CAL    Natural Gas calendar spread  (Buy front, Sell back)
//   G-B-CRK  Gasoil/Brent crack spread   (Buy Gasoil, Sell Brent)
//   B-G-CRK  Brent/Gasoil crack spread   (Buy Brent, Sell Gasoil)
//
// Leg instrument IDs reference the outright product table (get_ice_products):
//   B=1, G=2, M=3.  Calendar spreads use the same instrument_id for both
//   legs (same product, different expiry -- expiry selection is handled at
//   the exchange session level, not in the strategy definition).
//
// Spread instrument IDs start at 2001 to avoid collision with outrights (1-10)
// and leave room for future outright additions.
//
// Tick/lot sizes on each leg are copied from the outright product config.
// Spread tick is auto-computed as GCD of leg tick contributions.
inline std::vector<SpreadInstrumentConfig> get_ice_spread_products() {
    // Outright tick/lot constants (4-decimal-place fixed-point).
    // Brent (id=1): $0.01/bbl tick
    // Gasoil (id=2): $0.25/tonne tick
    // Natural Gas (id=3): 0.01 p/therm tick
    constexpr Price    brent_tick  = 100;
    constexpr Quantity brent_lot   = 10000;
    constexpr Price    gasoil_tick = 2500;
    constexpr Quantity gasoil_lot  = 10000;
    constexpr Price    natgas_tick = 100;
    constexpr Quantity natgas_lot  = 10000;

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

    // Brent calendar spread: Buy B front, Sell B back
    // Spread tick = GCD(100*1, 100*1) = 100 ($0.01/bbl)
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 2001;
        cfg.symbol        = "B-CAL";
        cfg.strategy_type = StrategyType::CalendarSpread;
        cfg.legs = {
            make_leg(1, +1, 1, brent_tick, brent_lot),
            make_leg(1, -1, 1, brent_tick, brent_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    // Natural Gas (NBP) calendar spread: Buy M front, Sell M back
    // Spread tick = GCD(100*1, 100*1) = 100 (0.01 p/therm)
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 2002;
        cfg.symbol        = "M-CAL";
        cfg.strategy_type = StrategyType::CalendarSpread;
        cfg.legs = {
            make_leg(3, +1, 1, natgas_tick, natgas_lot),
            make_leg(3, -1, 1, natgas_tick, natgas_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    // Gasoil/Brent crack spread: Buy Gasoil, Sell Brent
    // Spread tick = GCD(2500*1, 100*1) = 100
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 2003;
        cfg.symbol        = "G-B-CRK";
        cfg.strategy_type = StrategyType::InterCommodity;
        cfg.legs = {
            make_leg(2, +1, 1, gasoil_tick, gasoil_lot),
            make_leg(1, -1, 1, brent_tick,  brent_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    // Brent/Gasoil crack spread: Buy Brent, Sell Gasoil
    // Spread tick = GCD(100*1, 2500*1) = 100
    {
        SpreadInstrumentConfig cfg;
        cfg.id            = 2004;
        cfg.symbol        = "B-G-CRK";
        cfg.strategy_type = StrategyType::InterCommodity;
        cfg.legs = {
            make_leg(1, +1, 1, brent_tick,  brent_lot),
            make_leg(2, -1, 1, gasoil_tick, gasoil_lot),
        };
        spreads.push_back(std::move(cfg));
    }

    return spreads;
}

}  // namespace ice
}  // namespace exchange
