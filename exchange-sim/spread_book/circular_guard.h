#pragma once

#include "exchange-core/types.h"

#include <cstdint>

namespace exchange {

// CircularGuard prevents infinite loops in implied price propagation.
//
// When spread A depends on outrights that also participate in spread B,
// and spread B depends on outrights from spread A, recalculating implied
// prices can cycle indefinitely: A updates -> B recalcs -> A recalcs -> ...
//
// This guard provides two termination conditions:
//   1. Depth limit: hard cap on propagation depth (default 3).
//   2. Convergence: prices unchanged between iterations (early exit).
//
// Usage:
//   CircularGuard guard(max_depth);
//   while (guard.enter()) {
//       // Recalculate implied prices...
//       if (no_prices_changed) {
//           guard.mark_converged();
//           break;
//       }
//   }
//
// Single-threaded, no allocation, ~40 bytes on the stack.

class CircularGuard {
public:
    static constexpr uint32_t kDefaultMaxDepth = 3;

    explicit CircularGuard(uint32_t max_depth = kDefaultMaxDepth) noexcept
        : max_depth_(max_depth) {}

    // Attempt to enter the next propagation level.
    // Returns true if depth < max_depth and not yet converged.
    // Returns false if the depth limit is reached or convergence was signaled.
    // Increments current depth on success.
    bool enter() noexcept {
        if (converged_ || current_depth_ >= max_depth_) return false;
        ++current_depth_;
        return true;
    }

    // Signal that prices have converged (no changes in this iteration).
    // Subsequent enter() calls will return false.
    void mark_converged() noexcept { converged_ = true; }

    // Reset for a new propagation cycle (e.g., next BBO update).
    void reset() noexcept {
        current_depth_ = 0;
        converged_ = false;
    }

    // --- Accessors ---

    uint32_t current_depth() const noexcept { return current_depth_; }
    uint32_t max_depth() const noexcept { return max_depth_; }
    bool is_converged() const noexcept { return converged_; }
    bool is_exhausted() const noexcept { return current_depth_ >= max_depth_; }

private:
    uint32_t max_depth_{kDefaultMaxDepth};
    uint32_t current_depth_{0};
    bool converged_{false};
};

}  // namespace exchange
