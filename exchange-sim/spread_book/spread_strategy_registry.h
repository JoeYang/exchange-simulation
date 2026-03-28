#pragma once

#include "exchange-sim/spread_book/spread_strategy.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace exchange {

// SpreadStrategyRegistry -- maps spread instrument IDs to their strategy
// definitions and validates structural invariants at registration time.
//
// Single-threaded: no concurrent access.  The registry is populated during
// instrument setup (before trading) and queried during order routing.
class SpreadStrategyRegistry {
public:
    // Callback to check whether a given outright instrument ID is known.
    // Set via set_instrument_validator() before registering strategies.
    using InstrumentValidator = std::function<bool(uint32_t)>;

    SpreadStrategyRegistry() = default;

    void set_instrument_validator(InstrumentValidator validator) {
        validator_ = std::move(validator);
    }

    // Register a spread strategy for the given spread_instrument_id.
    // Returns empty string on success, error message on failure.
    //
    // Validation:
    //   - spread_instrument_id must not already be registered.
    //   - Strategy must have >= 2 legs.
    //   - First leg ratio must be > 0 (convention).
    //   - All leg ratios must be non-zero.
    //   - All leg tick_size and lot_size must be > 0.
    //   - All leg instrument_ids must pass the instrument validator (if set).
    //   - Spread instrument ID must not collide with any leg instrument ID.
    std::string register_strategy(uint32_t spread_instrument_id,
                                  SpreadStrategy strategy) {
        if (strategies_.count(spread_instrument_id)) {
            return "Duplicate spread instrument ID: "
                + std::to_string(spread_instrument_id);
        }

        const auto& legs = strategy.legs();
        if (legs.size() < 2) {
            return "Strategy must have at least 2 legs";
        }
        if (legs[0].ratio <= 0) {
            return "First leg ratio must be positive (convention)";
        }
        for (size_t i = 0; i < legs.size(); ++i) {
            if (legs[i].ratio == 0) {
                return "Leg " + std::to_string(i) + " has zero ratio";
            }
            if (legs[i].tick_size <= 0) {
                return "Leg " + std::to_string(i) + " has invalid tick_size";
            }
            if (legs[i].lot_size <= 0) {
                return "Leg " + std::to_string(i) + " has invalid lot_size";
            }
            if (legs[i].instrument_id == spread_instrument_id) {
                return "Leg instrument ID cannot equal spread instrument ID";
            }
            if (validator_ && !validator_(legs[i].instrument_id)) {
                return "Unknown leg instrument ID: "
                    + std::to_string(legs[i].instrument_id);
            }
        }

        spread_ids_.insert(spread_instrument_id);
        strategies_.emplace(spread_instrument_id, std::move(strategy));
        return {};
    }

    // Lookup strategy by spread instrument ID.  Returns nullptr if not found.
    const SpreadStrategy* lookup(uint32_t spread_instrument_id) const {
        auto it = strategies_.find(spread_instrument_id);
        return it != strategies_.end() ? &it->second : nullptr;
    }

    // Check if an instrument ID is a registered spread.
    bool is_spread(uint32_t instrument_id) const {
        return spread_ids_.count(instrument_id) != 0;
    }

    size_t size() const { return strategies_.size(); }

private:
    InstrumentValidator validator_;
    std::unordered_map<uint32_t, SpreadStrategy> strategies_;
    std::unordered_set<uint32_t> spread_ids_;
};

}  // namespace exchange
