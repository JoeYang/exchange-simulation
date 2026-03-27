#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "exchange-core/types.h"

namespace exchange {

// Per-account net position tracker for pre-trade position limit checks.
//
// Tracks net position (long fills - short fills) per account, updated on
// each fill. Provides would_exceed_limit() for pre-trade checks.
//
// Design decisions:
//   - Fixed-size array indexed by account_id: O(1) lookup, zero allocation
//     on the hot path. account_id must be < MaxAccounts.
//   - account_id == 0 is never tracked (convention: system/untagged).
//   - Positions are signed: positive = net long, negative = net short.
//   - No internal limit storage -- limits are passed per-check by the
//     engine's CRTP hook, allowing exchange-specific limit policies.
//
// Performance: ~2ns per update_fill or would_exceed_limit on modern x86
// (L1 cache hit for active accounts).

template <size_t MaxAccounts = 4096>
class PositionTracker {
public:
    PositionTracker() noexcept { reset(); }

    // Update position after a fill. Buy fills increase position (long),
    // sell fills decrease position (short).
    void update_fill(uint64_t account_id, Side side,
                     Quantity qty) noexcept {
        if (account_id == 0 || account_id >= MaxAccounts) return;
        if (side == Side::Buy) {
            positions_[account_id] += qty;
        } else {
            positions_[account_id] -= qty;
        }
    }

    // Reverse a fill (e.g. after trade bust). Undoes the effect of
    // update_fill for the same parameters.
    void reverse_fill(uint64_t account_id, Side side,
                      Quantity qty) noexcept {
        if (account_id == 0 || account_id >= MaxAccounts) return;
        if (side == Side::Buy) {
            positions_[account_id] -= qty;
        } else {
            positions_[account_id] += qty;
        }
    }

    // Check whether adding a fill of (side, qty) for account_id would
    // cause the net position to exceed the given limit.
    //
    // For buy orders: checks if (current_position + qty) > limit
    // For sell orders: checks if (current_position - qty) < -limit
    //
    // A limit of 0 means "no limit" (always returns false).
    [[nodiscard]] bool would_exceed_limit(uint64_t account_id, Side side,
                                          Quantity qty,
                                          Quantity limit) const noexcept {
        if (limit <= 0) return false;  // disabled
        if (account_id == 0 || account_id >= MaxAccounts) return false;

        int64_t pos = positions_[account_id];
        if (side == Side::Buy) {
            return (pos + qty) > limit;
        } else {
            return (pos - qty) < -limit;
        }
    }

    // Get current net position for an account.
    // Positive = net long, negative = net short.
    [[nodiscard]] int64_t net_position(uint64_t account_id) const noexcept {
        if (account_id == 0 || account_id >= MaxAccounts) return 0;
        return positions_[account_id];
    }

    // Reset all positions to zero.
    void reset() noexcept {
        positions_.fill(0);
    }

private:
    std::array<int64_t, MaxAccounts> positions_;
};

}  // namespace exchange
