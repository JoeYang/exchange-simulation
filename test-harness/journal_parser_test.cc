#include "test-harness/journal_parser.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace exchange {

// ---------------------------------------------------------------------------
// CONFIG parsing
// ---------------------------------------------------------------------------

TEST(JournalParserTest, DefaultConfigValues) {
    const std::string content = "";
    Journal j = JournalParser::parse_string(content);
    EXPECT_EQ(j.config.match_algo, "FIFO");
    EXPECT_EQ(j.config.tick_size, 100);
    EXPECT_EQ(j.config.lot_size, 10000);
    EXPECT_EQ(j.config.max_orders, 1000);
    EXPECT_EQ(j.config.max_levels, 100);
    EXPECT_EQ(j.config.max_order_ids, 10000);
    EXPECT_EQ(j.config.price_band_low, 0);
    EXPECT_EQ(j.config.price_band_high, 0);
}

TEST(JournalParserTest, ParseConfigMatchAlgo) {
    const std::string content = "CONFIG match_algo=PRO_RATA\n";
    Journal j = JournalParser::parse_string(content);
    EXPECT_EQ(j.config.match_algo, "PRO_RATA");
}

TEST(JournalParserTest, ParseConfigMultipleFields) {
    const std::string content =
        "CONFIG match_algo=FIFO tick_size=200 lot_size=5000 max_orders=500 "
        "max_levels=50 max_order_ids=2000 price_band_low=1000 price_band_high=9999\n";
    Journal j = JournalParser::parse_string(content);
    EXPECT_EQ(j.config.match_algo, "FIFO");
    EXPECT_EQ(j.config.tick_size, 200);
    EXPECT_EQ(j.config.lot_size, 5000);
    EXPECT_EQ(j.config.max_orders, 500);
    EXPECT_EQ(j.config.max_levels, 50);
    EXPECT_EQ(j.config.max_order_ids, 2000);
    EXPECT_EQ(j.config.price_band_low, 1000);
    EXPECT_EQ(j.config.price_band_high, 9999);
}

TEST(JournalParserTest, ParseConfigAcrossMultipleLines) {
    // Two CONFIG lines -- later values override earlier ones for overlapping keys.
    const std::string content =
        "CONFIG match_algo=FIFO tick_size=100\n"
        "CONFIG lot_size=20000\n";
    Journal j = JournalParser::parse_string(content);
    EXPECT_EQ(j.config.match_algo, "FIFO");
    EXPECT_EQ(j.config.tick_size, 100);
    EXPECT_EQ(j.config.lot_size, 20000);
}

// ---------------------------------------------------------------------------
// Comment and blank line handling
// ---------------------------------------------------------------------------

TEST(JournalParserTest, CommentsAndBlankLinesAreIgnored) {
    const std::string content =
        "# This is a comment\n"
        "\n"
        "   \n"
        "# Another comment\n"
        "CONFIG match_algo=FIFO\n"
        "\n"
        "# trailing comment\n";
    Journal j = JournalParser::parse_string(content);
    EXPECT_EQ(j.config.match_algo, "FIFO");
    EXPECT_TRUE(j.entries.empty());
}

// ---------------------------------------------------------------------------
// ACTION parsing
// ---------------------------------------------------------------------------

TEST(JournalParserTest, ParseNewOrderAction) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::NewOrder);
    EXPECT_EQ(a.get_int("ts"), 1000);
    EXPECT_EQ(a.get_int("cl_ord_id"), 1);
    EXPECT_EQ(a.get_int("account_id"), 100);
    EXPECT_EQ(a.get_str("side"), "BUY");
    EXPECT_EQ(a.get_int("price"), 1005000);
    EXPECT_EQ(a.get_int("qty"), 10000);
    EXPECT_EQ(a.get_str("type"), "LIMIT");
    EXPECT_EQ(a.get_str("tif"), "GTC");
}

