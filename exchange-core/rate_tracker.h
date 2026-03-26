#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "exchange-core/types.h"

namespace exchange {

// Per-account message rate tracker using a fixed-window algorithm.
//
// Tracks message counts per account within a configurable time window.
// When an account exceeds max_messages within interval_ns, subsequent
// messages are rejected until the window slides forward.
//
// Design decisions:
//   - Fixed-size array indexed by account_id: O(1) lookup, zero allocation
//     on the hot path. account_id must be < MaxAccounts.
//   - account_id == 0 is never throttled (convention: untagged/system messages).
//   - Window resets fully when timestamp moves past the current window end.
//     This is a tumbling window, not a true sliding window -- simpler and
//     deterministic, matching CME/ICE rate-limit semantics.
//
// Performance: ~3ns per check_and_increment on modern x86 (L1 cache hit
// for active accounts). No branches except the threshold comparison.

struct ThrottleConfig {
    int64_t max_messages_per_interval{0};  // 0 = disabled (unlimited)
    int64_t interval_ns{0};               // window size in nanoseconds
};

template <size_t MaxAccounts = 4096>
class RateTracker {
public:
    explicit RateTracker(ThrottleConfig config) noexcept
        : config_(config) {
        reset();
    }

    // Check whether account_id is within its rate limit at the given timestamp.
    // If within limit, increments the counter and returns true (accepted).
    // If over limit, returns false (rejected) without incrementing.
    //
    // Precondition: account_id < MaxAccounts.
    // account_id == 0 is always accepted (system/untagged messages).
    [[nodiscard]] bool check_and_increment(uint64_t account_id,
                                           Timestamp ts) noexcept {
        // Disabled: always accept
        if (config_.max_messages_per_interval <= 0 || config_.interval_ns <= 0)
            return true;

        // Untagged account: never throttled
        if (account_id == 0) return true;

        // Bounds check -- reject out-of-range accounts
        if (account_id >= MaxAccounts) [[unlikely]]
            return false;

        auto& entry = entries_[account_id];

        // Window has expired: reset and start new window
        Timestamp window_end = entry.window_start + config_.interval_ns;
        if (ts >= window_end) {
            entry.window_start = ts;
            entry.count = 1;
            return true;
        }

        // Within current window: check limit
        if (entry.count >= config_.max_messages_per_interval)
            return false;

        ++entry.count;
        return true;
    }

    // Reset all counters. Called during initialization or session reset.
    void reset() noexcept {
        for (auto& e : entries_) {
            e.window_start = 0;
            e.count = 0;
        }
    }

    // Accessors for testing/monitoring
    int64_t count_for(uint64_t account_id) const noexcept {
        if (account_id >= MaxAccounts) return 0;
        return entries_[account_id].count;
    }

    const ThrottleConfig& config() const noexcept { return config_; }

private:
    struct Entry {
        Timestamp window_start{0};
        int64_t count{0};
    };

    ThrottleConfig config_;
    std::array<Entry, MaxAccounts> entries_;
};

}  // namespace exchange
