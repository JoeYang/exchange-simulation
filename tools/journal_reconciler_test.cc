#include "tools/journal_reconciler.h"

#include <gtest/gtest.h>
#include <string>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// FlatJournalLine parsing tests.
// ---------------------------------------------------------------------------
TEST(ParseFlatJournal, ParsesActionAndExpectLines) {
    auto lines = parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n");

    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].kind, FlatJournalLine::Action);
    EXPECT_EQ(lines[0].type, "ILINK3_NEW_ORDER");
    EXPECT_EQ(lines[0].get_int("price"), 5000);
    EXPECT_EQ(lines[0].get_str("side"), "BUY");

    EXPECT_EQ(lines[1].kind, FlatJournalLine::Expect);
    EXPECT_EQ(lines[1].type, "ORDER_ACCEPTED");
    EXPECT_EQ(lines[1].get_int("order_id"), 1);

    EXPECT_EQ(lines[2].kind, FlatJournalLine::Expect);
    EXPECT_EQ(lines[2].type, "EXEC_FILL");
    EXPECT_EQ(lines[2].get_int("fill_price"), 5000);
}

TEST(ParseFlatJournal, SkipsConfigAndComments) {
    auto lines = parse_flat_journal(
        "CONFIG match_algo=FIFO tick_size=100\n"
        "# This is a comment\n"
        "\n"
        "ACTION NEW_ORDER ts=100 side=BUY price=5000 qty=10\n");

    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].type, "NEW_ORDER");
}

TEST(ParseFlatJournal, ParsesObserverJournal) {
    auto lines = parse_flat_journal(
        "EXPECT MD_TRADE ts=200 instrument=ES price=5000 qty=10 aggressor_side=BUY\n"
        "EXPECT MD_BOOK_ADD ts=210 instrument=ES side=BUY price=4900 qty=20 order_count=1\n"
        "EXPECT MD_BOOK_DELETE ts=220 instrument=ES side=BUY price=4900\n");

    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].type, "MD_TRADE");
    EXPECT_EQ(lines[1].type, "MD_BOOK_ADD");
    EXPECT_EQ(lines[2].type, "MD_BOOK_DELETE");
}

TEST(ParseFlatJournal, HandlesEmptyInput) {
    auto lines = parse_flat_journal("");
    EXPECT_TRUE(lines.empty());
}

TEST(ParseFlatJournal, MissingFieldReturnsDefault) {
    auto lines = parse_flat_journal(
        "EXPECT MD_TRADE ts=100 price=5000\n");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].get_int("nonexistent"), 0);
    EXPECT_EQ(lines[0].get_str("nonexistent"), "");
    EXPECT_FALSE(lines[0].has("nonexistent"));
    EXPECT_TRUE(lines[0].has("price"));
}

// ---------------------------------------------------------------------------
// Invariant 1: Trade matching.
// ---------------------------------------------------------------------------
class ReconcilerTest : public ::testing::Test {
protected:
    JournalReconciler reconciler;
};

TEST_F(ReconcilerTest, Inv1_AllTradesMatched) {
    // Two traders, each gets one fill from a trade.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=2 side=SELL price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=2 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 instrument=ES price=5000 qty=10 aggressor_side=BUY\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_trades, 1);
    EXPECT_EQ(report.matched_trades, 1);
    EXPECT_EQ(report.unmatched_trades, 0);
}

TEST_F(ReconcilerTest, Inv1_UnmatchedTrade) {
    // Observer sees a trade but no trader fills at that price/qty.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 instrument=ES price=5000 qty=10 aggressor_side=BUY\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_trades, 1);
    EXPECT_EQ(report.matched_trades, 0);
    EXPECT_EQ(report.unmatched_trades, 1);
}

TEST_F(ReconcilerTest, Inv1_MultipleTradesMatched) {
    // Two trades at different prices, each with 2 fills.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"
        "ACTION ILINK3_NEW_ORDER ts=200 client_id=1 side=BUY price=6000 qty=20\n"
        "EXPECT EXEC_FILL ts=215 order_id=3 fill_price=6000 fill_qty=20 leaves_qty=0\n"));

    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=2 side=SELL price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"
        "ACTION ILINK3_NEW_ORDER ts=200 client_id=2 side=SELL price=6000 qty=20\n"
        "EXPECT EXEC_FILL ts=215 order_id=4 fill_price=6000 fill_qty=20 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10 aggressor_side=BUY\n"
        "EXPECT MD_TRADE ts=220 price=6000 qty=20 aggressor_side=BUY\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_trades, 2);
    EXPECT_EQ(report.matched_trades, 2);
    EXPECT_EQ(report.unmatched_trades, 0);
}

// ---------------------------------------------------------------------------
// Invariant 2: Book add traceability.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv2_BookAddTraced) {
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10 order_count=1\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_book_adds, 1);
    EXPECT_EQ(report.book_adds_traced, 1);
}

