#include "exchange-core/rate_tracker.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

constexpr int64_t ONE_SECOND_NS = 1'000'000'000;

// ---------------------------------------------------------------------------
// Basic acceptance: messages within limit
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, WithinLimitAccepted) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 5,
                                           .interval_ns = ONE_SECOND_NS});

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(tracker.check_and_increment(1, 100 + i))
            << "message " << i << " should be accepted";
    }
    EXPECT_EQ(tracker.count_for(1), 5);
}

// ---------------------------------------------------------------------------
// Exceeding limit: N+1 message rejected
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, ExceedingLimitRejected) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 3,
                                           .interval_ns = ONE_SECOND_NS});

    EXPECT_TRUE(tracker.check_and_increment(1, 100));
    EXPECT_TRUE(tracker.check_and_increment(1, 200));
    EXPECT_TRUE(tracker.check_and_increment(1, 300));
    // 4th message within the same window: rejected
    EXPECT_FALSE(tracker.check_and_increment(1, 400));
    // Counter stays at 3 (rejection does not increment)
    EXPECT_EQ(tracker.count_for(1), 3);
}

// ---------------------------------------------------------------------------
// Window slides: counter resets after interval passes
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, WindowSlides) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 2,
                                           .interval_ns = ONE_SECOND_NS});

    // Fill window
    EXPECT_TRUE(tracker.check_and_increment(1, 100));
    EXPECT_TRUE(tracker.check_and_increment(1, 200));
    EXPECT_FALSE(tracker.check_and_increment(1, 300));

    // After window expires: accepted, counter resets
    Timestamp after_window = 100 + ONE_SECOND_NS;
    EXPECT_TRUE(tracker.check_and_increment(1, after_window));
    EXPECT_EQ(tracker.count_for(1), 1);

    // Can send another in new window
    EXPECT_TRUE(tracker.check_and_increment(1, after_window + 1));
    EXPECT_EQ(tracker.count_for(1), 2);

    // But not a third
    EXPECT_FALSE(tracker.check_and_increment(1, after_window + 2));
}

// ---------------------------------------------------------------------------
// Multiple accounts: independent tracking
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, MultipleAccountsIndependent) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 2,
                                           .interval_ns = ONE_SECOND_NS});

    // Account 1 fills its limit
    EXPECT_TRUE(tracker.check_and_increment(1, 100));
    EXPECT_TRUE(tracker.check_and_increment(1, 200));
    EXPECT_FALSE(tracker.check_and_increment(1, 300));

    // Account 2 is unaffected
    EXPECT_TRUE(tracker.check_and_increment(2, 300));
    EXPECT_TRUE(tracker.check_and_increment(2, 400));
    EXPECT_FALSE(tracker.check_and_increment(2, 500));

    EXPECT_EQ(tracker.count_for(1), 2);
    EXPECT_EQ(tracker.count_for(2), 2);
}

// ---------------------------------------------------------------------------
// account_id == 0: never throttled (system/untagged messages)
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, AccountZeroNeverThrottled) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 1,
                                           .interval_ns = ONE_SECOND_NS});

    // Send many messages as account 0 -- all accepted
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(tracker.check_and_increment(0, 100 + i));
    }
}

// ---------------------------------------------------------------------------
// Boundary: exact limit count (N-th accepted, N+1-th rejected)
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, ExactLimitBoundary) {
    constexpr int64_t LIMIT = 10;
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = LIMIT,
                                           .interval_ns = ONE_SECOND_NS});

    for (int64_t i = 0; i < LIMIT; ++i) {
        EXPECT_TRUE(tracker.check_and_increment(1, 100 + i))
            << "message " << i << " should be accepted (limit=" << LIMIT << ")";
    }

    // N+1 rejected
    EXPECT_FALSE(tracker.check_and_increment(1, 100 + LIMIT));
}

// ---------------------------------------------------------------------------
// Boundary: timestamp exactly at window edge
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, TimestampAtWindowEdge) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 2,
                                           .interval_ns = ONE_SECOND_NS});

    Timestamp start = 1000;
    EXPECT_TRUE(tracker.check_and_increment(1, start));
    EXPECT_TRUE(tracker.check_and_increment(1, start + 1));
    EXPECT_FALSE(tracker.check_and_increment(1, start + 2));

    // Exactly at window boundary (start + interval_ns): new window begins
    Timestamp edge = start + ONE_SECOND_NS;
    EXPECT_TRUE(tracker.check_and_increment(1, edge));
    EXPECT_EQ(tracker.count_for(1), 1);
}

// ---------------------------------------------------------------------------
// Disabled config: all messages accepted
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, DisabledConfigAcceptsAll) {
    // max_messages_per_interval = 0 means disabled
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 0,
                                           .interval_ns = ONE_SECOND_NS});

    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(tracker.check_and_increment(1, 100 + i));
    }
}

TEST(RateTrackerTest, DisabledConfigZeroInterval) {
    // interval_ns = 0 means disabled
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 5,
                                           .interval_ns = 0});

    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(tracker.check_and_increment(1, 100 + i));
    }
}

// ---------------------------------------------------------------------------
// Out-of-range account_id rejected
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, OutOfRangeAccountRejected) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 5,
                                           .interval_ns = ONE_SECOND_NS});

    // account_id >= MaxAccounts is rejected
    EXPECT_FALSE(tracker.check_and_increment(16, 100));
    EXPECT_FALSE(tracker.check_and_increment(100, 100));
}

// ---------------------------------------------------------------------------
// Reset clears all counters
// ---------------------------------------------------------------------------

TEST(RateTrackerTest, ResetClearsCounters) {
    RateTracker<16> tracker(ThrottleConfig{.max_messages_per_interval = 2,
                                           .interval_ns = ONE_SECOND_NS});

    EXPECT_TRUE(tracker.check_and_increment(1, 100));
    EXPECT_TRUE(tracker.check_and_increment(1, 200));
    EXPECT_FALSE(tracker.check_and_increment(1, 300));

    tracker.reset();

    // After reset, account can send again (new window will start)
    EXPECT_TRUE(tracker.check_and_increment(1, 400));
    EXPECT_EQ(tracker.count_for(1), 1);
}

}  // namespace
}  // namespace exchange
