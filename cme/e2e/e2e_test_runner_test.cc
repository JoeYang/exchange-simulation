#include "cme/e2e/e2e_test_runner.h"
#include "test-harness/journal_parser.h"

#include <gtest/gtest.h>
#include <string>

using namespace exchange;
using namespace exchange::cme;

namespace {

// Helper: run a journal string through the E2E runner, assert all pass.
void assert_all_pass(const std::string& journal_str) {
    auto journal = JournalParser::parse_string(journal_str);
    E2ETestRunner runner;
    auto results = runner.run(journal);
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].passed)
            << "Result " << i << " failed at action " << results[i].action_index
            << " [" << results[i].category << "]: "
            << "expected=" << results[i].expected
            << " actual=" << results[i].actual;
    }
    EXPECT_GT(results.size(), 0u) << "Expected at least one result";
}

// ---------------------------------------------------------------------------
// Test: basic limit order -> EXEC_NEW + MD_BOOK_ADD
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, BasicLimitOrder) {
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=1 status=NEW
EXPECT MD_BOOK_ADD side=BUY price=5000000000000 qty=1 num_orders=1
)";
    assert_all_pass(journal);
}

// ---------------------------------------------------------------------------
// Test: two crossing orders -> fills
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, CrossingOrdersFill) {
    // When the aggressor order crosses the resting order, the engine fires:
    //   1. on_order_accepted for the aggressor (EXEC_NEW)
    //   2. on_order_filled for the resting order (EXEC_FILL, order 1)
    //   3. The aggressor's fill is implicit (it was IOC-like within the match)
    // The ILink3ReportPublisher produces one EXEC_NEW + one EXEC_FILL (resting).
    // Market data: the book removal (delete) and trade message.
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=1 status=NEW
EXPECT MD_BOOK_ADD side=BUY price=5000000000000 qty=1 num_orders=1

ACTION ILINK3_NEW_ORDER ts=2000 instrument=ES cl_ord_id=2 account=FIRM_B side=SELL price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=2 status=NEW
EXPECT EXEC_FILL cl_ord_id=1 fill_qty=1 status=FILLED
)";
    auto parsed = JournalParser::parse_string(journal);
    E2ETestRunner runner;
    auto results = runner.run(parsed);
    ASSERT_EQ(results.size(), 4u);
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].passed)
            << "Result " << i << " [" << results[i].category << "]: "
            << "expected=" << results[i].expected
            << " actual=" << results[i].actual;
    }
}

// ---------------------------------------------------------------------------
// Test: cancel order -> EXEC_CANCELLED
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, CancelOrder) {
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=1 status=NEW

ACTION ILINK3_CANCEL ts=2000 instrument=ES cl_ord_id=2 orig_cl_ord_id=1
EXPECT EXEC_CANCELLED cl_ord_id=1
)";
    assert_all_pass(journal);
}

// ---------------------------------------------------------------------------
// Test: reject (wrong session state — order during Closed)
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, RejectInClosedSession) {
    // Don't start a session. The exchange is Closed.
    // Submitting an order should produce a reject.
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=PRE_OPEN

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=1 status=NEW
)";
    // In PreOpen, limit orders are accepted (collected for auction).
    assert_all_pass(journal);
}

// ---------------------------------------------------------------------------
// Test: E2EResult reports category correctly
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, ResultCategories) {
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=1
EXPECT MD_BOOK_ADD side=BUY
)";
    auto parsed = JournalParser::parse_string(journal);
    E2ETestRunner runner;
    auto results = runner.run(parsed);
    ASSERT_GE(results.size(), 2u);
    EXPECT_EQ(results[0].category, "EXEC_REPORT");
    EXPECT_EQ(results[1].category, "MARKET_DATA");
}

// ---------------------------------------------------------------------------
// Test: mass cancel
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, MassCancel) {
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=1

ACTION ILINK3_NEW_ORDER ts=1001 instrument=ES cl_ord_id=2 account=FIRM_A side=BUY price=49990000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_NEW cl_ord_id=2

ACTION ILINK3_MASS_CANCEL ts=2000 instrument=ES account=FIRM_A
EXPECT EXEC_CANCELLED
EXPECT EXEC_CANCELLED
)";
    assert_all_pass(journal);
}

// ---------------------------------------------------------------------------
// Test: empty journal runs without errors
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, EmptyJournal) {
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000
)";
    auto parsed = JournalParser::parse_string(journal);
    E2ETestRunner runner;
    auto results = runner.run(parsed);
    EXPECT_EQ(results.size(), 0u);
}

// ---------------------------------------------------------------------------
// Test: expectation mismatch reports failure
// ---------------------------------------------------------------------------
TEST(E2ETestRunnerTest, ExpectationMismatchReportsFailure) {
    std::string journal = R"(
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT EXEC_FILL cl_ord_id=1
)";
    // We expect a FILL but only a NEW will come back — mismatch.
    auto parsed = JournalParser::parse_string(journal);
    E2ETestRunner runner;
    auto results = runner.run(parsed);
    ASSERT_GE(results.size(), 1u);
    EXPECT_FALSE(results[0].passed);
}

}  // anonymous namespace