TEST_F(ReconcilerTest, Inv2_BookAddNotTraced) {
    // Observer sees a book add but no trader accepted an order at that price.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=4000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10 order_count=1\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_book_adds, 1);
    EXPECT_EQ(report.book_adds_traced, 0);
}

TEST_F(ReconcilerTest, Inv2_BookAddWrongSide) {
    // Price matches but side doesn't.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=SELL price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10 order_count=1\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.book_adds_traced, 0);
}

// ---------------------------------------------------------------------------
// Invariant 3: Book delete traceability.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv3_DeleteTracedToCancel) {
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "ACTION ILINK3_CANCEL ts=200 client_id=1 cl_ord_id=1\n"
        "EXPECT ORDER_CANCELLED ts=212 order_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10\n"
        "EXPECT MD_BOOK_DELETE ts=215 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_book_deletes, 1);
    EXPECT_EQ(report.book_deletes_traced_to_cancel, 1);
    EXPECT_EQ(report.book_deletes_traced_to_fill, 0);
}

TEST_F(ReconcilerTest, Inv3_DeleteTracedToFill) {
    // Order fully filled (leaves_qty=0) -> book delete.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=2 side=SELL price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10\n"
        "EXPECT MD_BOOK_DELETE ts=121 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_book_deletes, 1);
    // No cancels, so delete attributed to fill.
    EXPECT_EQ(report.book_deletes_traced_to_cancel, 0);
    EXPECT_EQ(report.book_deletes_traced_to_fill, 1);
}

// ---------------------------------------------------------------------------
// Invariant 4: Event ordering.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv4_OrderedObserver) {
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=100 side=BUY price=5000 qty=10\n"
        "EXPECT MD_TRADE ts=200 price=5000 qty=10\n"
        "EXPECT MD_BOOK_DELETE ts=300 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.ordering_violations, 0);
}

TEST_F(ReconcilerTest, Inv4_OutOfOrderObserver) {
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=300 side=BUY price=5000 qty=10\n"
        "EXPECT MD_TRADE ts=100 price=5000 qty=10\n"
        "EXPECT MD_BOOK_DELETE ts=200 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    // Sequence 300, 100, 200: only 100 < 300 is a consecutive violation.
    EXPECT_EQ(report.ordering_violations, 1);
}

// ---------------------------------------------------------------------------
// Invariant 5: No phantom events.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv5_NoPhantoms) {
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=2 side=SELL price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.phantom_trades, 0);
    EXPECT_EQ(report.phantom_book_adds, 0);
}

TEST_F(ReconcilerTest, Inv5_PhantomTrade) {
    // Observer sees a trade, traders have no fills.
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.phantom_trades, 1);
}

TEST_F(ReconcilerTest, Inv5_PhantomBookAdd) {
    // Observer sees a book add, no trader had an accepted order.
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=120 side=BUY price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.phantom_book_adds, 1);
}

// ---------------------------------------------------------------------------
// Invariant 6: No lost events.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv6_NoLostEvents) {
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "ACTION ILINK3_CANCEL ts=200 client_id=1 cl_ord_id=1\n"
        "EXPECT ORDER_CANCELLED ts=212 order_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10\n"
        "EXPECT MD_BOOK_DELETE ts=215 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.accepted_without_book_add_or_trade, 0);
    EXPECT_EQ(report.fill_without_trade, 0);
    EXPECT_EQ(report.cancelled_without_book_delete, 0);
}

TEST_F(ReconcilerTest, Inv6_LostBookAdd) {
    // Trader has an accepted order but observer never saw the book add.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"));

    reconciler.set_observer_journal(parse_flat_journal(""));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.accepted_without_book_add_or_trade, 1);
}

TEST_F(ReconcilerTest, Inv6_AcceptedButImmediatelyFilled) {
    // Accepted + immediately filled = expects MD_TRADE, not MD_BOOK_ADD.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=2 side=SELL price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    // 2 accepts, 1 immediately filled (order_id=1). Expected book adds = 2-1=1.
    // But we have 0 book adds, and the immediately filled order is accounted for
    // by the trade. However, order_id=2 was never explicitly accepted in the
    // journal, so only 1 accepted count. Expected book adds = 1 - 1 = 0.
    EXPECT_EQ(report.accepted_without_book_add_or_trade, 0);
}

TEST_F(ReconcilerTest, Inv6_LostTrade) {
    // Traders have fills but observer has no trade.
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(""));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.fill_without_trade, 1);  // 2 fills / 2 = 1 expected trade
}

// ---------------------------------------------------------------------------
// Invariant 7: Final book state.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv7_CleanShutdown) {
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=100 side=BUY price=5000 qty=10\n"
        "EXPECT MD_BOOK_ADD ts=110 side=SELL price=6000 qty=20\n"
        "EXPECT MD_BOOK_DELETE ts=200 side=BUY price=5000\n"
        "EXPECT MD_BOOK_DELETE ts=210 side=SELL price=6000\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.final_book_levels, 0);
}

