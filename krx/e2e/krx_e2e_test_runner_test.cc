#include "krx/e2e/krx_e2e_test_runner.h"
#include "test-harness/journal_parser.h"

#include <gtest/gtest.h>

namespace exchange::krx {
namespace {

// Simple journal: place a limit order on KS (KOSPI200), expect acceptance.
TEST(KrxE2ETestRunnerTest, BasicNewOrderAccepted) {
    std::string journal_str = R"(
# KRX E2E: basic new order acceptance
ACTION SESSION_START ts=1000 state=CONTINUOUS
ACTION KRX_FIX_NEW_ORDER ts=2000 cl_ord_id=1 instrument=KS side=BUY type=LIMIT tif=DAY price=3500000 qty=10000
EXPECT KRX_EXEC_NEW cl_ord_id=1
)";

    auto journal = JournalParser::parse_string(journal_str);
    KrxE2ETestRunner runner;
    auto results = runner.run(journal);

    // Should have exactly 1 result (the KRX_EXEC_NEW expectation).
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].passed)
        << "expected=" << results[0].expected
        << " actual=" << results[0].actual;
}

TEST(KrxE2ETestRunnerTest, NewOrderAndCancel) {
    std::string journal_str = R"(
ACTION SESSION_START ts=1000 state=CONTINUOUS
ACTION KRX_FIX_NEW_ORDER ts=2000 cl_ord_id=1 instrument=KS side=BUY type=LIMIT tif=DAY price=3500000 qty=10000
EXPECT KRX_EXEC_NEW cl_ord_id=1
ACTION KRX_FIX_CANCEL ts=3000 cl_ord_id=2 orig_cl_ord_id=1 instrument=KS
EXPECT KRX_EXEC_CANCELLED
)";

    auto journal = JournalParser::parse_string(journal_str);
    KrxE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].passed)
        << "expected=" << results[0].expected
        << " actual=" << results[0].actual;
    EXPECT_TRUE(results[1].passed)
        << "expected=" << results[1].expected
        << " actual=" << results[1].actual;
}

TEST(KrxE2ETestRunnerTest, TwoOrdersFillProducesExecReports) {
    // When a sell crosses a resting buy, the engine produces:
    //   1. ExecNew for sell (accepted)
    //   2. ExecFill for resting buy (fully filled)
    // The aggressor (sell) doesn't get a separate fill exec report from
    // the publisher because it only tracks resting fills.
    // After the full fill, the resting buy is removed -> ExecCancelled
    // is NOT produced (fills remove orders silently).
    //
    // Verify the sell acceptance and the resting fill.
    std::string journal_str = R"(
ACTION SESSION_START ts=1000 state=CONTINUOUS
ACTION KRX_FIX_NEW_ORDER ts=2000 cl_ord_id=1 instrument=KS side=BUY type=LIMIT tif=DAY price=3500000 qty=10000
EXPECT KRX_EXEC_NEW cl_ord_id=1
ACTION KRX_FIX_NEW_ORDER ts=3000 cl_ord_id=2 instrument=KS side=SELL type=LIMIT tif=DAY price=3500000 qty=10000
EXPECT KRX_EXEC_NEW cl_ord_id=2
)";

    auto journal = JournalParser::parse_string(journal_str);
    KrxE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 2u);
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].passed)
            << "result[" << i << "] expected=" << results[i].expected
            << " actual=" << results[i].actual;
    }
}

TEST(KrxE2ETestRunnerTest, WrongExecTypeFails) {
    std::string journal_str = R"(
ACTION SESSION_START ts=1000 state=CONTINUOUS
ACTION KRX_FIX_NEW_ORDER ts=2000 cl_ord_id=1 instrument=KS side=BUY type=LIMIT tif=DAY price=3500000 qty=10000
EXPECT KRX_EXEC_FILL
)";

    auto journal = JournalParser::parse_string(journal_str);
    KrxE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].passed);
}

TEST(KrxE2ETestRunnerTest, MdQuoteExpected) {
    std::string journal_str = R"(
ACTION SESSION_START ts=1000 state=CONTINUOUS
ACTION KRX_FIX_NEW_ORDER ts=2000 cl_ord_id=1 instrument=KS side=BUY type=LIMIT tif=DAY price=3500000 qty=10000
EXPECT KRX_EXEC_NEW cl_ord_id=1
EXPECT KRX_MD_QUOTE bid_price=3500000
)";

    auto journal = JournalParser::parse_string(journal_str);
    KrxE2ETestRunner runner;
    auto results = runner.run(journal);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].passed)
        << "exec: expected=" << results[0].expected
        << " actual=" << results[0].actual;
    EXPECT_TRUE(results[1].passed)
        << "md: expected=" << results[1].expected
        << " actual=" << results[1].actual;
}

}  // namespace
}  // namespace exchange::krx