TEST(JournalParserTest, ParseCancelAction) {
    const std::string content = "ACTION CANCEL ts=3000 ord_id=1\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::Cancel);
    EXPECT_EQ(a.get_int("ts"), 3000);
    EXPECT_EQ(a.get_int("ord_id"), 1);
}

TEST(JournalParserTest, ParseModifyAction) {
    const std::string content =
        "ACTION MODIFY ts=4000 ord_id=2 cl_ord_id=5 new_price=1006000 new_qty=20000\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::Modify);
    EXPECT_EQ(a.get_int("ts"), 4000);
    EXPECT_EQ(a.get_int("ord_id"), 2);
    EXPECT_EQ(a.get_int("cl_ord_id"), 5);
    EXPECT_EQ(a.get_int("new_price"), 1006000);
    EXPECT_EQ(a.get_int("new_qty"), 20000);
}

TEST(JournalParserTest, ParseTriggerExpiryAction) {
    const std::string content = "ACTION TRIGGER_EXPIRY ts=5000 tif=GTD\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::TriggerExpiry);
    EXPECT_EQ(a.get_int("ts"), 5000);
    EXPECT_EQ(a.get_str("tif"), "GTD");
}

// ---------------------------------------------------------------------------
// EXPECT parsing
// ---------------------------------------------------------------------------

TEST(JournalParserTest, ParseSingleExpect) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1000\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    ASSERT_EQ(j.entries[0].expectations.size(), 1u);
    const ParsedExpectation& e = j.entries[0].expectations[0];
    EXPECT_EQ(e.event_type, "ORDER_ACCEPTED");
    EXPECT_EQ(e.get_int("ord_id"), 1);
    EXPECT_EQ(e.get_int("cl_ord_id"), 1);
    EXPECT_EQ(e.get_int("ts"), 1000);
}

TEST(JournalParserTest, ParseMultipleExpectsForOneAction) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1000\n"
        "EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=10000 count=1 action=ADD ts=1000\n"
        "EXPECT TOP_OF_BOOK bid=1005000 bid_qty=10000 ask=0 ask_qty=0 ts=1000\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    ASSERT_EQ(j.entries[0].expectations.size(), 3u);
    EXPECT_EQ(j.entries[0].expectations[0].event_type, "ORDER_ACCEPTED");
    EXPECT_EQ(j.entries[0].expectations[1].event_type, "DEPTH_UPDATE");
    EXPECT_EQ(j.entries[0].expectations[2].event_type, "TOP_OF_BOOK");
}

// ---------------------------------------------------------------------------
// Grouping: multiple actions with their own expects
// ---------------------------------------------------------------------------

