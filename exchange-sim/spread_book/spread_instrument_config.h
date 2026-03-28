#pragma once

#include "exchange-core/types.h"
#include "exchange-sim/spread_book/spread_strategy.h"
#include "exchange-sim/spread_book/spread_strategy_registry.h"

#include <string>
#include <vector>

namespace exchange {

// SpreadInstrumentConfig -- security definition for a spread instrument.
//
// Parallels InstrumentConfig for outright instruments.  The spread's tick_size
// and lot_size are either auto-computed by SpreadStrategy from its legs or
// explicitly overridden here.
//
// The engine_tick_size / engine_lot_size fields configure the EngineConfig
// for the spread's own OrderBook (used by SpreadBook).
struct SpreadInstrumentConfig {
    uint32_t    id{0};              // spread instrument ID (unique across all instruments)
    std::string symbol;             // e.g. "ESU5-ESZ5"
    StrategyType strategy_type{StrategyType::CalendarSpread};
    std::vector<StrategyLeg> legs;  // leg definitions with tick/lot from outrights

    // Override spread tick/lot (0 = auto-compute from legs).
    Price    tick_size_override{0};
    Quantity lot_size_override{0};

    // Build a SpreadStrategy from this config.
    SpreadStrategy build_strategy() const {
        return SpreadStrategy(strategy_type, legs,
                              tick_size_override, lot_size_override);
    }

    // Register this spread instrument's strategy in the registry.
    // Returns empty string on success, error message on failure.
    std::string register_in(SpreadStrategyRegistry& registry) const {
        return registry.register_strategy(id, build_strategy());
    }
};

}  // namespace exchange
