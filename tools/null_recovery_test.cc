#include "tools/display_state.h"
#include "tools/null_recovery.h"

#include "gtest/gtest.h"

// NullRecovery::recover() must leave DisplayState completely unchanged.
TEST(NullRecovery, LeavesDisplayStateUnchanged) {
    DisplayState ds{};
    // Seed some state to verify it is NOT cleared.
    ds.bid_levels = 2;
    ds.bids[0] = BookLevel{5000, 10, 1};
    ds.bids[1] = BookLevel{4900, 20, 3};
    ds.ask_levels = 1;
    ds.asks[0] = BookLevel{5100, 5, 1};
    ds.total_messages = 42;

    NullRecovery recovery;
    recovery.recover("ES", ds);

    // Everything must be untouched.
    EXPECT_EQ(ds.bid_levels, 2);
    EXPECT_EQ(ds.bids[0].price, 5000);
    EXPECT_EQ(ds.bids[0].qty, 10);
    EXPECT_EQ(ds.bids[1].price, 4900);
    EXPECT_EQ(ds.ask_levels, 1);
    EXPECT_EQ(ds.asks[0].price, 5100);
    EXPECT_EQ(ds.total_messages, 42u);
}

// NullRecovery::recover() on a default-constructed DisplayState is also a no-op.
TEST(NullRecovery, EmptyStateRemainsEmpty) {
    DisplayState ds{};
    NullRecovery recovery;
    recovery.recover("ES", ds);

    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_EQ(ds.ask_levels, 0);
    EXPECT_EQ(ds.trade_count, 0);
    EXPECT_EQ(ds.total_messages, 0u);
}
