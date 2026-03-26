#include "ice/e2e/ice_e2e_test_runner.h"

#include <gtest/gtest.h>

namespace exchange::ice {
namespace {

// ---------------------------------------------------------------------------
// Helper: build a journal from inline text.
// ---------------------------------------------------------------------------
Journal make_journal(const std::string& text) {
    return JournalParser::parse_string(text);
}

// ---------------------------------------------------------------------------
// Basic limit order accepted
// ---------------------------------------------------------------------------
TEST(IceE2ETest, BasicNewOrderAccepted) {
    auto journal = make_journal(R"(
ACTION SESSION_START ts=100 state=CONTINUOUS
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1 account=42 side=BUY price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW cl_ord_id=1
)");

    IceE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].passed)
        << "expected: " << results[0].expected
        << " actual: " << results[0].actual;
}

// ---------------------------------------------------------------------------
// Two crossing orders produce a fill
// ---------------------------------------------------------------------------
TEST(IceE2ETest, CrossingOrdersFill) {
    auto journal = make_journal(R"(
ACTION SESSION_START ts=100 state=CONTINUOUS
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1 account=1 side=SELL price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW cl_ord_id=1
ACTION ICE_FIX_NEW_ORDER ts=2000 instrument=B cl_ord_id=2 account=2 side=BUY price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW cl_ord_id=2
)");

    IceE2ETestRunner runner;
    auto results = runner.run(journal);

    // Both orders should be accepted.
    bool all_passed = true;
    for (const auto& r : results) {
        if (!r.passed) {
            all_passed = false;
            break;
        }
    }
    EXPECT_TRUE(all_passed);
}

// ---------------------------------------------------------------------------
// Cancel order
// ---------------------------------------------------------------------------
TEST(IceE2ETest, CancelOrder) {
    auto journal = make_journal(R"(
ACTION SESSION_START ts=100 state=CONTINUOUS
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1 account=1 side=BUY price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW cl_ord_id=1
ACTION ICE_FIX_CANCEL ts=2000 instrument=B cl_ord_id=2 orig_cl_ord_id=1
EXPECT ICE_EXEC_CANCELLED
)");

    IceE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].passed)
        << "accept: " << results[0].expected << " vs " << results[0].actual;
    EXPECT_TRUE(results[1].passed)
        << "cancel: " << results[1].expected << " vs " << results[1].actual;
}

// ---------------------------------------------------------------------------
// Session lifecycle: pre-open rejects market order
// ---------------------------------------------------------------------------
TEST(IceE2ETest, PreOpenRejectsMarket) {
    auto journal = make_journal(R"(
ACTION SESSION_START ts=100 state=PRE_OPEN
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1 account=1 side=BUY price=0 qty=10000 type=MARKET tif=GTC
EXPECT ICE_EXEC_REJECTED
)");

    IceE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].passed)
        << results[0].expected << " vs " << results[0].actual;
}

// ---------------------------------------------------------------------------
// Multi-instrument isolation
// ---------------------------------------------------------------------------
TEST(IceE2ETest, MultiInstrumentIsolation) {
    auto journal = make_journal(R"(
ACTION SESSION_START ts=100 state=CONTINUOUS
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1 account=1 side=SELL price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW cl_ord_id=1
ACTION ICE_FIX_NEW_ORDER ts=2000 instrument=C cl_ord_id=2 account=2 side=BUY price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW cl_ord_id=2
)");

    IceE2ETestRunner runner;
    auto results = runner.run(journal);

    // Both accepted, no cross-instrument fill.
    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].passed);
    EXPECT_TRUE(results[1].passed);
}

// ---------------------------------------------------------------------------
// Runner returns results with correct action_index
// ---------------------------------------------------------------------------
TEST(IceE2ETest, ResultActionIndex) {
    auto journal = make_journal(R"(
ACTION SESSION_START ts=100 state=CONTINUOUS
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1 account=1 side=BUY price=800000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW
ACTION ICE_FIX_NEW_ORDER ts=2000 instrument=B cl_ord_id=2 account=2 side=BUY price=810000 qty=10000 type=LIMIT tif=GTC
EXPECT ICE_EXEC_NEW
)");

    IceE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 2u);
    // action_index 0 = SESSION_START (no expects), 1 = first order, 2 = second
    EXPECT_EQ(results[0].action_index, 1u);
    EXPECT_EQ(results[1].action_index, 2u);
}

}  // namespace
}  // namespace exchange::ice
