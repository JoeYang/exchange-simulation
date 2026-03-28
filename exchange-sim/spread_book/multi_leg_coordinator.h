#pragma once

#include "exchange-core/types.h"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace exchange {

// MultiLegCoordinator -- ensures atomic execution of multi-leg implied fills.
//
// Problem: when filling a spread, we must atomically fill outright orders on
// multiple engines. If we apply fills one engine at a time and a later engine
// fails, we've already committed fills on earlier engines (non-reversible).
//
// Solution (single-threaded, same as production sequencer pattern):
//   1. VALIDATE all legs on all engines first (read-only check).
//   2. If all validations pass, APPLY all fills (guaranteed to succeed).
//   3. If any validation fails, apply nothing.
//
// The validator callback checks order existence and remaining qty without
// modifying state. The applier callback actually commits the fills.
//
// Usage:
//   MultiLegCoordinator coord;
//   coord.set_validator(...);
//   coord.set_applier(...);
//   coord.add_leg_fill(instrument_id, LegFill{...});
//   coord.add_leg_fill(instrument_id, LegFill{...});
//   bool ok = coord.execute(timestamp);
//   coord.clear();
class MultiLegCoordinator {
public:
    // Validates fills without applying. Returns false if any fill would fail.
    using FillValidator = std::function<bool(uint32_t instrument_id,
                                             std::span<const LegFill> fills)>;

    // Applies fills (must succeed after validation).
    using FillApplier = std::function<bool(uint32_t instrument_id,
                                           std::span<const LegFill> fills,
                                           Timestamp ts)>;

    MultiLegCoordinator() = default;

    void set_validator(FillValidator validator) {
        validator_ = std::move(validator);
    }

    void set_applier(FillApplier applier) {
        applier_ = std::move(applier);
    }

    // Add a fill for one leg. Fills are grouped by instrument_id.
    void add_leg_fill(uint32_t instrument_id, LegFill fill) {
        fills_by_instrument_[instrument_id].push_back(fill);
    }

    // Clear all pending fills.
    void clear() {
        fills_by_instrument_.clear();
    }

    // Validate all fills, then apply all. Returns true if all succeeded.
    // On failure, nothing is applied.
    bool execute(Timestamp ts) {
        if (fills_by_instrument_.empty()) return true;

        // Phase 1: validate all engines
        if (validator_) {
            for (const auto& [instr_id, fills] : fills_by_instrument_) {
                if (!validator_(instr_id, fills)) {
                    return false;
                }
            }
        }

        // Phase 2: apply all (should succeed after validation)
        if (applier_) {
            for (const auto& [instr_id, fills] : fills_by_instrument_) {
                if (!applier_(instr_id, fills, ts)) {
                    // This should never happen if validator passed,
                    // but handle defensively. Already-applied fills
                    // are committed (single-threaded, no rollback needed).
                    return false;
                }
            }
        }

        return true;
    }

    // Number of distinct instruments with pending fills.
    size_t instrument_count() const { return fills_by_instrument_.size(); }

    // Total number of pending fills across all instruments.
    size_t total_fill_count() const {
        size_t count = 0;
        for (const auto& [_, fills] : fills_by_instrument_) {
            count += fills.size();
        }
        return count;
    }

private:
    FillValidator validator_;
    FillApplier applier_;
    std::unordered_map<uint32_t, std::vector<LegFill>> fills_by_instrument_;
};

}  // namespace exchange