TEST(JournalParserTest, ParseMultipleActionsWithExpects) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1000\n"
        "EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=10000 count=1 action=ADD ts=1000\n"
        "\n"
        "ACTION NEW_ORDER ts=2000 cl_ord_id=2 account_id=200 side=SELL "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=2000\n"
        "EXPECT ORDER_FILLED aggressor=2 resting=1 price=1005000 qty=10000 ts=2000\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 2u);

    // First entry
    EXPECT_EQ(j.entries[0].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(j.entries[0].action.get_int("cl_ord_id"), 1);
    ASSERT_EQ(j.entries[0].expectations.size(), 2u);
    EXPECT_EQ(j.entries[0].expectations[0].event_type, "ORDER_ACCEPTED");
    EXPECT_EQ(j.entries[0].expectations[1].event_type, "DEPTH_UPDATE");

    // Second entry
    EXPECT_EQ(j.entries[1].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(j.entries[1].action.get_int("cl_ord_id"), 2);
    ASSERT_EQ(j.entries[1].expectations.size(), 2u);
    EXPECT_EQ(j.entries[1].expectations[0].event_type, "ORDER_ACCEPTED");
    EXPECT_EQ(j.entries[1].expectations[1].event_type, "ORDER_FILLED");
}

TEST(JournalParserTest, ActionWithNoExpects) {
    const std::string content =
        "ACTION CANCEL ts=3000 ord_id=99\n"
        "\n"
        "ACTION CANCEL ts=4000 ord_id=88\n"
        "EXPECT ORDER_CANCEL_REJECTED ord_id=88 ts=4000 reason=UNKNOWN_ORDER\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 2u);
    EXPECT_EQ(j.entries[0].expectations.size(), 0u);
    ASSERT_EQ(j.entries[1].expectations.size(), 1u);
    EXPECT_EQ(j.entries[1].expectations[0].event_type, "ORDER_CANCEL_REJECTED");
}

// ---------------------------------------------------------------------------
// Full journal round-trip (config + multiple actions)
// ---------------------------------------------------------------------------

TEST(JournalParserTest, FullJournalWithConfigAndActions) {
    const std::string content =
        "# Basic limit order fill\n"
        "CONFIG match_algo=FIFO tick_size=100 lot_size=10000\n"
        "\n"
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1000\n"
        "EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=10000 count=1 action=ADD ts=1000\n"
        "EXPECT TOP_OF_BOOK bid=1005000 bid_qty=10000 ask=0 ask_qty=0 ts=1000\n"
        "\n"
        "ACTION NEW_ORDER ts=2000 cl_ord_id=2 account_id=200 side=SELL "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=2000\n"
        "EXPECT ORDER_FILLED aggressor=2 resting=1 price=1005000 qty=10000 ts=2000\n"
        "EXPECT TRADE price=1005000 qty=10000 aggressor=2 resting=1 aggressor_side=SELL ts=2000\n"
        "EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=0 count=0 action=REMOVE ts=2000\n"
        "EXPECT TOP_OF_BOOK bid=0 bid_qty=0 ask=0 ask_qty=0 ts=2000\n"
        "\n"
        "ACTION CANCEL ts=3000 ord_id=1\n"
        "EXPECT ORDER_CANCEL_REJECTED ord_id=1 ts=3000 reason=UNKNOWN_ORDER\n";

    Journal j = JournalParser::parse_string(content);

    EXPECT_EQ(j.config.match_algo, "FIFO");
    EXPECT_EQ(j.config.tick_size, 100);
    EXPECT_EQ(j.config.lot_size, 10000);

    ASSERT_EQ(j.entries.size(), 3u);

    EXPECT_EQ(j.entries[0].action.type, ParsedAction::NewOrder);
    ASSERT_EQ(j.entries[0].expectations.size(), 3u);

    EXPECT_EQ(j.entries[1].action.type, ParsedAction::NewOrder);
    ASSERT_EQ(j.entries[1].expectations.size(), 5u);

    EXPECT_EQ(j.entries[2].action.type, ParsedAction::Cancel);
    ASSERT_EQ(j.entries[2].expectations.size(), 1u);
    EXPECT_EQ(j.entries[2].expectations[0].get_str("reason"), "UNKNOWN_ORDER");
}

// ---------------------------------------------------------------------------
// Optional fields
// ---------------------------------------------------------------------------

TEST(JournalParserTest, NewOrderWithStopPriceAndGtdExpiry) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=STOP_LIMIT tif=GTD stop_price=1004000 gtd_expiry=9999999\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.get_int("stop_price"), 1004000);
    EXPECT_EQ(a.get_int("gtd_expiry"), 9999999);
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(JournalParserTest, UnknownActionTypeThrows) {
    const std::string content = "ACTION FOOBAR ts=1000\n";
    EXPECT_THROW(JournalParser::parse_string(content), std::runtime_error);
}

TEST(JournalParserTest, MalformedKeyValueThrows) {
    // Field without '=' separator
    const std::string content = "ACTION NEW_ORDER ts1000\n";
    EXPECT_THROW(JournalParser::parse_string(content), std::runtime_error);
}

TEST(JournalParserTest, MissingActionTypeThrows) {
    // "ACTION" keyword with no type token
    const std::string content = "ACTION\n";
    EXPECT_THROW(JournalParser::parse_string(content), std::runtime_error);
}

