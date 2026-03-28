#include "exchange-sim/spread_book/circular_guard.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

TEST(CircularGuardTest, DepthLimitDefault) {
    CircularGuard guard;
    EXPECT_EQ(guard.max_depth(), 3u);
    EXPECT_EQ(guard.current_depth(), 0u);
    EXPECT_FALSE(guard.is_converged());
    EXPECT_FALSE(guard.is_exhausted());
}

TEST(CircularGuardTest, EnterUpToMaxDepth) {
    CircularGuard guard(3);
    EXPECT_TRUE(guard.enter());   // depth=1
    EXPECT_TRUE(guard.enter());   // depth=2
    EXPECT_TRUE(guard.enter());   // depth=3
    EXPECT_FALSE(guard.enter());  // blocked
    EXPECT_EQ(guard.current_depth(), 3u);
    EXPECT_TRUE(guard.is_exhausted());
    EXPECT_FALSE(guard.is_converged());
}

TEST(CircularGuardTest, EarlyConvergence) {
    CircularGuard guard(5);
    EXPECT_TRUE(guard.enter());   // depth=1
    guard.mark_converged();
    EXPECT_FALSE(guard.enter());  // converged, no more iterations
    EXPECT_TRUE(guard.is_converged());
    EXPECT_EQ(guard.current_depth(), 1u);
}

TEST(CircularGuardTest, ResetClearsState) {
    CircularGuard guard(2);
    EXPECT_TRUE(guard.enter());
    EXPECT_TRUE(guard.enter());
    EXPECT_FALSE(guard.enter());  // exhausted
    EXPECT_TRUE(guard.is_exhausted());

    guard.reset();
    EXPECT_EQ(guard.current_depth(), 0u);
    EXPECT_FALSE(guard.is_converged());
    EXPECT_FALSE(guard.is_exhausted());
    EXPECT_TRUE(guard.enter());  // can enter again
}

TEST(CircularGuardTest, ResetAfterConvergence) {
    CircularGuard guard(5);
    EXPECT_TRUE(guard.enter());
    guard.mark_converged();
    EXPECT_FALSE(guard.enter());

    guard.reset();
    EXPECT_FALSE(guard.is_converged());
    EXPECT_TRUE(guard.enter());
}

TEST(CircularGuardTest, ZeroMaxDepth) {
    CircularGuard guard(0);
    EXPECT_FALSE(guard.enter());  // cannot enter at all
    EXPECT_TRUE(guard.is_exhausted());
}

TEST(CircularGuardTest, SingleDepth) {
    CircularGuard guard(1);
    EXPECT_TRUE(guard.enter());   // depth=1
    EXPECT_FALSE(guard.enter());  // exhausted
    EXPECT_EQ(guard.current_depth(), 1u);
}

}  // namespace
}  // namespace exchange
