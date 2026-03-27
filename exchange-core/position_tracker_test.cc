#include "exchange-core/position_tracker.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

class PositionTrackerTest : public ::testing::Test {
protected:
    PositionTracker<64> tracker_;
};

// ---------------------------------------------------------------------------
// 1. Buy fills increase long position
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, BuyFillIncreasesPosition) {
    tracker_.update_fill(1, Side::Buy, 10000);
    EXPECT_EQ(tracker_.net_position(1), 10000);

    tracker_.update_fill(1, Side::Buy, 5000);
    EXPECT_EQ(tracker_.net_position(1), 15000);
}

// ---------------------------------------------------------------------------
// 2. Sell fills decrease position (go short)
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, SellFillDecreasesPosition) {
    tracker_.update_fill(1, Side::Sell, 10000);
    EXPECT_EQ(tracker_.net_position(1), -10000);

    tracker_.update_fill(1, Side::Sell, 5000);
    EXPECT_EQ(tracker_.net_position(1), -15000);
}

// ---------------------------------------------------------------------------
// 3. Net position correctly computed (long - short)
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, NetPositionIsLongMinusShort) {
    tracker_.update_fill(1, Side::Buy, 30000);
    tracker_.update_fill(1, Side::Sell, 10000);
    EXPECT_EQ(tracker_.net_position(1), 20000);

    tracker_.update_fill(1, Side::Sell, 25000);
    EXPECT_EQ(tracker_.net_position(1), -5000);
}

// ---------------------------------------------------------------------------
// 4. would_exceed_limit returns true when adding qty would breach limit
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, WouldExceedLimitBuy) {
    tracker_.update_fill(1, Side::Buy, 90000);
    // At 90000, buying 20000 more -> 110000 > 100000 limit
    EXPECT_TRUE(tracker_.would_exceed_limit(1, Side::Buy, 20000, 100000));
}

TEST_F(PositionTrackerTest, WouldExceedLimitSell) {
    tracker_.update_fill(1, Side::Sell, 90000);
    // At -90000, selling 20000 more -> -110000 < -100000 limit
    EXPECT_TRUE(tracker_.would_exceed_limit(1, Side::Sell, 20000, 100000));
}

// ---------------------------------------------------------------------------
// 5. would_exceed_limit returns false within limit
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, WithinLimitBuy) {
    tracker_.update_fill(1, Side::Buy, 50000);
    // At 50000, buying 40000 more -> 90000 <= 100000
    EXPECT_FALSE(tracker_.would_exceed_limit(1, Side::Buy, 40000, 100000));
}

TEST_F(PositionTrackerTest, WithinLimitSell) {
    tracker_.update_fill(1, Side::Sell, 50000);
    // At -50000, selling 40000 more -> -90000 >= -100000
    EXPECT_FALSE(tracker_.would_exceed_limit(1, Side::Sell, 40000, 100000));
}

TEST_F(PositionTrackerTest, ExactlyAtLimitNotExceeded) {
    tracker_.update_fill(1, Side::Buy, 50000);
    // At 50000, buying 50000 more -> 100000 == 100000 (not exceeded)
    EXPECT_FALSE(tracker_.would_exceed_limit(1, Side::Buy, 50000, 100000));
}

// ---------------------------------------------------------------------------
// 6. Multiple accounts tracked independently
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, MultipleAccountsIndependent) {
    tracker_.update_fill(1, Side::Buy, 10000);
    tracker_.update_fill(2, Side::Sell, 20000);
    tracker_.update_fill(3, Side::Buy, 30000);

    EXPECT_EQ(tracker_.net_position(1), 10000);
    EXPECT_EQ(tracker_.net_position(2), -20000);
    EXPECT_EQ(tracker_.net_position(3), 30000);

    // Account 1 within limit, account 3 would exceed
    EXPECT_FALSE(tracker_.would_exceed_limit(1, Side::Buy, 5000, 20000));
    EXPECT_TRUE(tracker_.would_exceed_limit(3, Side::Buy, 5000, 30000));
}

// ---------------------------------------------------------------------------
// 7. Position after bust (reverse_fill)
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, ReverseFillUndo) {
    tracker_.update_fill(1, Side::Buy, 50000);
    EXPECT_EQ(tracker_.net_position(1), 50000);

    // Bust: reverse the buy fill
    tracker_.reverse_fill(1, Side::Buy, 50000);
    EXPECT_EQ(tracker_.net_position(1), 0);
}

TEST_F(PositionTrackerTest, ReverseSellFill) {
    tracker_.update_fill(1, Side::Sell, 30000);
    EXPECT_EQ(tracker_.net_position(1), -30000);

    tracker_.reverse_fill(1, Side::Sell, 30000);
    EXPECT_EQ(tracker_.net_position(1), 0);
}

// ---------------------------------------------------------------------------
// 8. Zero limit means disabled (never exceeds)
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, ZeroLimitDisabled) {
    tracker_.update_fill(1, Side::Buy, 999999999);
    EXPECT_FALSE(tracker_.would_exceed_limit(1, Side::Buy, 1, 0));
}

// ---------------------------------------------------------------------------
// 9. Account 0 is never tracked
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, AccountZeroNeverTracked) {
    tracker_.update_fill(0, Side::Buy, 10000);
    EXPECT_EQ(tracker_.net_position(0), 0);
    EXPECT_FALSE(tracker_.would_exceed_limit(0, Side::Buy, 10000, 1));
}

// ---------------------------------------------------------------------------
// 10. Out-of-range account handled gracefully
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, OutOfRangeAccountSafe) {
    tracker_.update_fill(999, Side::Buy, 10000);  // >= MaxAccounts(64)
    EXPECT_EQ(tracker_.net_position(999), 0);
    EXPECT_FALSE(tracker_.would_exceed_limit(999, Side::Buy, 10000, 1));
}

// ---------------------------------------------------------------------------
// 11. Reset clears all positions
// ---------------------------------------------------------------------------

TEST_F(PositionTrackerTest, ResetClearsAll) {
    tracker_.update_fill(1, Side::Buy, 50000);
    tracker_.update_fill(2, Side::Sell, 30000);

    tracker_.reset();

    EXPECT_EQ(tracker_.net_position(1), 0);
    EXPECT_EQ(tracker_.net_position(2), 0);
}

}  // namespace
}  // namespace exchange