TEST_F(ReconcilerTest, Inv7_NonCleanShutdown) {
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=100 side=BUY price=5000 qty=10\n"
        "EXPECT MD_BOOK_ADD ts=110 side=SELL price=6000 qty=20\n"
        "EXPECT MD_BOOK_DELETE ts=200 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.final_book_levels, 1);  // SELL@6000 still open
}

// ---------------------------------------------------------------------------
// Invariant 8: Fill consistency.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, Inv8_ConsistentFills) {
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.price_mismatches, 0);
    EXPECT_EQ(report.qty_mismatches, 0);
}

TEST_F(ReconcilerTest, Inv8_PriceMismatch) {
    // Trader fills at 5000 but observer trade at 5100.
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5100 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.price_mismatches, 1);
}

TEST_F(ReconcilerTest, Inv8_QtyMismatch) {
    // Trader fills qty=10 but observer trade qty=20.
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "EXPECT EXEC_FILL ts=115 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=20\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.qty_mismatches, 1);
}

// ---------------------------------------------------------------------------
// all_pass() integration test.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, AllPass_FullScenario) {
    // Complete scenario: 2 traders, 1 trade, 1 resting order, clean shutdown.
    // Trader 1: resting buy order, gets filled.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=150 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    // Trader 2: aggressor sell, immediately filled.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=140 client_id=2 side=SELL price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=150 order_id=2 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10 order_count=1\n"
        "EXPECT MD_TRADE ts=155 price=5000 qty=10 aggressor_side=SELL\n"
        "EXPECT MD_BOOK_DELETE ts=156 side=BUY price=5000\n"));

    auto report = reconciler.reconcile();
    EXPECT_TRUE(report.all_pass());
}

TEST_F(ReconcilerTest, AllPass_FalseWhenViolations) {
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_TRADE ts=120 price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_FALSE(report.all_pass());
}

// ---------------------------------------------------------------------------
// Empty journals.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, EmptyJournals) {
    reconciler.add_trader_journal(parse_flat_journal(""));
    reconciler.set_observer_journal(parse_flat_journal(""));

    auto report = reconciler.reconcile();
    EXPECT_TRUE(report.all_pass());
    EXPECT_EQ(report.observer_trades, 0);
    EXPECT_EQ(report.final_book_levels, 0);
}

// ---------------------------------------------------------------------------
// Multiple traders with interleaved events.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, MultipleTraders_Interleaved) {
    // 3 traders place resting orders, 2 get filled.
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"
        "EXPECT EXEC_FILL ts=200 order_id=1 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=110 client_id=2 side=BUY price=4900 qty=20\n"
        "EXPECT ORDER_ACCEPTED ts=122 order_id=3 cl_ord_id=1\n"
        "ACTION ILINK3_CANCEL ts=300 client_id=2 cl_ord_id=1\n"
        "EXPECT ORDER_CANCELLED ts=312 order_id=3\n"));

    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=190 client_id=3 side=SELL price=5000 qty=10\n"
        "EXPECT EXEC_FILL ts=200 order_id=4 fill_price=5000 fill_qty=10 leaves_qty=0\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10 order_count=1\n"
        "EXPECT MD_BOOK_ADD ts=125 side=BUY price=4900 qty=20 order_count=1\n"
        "EXPECT MD_TRADE ts=205 price=5000 qty=10 aggressor_side=SELL\n"
        "EXPECT MD_BOOK_DELETE ts=206 side=BUY price=5000\n"
        "EXPECT MD_BOOK_DELETE ts=315 side=BUY price=4900\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.observer_trades, 1);
    EXPECT_EQ(report.matched_trades, 1);
    EXPECT_EQ(report.observer_book_adds, 2);
    EXPECT_EQ(report.book_adds_traced, 2);
    EXPECT_EQ(report.observer_book_deletes, 2);
    EXPECT_EQ(report.ordering_violations, 0);
    EXPECT_EQ(report.final_book_levels, 0);
    EXPECT_TRUE(report.all_pass());
}

// ---------------------------------------------------------------------------
// Report metadata.
// ---------------------------------------------------------------------------
TEST_F(ReconcilerTest, ReportMetadata) {
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=1 side=BUY price=5000 qty=10\n"
        "EXPECT ORDER_ACCEPTED ts=112 order_id=1 cl_ord_id=1\n"));
    reconciler.add_trader_journal(parse_flat_journal(
        "ACTION ILINK3_NEW_ORDER ts=100 client_id=2 side=SELL price=5000 qty=10\n"));

    reconciler.set_observer_journal(parse_flat_journal(
        "EXPECT MD_BOOK_ADD ts=115 side=BUY price=5000 qty=10\n"
        "EXPECT MD_BOOK_ADD ts=116 side=SELL price=5000 qty=10\n"));

    auto report = reconciler.reconcile();
    EXPECT_EQ(report.total_client_journals, 2);
    EXPECT_EQ(report.total_observer_events, 2);
}

}  // namespace
}  // namespace exchange