TEST(JournalParserTest, MissingExpectTypeThrows) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT\n";
    EXPECT_THROW(JournalParser::parse_string(content), std::runtime_error);
}

TEST(JournalParserTest, ExpectBeforeActionThrows) {
    const std::string content = "EXPECT ORDER_ACCEPTED ord_id=1 ts=1000\n";
    EXPECT_THROW(JournalParser::parse_string(content), std::runtime_error);
}

TEST(JournalParserTest, GetIntOnMissingKeyThrows) {
    const std::string content = "ACTION CANCEL ts=1000 ord_id=42\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    EXPECT_THROW(j.entries[0].action.get_int("nonexistent"), std::out_of_range);
}

TEST(JournalParserTest, GetStrOnMissingKeyThrows) {
    const std::string content = "ACTION CANCEL ts=1000 ord_id=42\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    EXPECT_THROW(j.entries[0].action.get_str("nonexistent"), std::out_of_range);
}

TEST(JournalParserTest, GetIntOnNonNumericValueThrows) {
    const std::string content = "ACTION CANCEL ts=abc ord_id=1\n";
    // Parsing should succeed (fields stored as strings), but get_int should throw.
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    EXPECT_THROW(j.entries[0].action.get_int("ts"), std::invalid_argument);
}

TEST(JournalParserTest, UnknownTopLevelKeywordThrows) {
    const std::string content = "FOOBAR something=1\n";
    EXPECT_THROW(JournalParser::parse_string(content), std::runtime_error);
}

// ---------------------------------------------------------------------------
// File-based parsing
// ---------------------------------------------------------------------------

TEST(JournalParserTest, ParseFromFile) {
    // Write a temp file and verify round-trip.
    const char* tmp_path = "/tmp/journal_parser_test.journal";
    {
        std::ofstream f(tmp_path);
        ASSERT_TRUE(f.is_open());
        f << "# temp file test\n"
          << "CONFIG match_algo=PRO_RATA tick_size=50\n"
          << "\n"
          << "ACTION NEW_ORDER ts=100 cl_ord_id=7 account_id=42 side=SELL "
          << "price=2000000 qty=30000 type=LIMIT tif=IOC\n"
          << "EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=7 ts=100\n";
    }

    Journal j = JournalParser::parse(tmp_path);
    std::remove(tmp_path);

    EXPECT_EQ(j.config.match_algo, "PRO_RATA");
    EXPECT_EQ(j.config.tick_size, 50);
    ASSERT_EQ(j.entries.size(), 1u);
    EXPECT_EQ(j.entries[0].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(j.entries[0].action.get_int("cl_ord_id"), 7);
    ASSERT_EQ(j.entries[0].expectations.size(), 1u);
    EXPECT_EQ(j.entries[0].expectations[0].event_type, "ORDER_ACCEPTED");
}

TEST(JournalParserTest, ParseNonexistentFileThrows) {
    EXPECT_THROW(JournalParser::parse("/tmp/this_file_does_not_exist_12345.journal"),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// ParsedExpectation field accessors
// ---------------------------------------------------------------------------

TEST(JournalParserTest, ExpectationFieldAccessors) {
    const std::string content =
        "ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY "
        "price=1005000 qty=10000 type=LIMIT tif=GTC\n"
        "EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=10000 count=1 action=ADD ts=1000\n";
    Journal j = JournalParser::parse_string(content);
    ASSERT_EQ(j.entries.size(), 1u);
    ASSERT_EQ(j.entries[0].expectations.size(), 1u);
    const ParsedExpectation& e = j.entries[0].expectations[0];
    EXPECT_EQ(e.get_str("side"), "BUY");
    EXPECT_EQ(e.get_int("price"), 1005000);
    EXPECT_EQ(e.get_int("qty"), 10000);
    EXPECT_EQ(e.get_int("count"), 1);
    EXPECT_EQ(e.get_str("action"), "ADD");
    EXPECT_EQ(e.get_int("ts"), 1000);
}

}  // namespace exchange
