#include "exchange-core/trade_registry.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Use a small capacity for testing boundary conditions
using TestRegistry = TradeRegistry<16>;

class TradeRegistryTest : public ::testing::Test {
protected:
    TestRegistry registry_;
};

// ---------------------------------------------------------------------------
// Register and look up a single trade
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, RegisterAndLookupSingleTrade) {
    TradeId id = registry_.record(/*aggressor_id=*/10, /*resting_id=*/20,
                                  /*price=*/1000000, /*quantity=*/50000,
                                  /*ts=*/100);
    ASSERT_NE(id, 0u);
    EXPECT_EQ(id, 1u);

    auto record = registry_.lookup(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->trade_id, 1u);
    EXPECT_EQ(record->aggressor_id, 10u);
    EXPECT_EQ(record->resting_id, 20u);
    EXPECT_EQ(record->price, 1000000);
    EXPECT_EQ(record->quantity, 50000);
    EXPECT_EQ(record->ts, 100);
    EXPECT_FALSE(record->busted);
}

// ---------------------------------------------------------------------------
// Lookup non-existent trade returns nullopt
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, LookupNonExistentReturnsNullopt) {
    EXPECT_FALSE(registry_.lookup(0).has_value());
    EXPECT_FALSE(registry_.lookup(1).has_value());
    EXPECT_FALSE(registry_.lookup(999).has_value());
}

// ---------------------------------------------------------------------------
// Register multiple trades, verify all retrievable
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, RegisterMultipleTradesAllRetrievable) {
    TradeId id1 = registry_.record(1, 2, 1000000, 10000, 100);
    TradeId id2 = registry_.record(3, 4, 2000000, 20000, 200);
    TradeId id3 = registry_.record(5, 6, 3000000, 30000, 300);

    EXPECT_EQ(id1, 1u);
    EXPECT_EQ(id2, 2u);
    EXPECT_EQ(id3, 3u);

    auto r1 = registry_.lookup(id1);
    auto r2 = registry_.lookup(id2);
    auto r3 = registry_.lookup(id3);

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    EXPECT_EQ(r1->aggressor_id, 1u);
    EXPECT_EQ(r2->aggressor_id, 3u);
    EXPECT_EQ(r3->aggressor_id, 5u);

    EXPECT_EQ(r1->price, 1000000);
    EXPECT_EQ(r2->price, 2000000);
    EXPECT_EQ(r3->price, 3000000);

    EXPECT_EQ(registry_.trade_count(), 3u);
}

// ---------------------------------------------------------------------------
// Capacity exhausted returns 0
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, CapacityExhaustedReturnsZero) {
    // Fill up the registry (IDs 1..15, since MaxTrades=16 and index 0 unused)
    for (size_t i = 0; i < 15; ++i) {
        TradeId id = registry_.record(i, i + 1, 1000000, 10000, 100);
        ASSERT_NE(id, 0u) << "Failed at trade " << i;
    }

    // Next record should fail -- next_trade_id_ == 16 == MaxTrades
    TradeId overflow = registry_.record(99, 100, 1000000, 10000, 100);
    EXPECT_EQ(overflow, 0u);
}

// ---------------------------------------------------------------------------
// mark_busted succeeds once, fails on second call
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, MarkBustedSucceedsOnceThenFails) {
    TradeId id = registry_.record(1, 2, 1000000, 10000, 100);

    EXPECT_TRUE(registry_.mark_busted(id));

    // Second bust of same trade should fail
    EXPECT_FALSE(registry_.mark_busted(id));

    // Verify busted flag is set
    auto record = registry_.lookup(id);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->busted);
}

// ---------------------------------------------------------------------------
// mark_busted on non-existent trade returns false
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, MarkBustedNonExistentReturnsFalse) {
    EXPECT_FALSE(registry_.mark_busted(0));
    EXPECT_FALSE(registry_.mark_busted(1));
    EXPECT_FALSE(registry_.mark_busted(999));
}

// ---------------------------------------------------------------------------
// trade_count tracks registrations
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, TradeCountTracksRegistrations) {
    EXPECT_EQ(registry_.trade_count(), 0u);

    registry_.record(1, 2, 1000000, 10000, 100);
    EXPECT_EQ(registry_.trade_count(), 1u);

    registry_.record(3, 4, 2000000, 20000, 200);
    EXPECT_EQ(registry_.trade_count(), 2u);
}

// ---------------------------------------------------------------------------
// Sequential IDs start at 1
// ---------------------------------------------------------------------------

TEST_F(TradeRegistryTest, SequentialIdsStartAtOne) {
    EXPECT_EQ(registry_.next_trade_id(), 1u);

    TradeId id1 = registry_.record(1, 2, 1000000, 10000, 100);
    EXPECT_EQ(id1, 1u);
    EXPECT_EQ(registry_.next_trade_id(), 2u);

    TradeId id2 = registry_.record(3, 4, 2000000, 20000, 200);
    EXPECT_EQ(id2, 2u);
    EXPECT_EQ(registry_.next_trade_id(), 3u);
}

}  // namespace
}  // namespace exchange
