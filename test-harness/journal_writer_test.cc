#include "test-harness/journal_writer.h"
#include "test-harness/journal_parser.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Helpers to build test fixtures
// ---------------------------------------------------------------------------

static ParsedConfig default_config() {
    ParsedConfig c;
    c.match_algo      = "FIFO";
    c.tick_size       = 100;
    c.lot_size        = 10000;
    c.max_orders      = 1000;
    c.max_levels      = 100;
    c.max_order_ids   = 10000;
    c.price_band_low  = 0;
    c.price_band_high = 0;
    return c;
}

static ParsedAction make_new_order(int64_t ts, int64_t cl_ord_id,
                                   int64_t account_id, const std::string& side,
                                   int64_t price, int64_t qty,
                                   const std::string& type,
                                   const std::string& tif) {
    ParsedAction a;
    a.type = ParsedAction::NewOrder;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["cl_ord_id"]  = std::to_string(cl_ord_id);
    a.fields["account_id"] = std::to_string(account_id);
    a.fields["side"]       = side;
    a.fields["price"]      = std::to_string(price);
    a.fields["qty"]        = std::to_string(qty);
    a.fields["type"]       = type;
    a.fields["tif"]        = tif;
    return a;
}

static ParsedAction make_cancel(int64_t ts, int64_t ord_id) {
    ParsedAction a;
    a.type = ParsedAction::Cancel;
    a.fields["ts"]     = std::to_string(ts);
    a.fields["ord_id"] = std::to_string(ord_id);
    return a;
}

static ParsedAction make_modify(int64_t ts, int64_t ord_id, int64_t cl_ord_id,
                                int64_t new_price, int64_t new_qty) {
    ParsedAction a;
    a.type = ParsedAction::Modify;
    a.fields["ts"]        = std::to_string(ts);
    a.fields["ord_id"]    = std::to_string(ord_id);
    a.fields["cl_ord_id"] = std::to_string(cl_ord_id);
    a.fields["new_price"] = std::to_string(new_price);
    a.fields["new_qty"]   = std::to_string(new_qty);
    return a;
}

static ParsedAction make_trigger_expiry(int64_t ts, const std::string& tif) {
    ParsedAction a;
    a.type = ParsedAction::TriggerExpiry;
    a.fields["ts"]  = std::to_string(ts);
    a.fields["tif"] = tif;
    return a;
}

// ---------------------------------------------------------------------------
// Config serialization
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, ConfigToConfigLinesDefaultValues) {
    ParsedConfig cfg = default_config();
    std::string line = JournalWriter::config_to_config_lines(cfg);
    // Must start with "CONFIG "
    EXPECT_EQ(line.substr(0, 7), "CONFIG ");
    // Must contain each expected key=value pair
    EXPECT_NE(line.find("match_algo=FIFO"), std::string::npos);
    EXPECT_NE(line.find("tick_size=100"), std::string::npos);
    EXPECT_NE(line.find("lot_size=10000"), std::string::npos);
    EXPECT_NE(line.find("max_orders=1000"), std::string::npos);
    EXPECT_NE(line.find("max_levels=100"), std::string::npos);
    EXPECT_NE(line.find("max_order_ids=10000"), std::string::npos);
    EXPECT_NE(line.find("price_band_low=0"), std::string::npos);
    EXPECT_NE(line.find("price_band_high=0"), std::string::npos);
}

TEST(JournalWriterTest, ConfigToConfigLinesNonDefaultValues) {
    ParsedConfig cfg;
    cfg.match_algo      = "PRO_RATA";
    cfg.tick_size       = 200;
    cfg.lot_size        = 5000;
    cfg.max_orders      = 500;
    cfg.max_levels      = 50;
    cfg.max_order_ids   = 2000;
    cfg.price_band_low  = 1000;
    cfg.price_band_high = 9999;

    std::string line = JournalWriter::config_to_config_lines(cfg);
    EXPECT_NE(line.find("match_algo=PRO_RATA"), std::string::npos);
    EXPECT_NE(line.find("tick_size=200"), std::string::npos);
    EXPECT_NE(line.find("lot_size=5000"), std::string::npos);
    EXPECT_NE(line.find("max_orders=500"), std::string::npos);
    EXPECT_NE(line.find("max_levels=50"), std::string::npos);
    EXPECT_NE(line.find("max_order_ids=2000"), std::string::npos);
    EXPECT_NE(line.find("price_band_low=1000"), std::string::npos);
    EXPECT_NE(line.find("price_band_high=9999"), std::string::npos);
}

TEST(JournalWriterTest, ConfigRoundTrip) {
    ParsedConfig orig;
    orig.match_algo      = "PRO_RATA";
    orig.tick_size       = 50;
    orig.lot_size        = 20000;
    orig.max_orders      = 2000;
    orig.max_levels      = 200;
    orig.max_order_ids   = 5000;
    orig.price_band_low  = 100;
    orig.price_band_high = 99900;

    std::string lines = JournalWriter::config_to_config_lines(orig);
    Journal j = JournalParser::parse_string(lines + "\n");

    EXPECT_EQ(j.config.match_algo,      orig.match_algo);
    EXPECT_EQ(j.config.tick_size,       orig.tick_size);
    EXPECT_EQ(j.config.lot_size,        orig.lot_size);
    EXPECT_EQ(j.config.max_orders,      orig.max_orders);
    EXPECT_EQ(j.config.max_levels,      orig.max_levels);
    EXPECT_EQ(j.config.max_order_ids,   orig.max_order_ids);
    EXPECT_EQ(j.config.price_band_low,  orig.price_band_low);
    EXPECT_EQ(j.config.price_band_high, orig.price_band_high);
}

// ---------------------------------------------------------------------------
// Action serialization
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, ActionToActionLineNewOrder) {
    ParsedAction a = make_new_order(1000, 1, 100, "BUY", 1005000, 10000,
                                    "LIMIT", "GTC");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_EQ(line.substr(0, 11), "ACTION NEW_");
    EXPECT_NE(line.find("NEW_ORDER"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("account_id=100"), std::string::npos);
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    EXPECT_NE(line.find("price=1005000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
    EXPECT_NE(line.find("type=LIMIT"), std::string::npos);
    EXPECT_NE(line.find("tif=GTC"), std::string::npos);
}

TEST(JournalWriterTest, ActionToActionLineCancel) {
    ParsedAction a = make_cancel(3000, 1);
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION CANCEL"), std::string::npos);
    EXPECT_NE(line.find("ts=3000"), std::string::npos);
    EXPECT_NE(line.find("ord_id=1"), std::string::npos);
}

TEST(JournalWriterTest, ActionToActionLineModify) {
    ParsedAction a = make_modify(4000, 2, 5, 1006000, 20000);
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION MODIFY"), std::string::npos);
    EXPECT_NE(line.find("ts=4000"), std::string::npos);
    EXPECT_NE(line.find("ord_id=2"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=5"), std::string::npos);
    EXPECT_NE(line.find("new_price=1006000"), std::string::npos);
    EXPECT_NE(line.find("new_qty=20000"), std::string::npos);
}

TEST(JournalWriterTest, ActionToActionLineTriggerExpiry) {
    ParsedAction a = make_trigger_expiry(5000, "GTD");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION TRIGGER_EXPIRY"), std::string::npos);
    EXPECT_NE(line.find("ts=5000"), std::string::npos);
    EXPECT_NE(line.find("tif=GTD"), std::string::npos);
}

TEST(JournalWriterTest, ActionNewOrderRoundTrip) {
    ParsedAction orig = make_new_order(1000, 7, 42, "SELL", 2000000, 30000,
                                       "LIMIT", "IOC");
    std::string line = JournalWriter::action_to_action_line(orig);
    Journal j = JournalParser::parse_string(line + "\n");

    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::NewOrder);
    EXPECT_EQ(a.get_int("ts"), 1000);
    EXPECT_EQ(a.get_int("cl_ord_id"), 7);
    EXPECT_EQ(a.get_int("account_id"), 42);
    EXPECT_EQ(a.get_str("side"), "SELL");
    EXPECT_EQ(a.get_int("price"), 2000000);
    EXPECT_EQ(a.get_int("qty"), 30000);
    EXPECT_EQ(a.get_str("type"), "LIMIT");
    EXPECT_EQ(a.get_str("tif"), "IOC");
}

TEST(JournalWriterTest, ActionCancelRoundTrip) {
    ParsedAction orig = make_cancel(3000, 99);
    std::string line = JournalWriter::action_to_action_line(orig);
    Journal j = JournalParser::parse_string(line + "\n");

    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::Cancel);
    EXPECT_EQ(a.get_int("ts"), 3000);
    EXPECT_EQ(a.get_int("ord_id"), 99);
}

TEST(JournalWriterTest, ActionModifyRoundTrip) {
    ParsedAction orig = make_modify(4000, 2, 5, 1006000, 20000);
    std::string line = JournalWriter::action_to_action_line(orig);
    Journal j = JournalParser::parse_string(line + "\n");

    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::Modify);
    EXPECT_EQ(a.get_int("ts"), 4000);
    EXPECT_EQ(a.get_int("ord_id"), 2);
    EXPECT_EQ(a.get_int("cl_ord_id"), 5);
    EXPECT_EQ(a.get_int("new_price"), 1006000);
    EXPECT_EQ(a.get_int("new_qty"), 20000);
}

TEST(JournalWriterTest, ActionTriggerExpiryRoundTrip) {
    ParsedAction orig = make_trigger_expiry(5000, "GTD");
    std::string line = JournalWriter::action_to_action_line(orig);
    Journal j = JournalParser::parse_string(line + "\n");

    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& a = j.entries[0].action;
    EXPECT_EQ(a.type, ParsedAction::TriggerExpiry);
    EXPECT_EQ(a.get_int("ts"), 5000);
    EXPECT_EQ(a.get_str("tif"), "GTD");
}

TEST(JournalWriterTest, ActionNewOrderWithOptionalFields) {
    ParsedAction a = make_new_order(1000, 1, 100, "BUY", 1005000, 10000,
                                    "STOP_LIMIT", "GTD");
    a.fields["stop_price"] = "1004000";
    a.fields["gtd_expiry"] = "9999999";

    std::string line = JournalWriter::action_to_action_line(a);
    Journal j = JournalParser::parse_string(line + "\n");

    ASSERT_EQ(j.entries.size(), 1u);
    const ParsedAction& parsed = j.entries[0].action;
    EXPECT_EQ(parsed.get_int("stop_price"), 1004000);
    EXPECT_EQ(parsed.get_int("gtd_expiry"), 9999999);
}

// ---------------------------------------------------------------------------
// Event-to-EXPECT-line serialization
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, EventOrderAccepted) {
    OrderAccepted e{1, 1, 1000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_ACCEPTED"), std::string::npos);
    EXPECT_NE(line.find("ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderRejected) {
    OrderRejected e{7, 2000, RejectReason::InvalidPrice};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_REJECTED"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=7"), std::string::npos);
    EXPECT_NE(line.find("ts=2000"), std::string::npos);
    EXPECT_NE(line.find("reason="), std::string::npos);
}

TEST(JournalWriterTest, EventOrderFilled) {
    OrderFilled e{2, 1, 1005000, 10000, 2000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_FILLED"), std::string::npos);
    EXPECT_NE(line.find("aggressor=2"), std::string::npos);
    EXPECT_NE(line.find("resting=1"), std::string::npos);
    EXPECT_NE(line.find("price=1005000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
    EXPECT_NE(line.find("ts=2000"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderPartiallyFilled) {
    OrderPartiallyFilled e{2, 1, 1005000, 5000, 5000, 5000, 2000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_PARTIALLY_FILLED"), std::string::npos);
    EXPECT_NE(line.find("aggressor=2"), std::string::npos);
    EXPECT_NE(line.find("resting=1"), std::string::npos);
    EXPECT_NE(line.find("price=1005000"), std::string::npos);
    EXPECT_NE(line.find("qty=5000"), std::string::npos);
    EXPECT_NE(line.find("aggressor_rem=5000"), std::string::npos);
    EXPECT_NE(line.find("resting_rem=5000"), std::string::npos);
    EXPECT_NE(line.find("ts=2000"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderCancelled) {
    OrderCancelled e{3, 3000, CancelReason::UserRequested};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_CANCELLED"), std::string::npos);
    EXPECT_NE(line.find("ord_id=3"), std::string::npos);
    EXPECT_NE(line.find("ts=3000"), std::string::npos);
    EXPECT_NE(line.find("reason="), std::string::npos);
}

TEST(JournalWriterTest, EventOrderCancelRejected) {
    OrderCancelRejected e{5, 10, 4000, RejectReason::UnknownOrder};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_CANCEL_REJECTED"), std::string::npos);
    EXPECT_NE(line.find("ord_id=5"), std::string::npos);
    EXPECT_NE(line.find("ts=4000"), std::string::npos);
    EXPECT_NE(line.find("reason=UNKNOWN_ORDER"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderModified) {
    OrderModified e{2, 5, 1006000, 20000, 4000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_MODIFIED"), std::string::npos);
    EXPECT_NE(line.find("ord_id=2"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=5"), std::string::npos);
    EXPECT_NE(line.find("new_price=1006000"), std::string::npos);
    EXPECT_NE(line.find("new_qty=20000"), std::string::npos);
    EXPECT_NE(line.find("ts=4000"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderModifyRejected) {
    OrderModifyRejected e{2, 5, 4000, RejectReason::InvalidPrice};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_MODIFY_REJECTED"), std::string::npos);
    EXPECT_NE(line.find("ord_id=2"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=5"), std::string::npos);
    EXPECT_NE(line.find("ts=4000"), std::string::npos);
    EXPECT_NE(line.find("reason="), std::string::npos);
}

TEST(JournalWriterTest, EventTopOfBook) {
    TopOfBook e{1005000, 10000, 0, 0, 1000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT TOP_OF_BOOK"), std::string::npos);
    EXPECT_NE(line.find("bid=1005000"), std::string::npos);
    EXPECT_NE(line.find("bid_qty=10000"), std::string::npos);
    EXPECT_NE(line.find("ask=0"), std::string::npos);
    EXPECT_NE(line.find("ask_qty=0"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
}

TEST(JournalWriterTest, EventDepthUpdateAdd) {
    DepthUpdate e{Side::Buy, 1005000, 10000, 1, DepthUpdate::Add, 1000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT DEPTH_UPDATE"), std::string::npos);
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    EXPECT_NE(line.find("price=1005000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
    EXPECT_NE(line.find("count=1"), std::string::npos);
    EXPECT_NE(line.find("action=ADD"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
}

TEST(JournalWriterTest, EventDepthUpdateUpdate) {
    DepthUpdate e{Side::Sell, 1006000, 20000, 2, DepthUpdate::Update, 2000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("side=SELL"), std::string::npos);
    EXPECT_NE(line.find("action=UPDATE"), std::string::npos);
}

TEST(JournalWriterTest, EventDepthUpdateRemove) {
    DepthUpdate e{Side::Buy, 1005000, 0, 0, DepthUpdate::Remove, 2000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("action=REMOVE"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderBookActionAdd) {
    OrderBookAction e{1, Side::Buy, 1005000, 10000, OrderBookAction::Add, 1000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT ORDER_BOOK_ACTION"), std::string::npos);
    EXPECT_NE(line.find("ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    EXPECT_NE(line.find("price=1005000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
    EXPECT_NE(line.find("action=ADD"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderBookActionCancel) {
    OrderBookAction e{1, Side::Buy, 1005000, 10000, OrderBookAction::Cancel, 1000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("action=CANCEL"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderBookActionModify) {
    OrderBookAction e{1, Side::Sell, 1006000, 5000, OrderBookAction::Modify, 1000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("action=MODIFY"), std::string::npos);
}

TEST(JournalWriterTest, EventOrderBookActionFill) {
    OrderBookAction e{1, Side::Buy, 1005000, 10000, OrderBookAction::Fill, 2000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("action=FILL"), std::string::npos);
}

TEST(JournalWriterTest, EventTrade) {
    Trade e{1005000, 10000, 2, 1, Side::Sell, 2000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("EXPECT TRADE"), std::string::npos);
    EXPECT_NE(line.find("price=1005000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
    EXPECT_NE(line.find("aggressor=2"), std::string::npos);
    EXPECT_NE(line.find("resting=1"), std::string::npos);
    EXPECT_NE(line.find("aggressor_side=SELL"), std::string::npos);
    EXPECT_NE(line.find("ts=2000"), std::string::npos);
}

TEST(JournalWriterTest, EventTradeBuySide) {
    Trade e{1005000, 10000, 3, 2, Side::Buy, 3000};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("aggressor_side=BUY"), std::string::npos);
}

// ---------------------------------------------------------------------------
// RejectReason string round-trips (all variants)
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, RejectReasonPoolExhausted) {
    OrderRejected e{1, 1000, RejectReason::PoolExhausted};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=POOL_EXHAUSTED"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonInvalidPrice) {
    OrderRejected e{1, 1000, RejectReason::InvalidPrice};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=INVALID_PRICE"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonInvalidQuantity) {
    OrderRejected e{1, 1000, RejectReason::InvalidQuantity};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=INVALID_QUANTITY"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonInvalidTif) {
    OrderRejected e{1, 1000, RejectReason::InvalidTif};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=INVALID_TIF"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonInvalidSide) {
    OrderRejected e{1, 1000, RejectReason::InvalidSide};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=INVALID_SIDE"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonUnknownOrder) {
    OrderCancelRejected e{5, 10, 4000, RejectReason::UnknownOrder};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=UNKNOWN_ORDER"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonPriceBandViolation) {
    OrderRejected e{1, 1000, RejectReason::PriceBandViolation};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=PRICE_BAND_VIOLATION"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonLevelPoolExhausted) {
    OrderRejected e{1, 1000, RejectReason::LevelPoolExhausted};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=LEVEL_POOL_EXHAUSTED"), std::string::npos);
}

TEST(JournalWriterTest, RejectReasonExchangeSpecific) {
    OrderRejected e{1, 1000, RejectReason::ExchangeSpecific};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=EXCHANGE_SPECIFIC"), std::string::npos);
}

// ---------------------------------------------------------------------------
// CancelReason string round-trips (all variants)
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, CancelReasonUserRequested) {
    OrderCancelled e{1, 1000, CancelReason::UserRequested};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=USER_REQUESTED"), std::string::npos);
}

TEST(JournalWriterTest, CancelReasonIOCRemainder) {
    OrderCancelled e{1, 1000, CancelReason::IOCRemainder};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=IOC_REMAINDER"), std::string::npos);
}

TEST(JournalWriterTest, CancelReasonFOKFailed) {
    OrderCancelled e{1, 1000, CancelReason::FOKFailed};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=FOK_FAILED"), std::string::npos);
}

TEST(JournalWriterTest, CancelReasonExpired) {
    OrderCancelled e{1, 1000, CancelReason::Expired};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=EXPIRED"), std::string::npos);
}

TEST(JournalWriterTest, CancelReasonSelfMatchPrevention) {
    OrderCancelled e{1, 1000, CancelReason::SelfMatchPrevention};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=SELF_MATCH_PREVENTION"), std::string::npos);
}

TEST(JournalWriterTest, CancelReasonLevelPoolExhausted) {
    OrderCancelled e{1, 1000, CancelReason::LevelPoolExhausted};
    std::string line = JournalWriter::event_to_expect_line(e);
    EXPECT_NE(line.find("reason=LEVEL_POOL_EXHAUSTED"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Full to_string output (config + entries)
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, ToStringContainsConfigAndActions) {
    ParsedConfig cfg = default_config();

    JournalEntry e1;
    e1.action = make_new_order(1000, 1, 100, "BUY", 1005000, 10000, "LIMIT", "GTC");

    JournalEntry e2;
    e2.action = make_cancel(3000, 1);

    std::vector<JournalEntry> entries = {e1, e2};
    std::string out = JournalWriter::to_string(cfg, entries);

    EXPECT_NE(out.find("CONFIG"), std::string::npos);
    EXPECT_NE(out.find("ACTION NEW_ORDER"), std::string::npos);
    EXPECT_NE(out.find("ACTION CANCEL"), std::string::npos);
}

TEST(JournalWriterTest, ToStringContainsExpectLines) {
    ParsedConfig cfg = default_config();

    JournalEntry e1;
    e1.action = make_new_order(1000, 1, 100, "BUY", 1005000, 10000, "LIMIT", "GTC");
    ParsedExpectation exp;
    exp.event_type = "ORDER_ACCEPTED";
    exp.fields["ord_id"]    = "1";
    exp.fields["cl_ord_id"] = "1";
    exp.fields["ts"]        = "1000";
    e1.expectations.push_back(exp);

    std::vector<JournalEntry> entries = {e1};
    std::string out = JournalWriter::to_string(cfg, entries);
    EXPECT_NE(out.find("EXPECT ORDER_ACCEPTED"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Round-trip: to_string -> parse_string -> compare
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, RoundTripWithParser) {
    ParsedConfig cfg;
    cfg.match_algo      = "FIFO";
    cfg.tick_size       = 100;
    cfg.lot_size        = 10000;
    cfg.max_orders      = 1000;
    cfg.max_levels      = 100;
    cfg.max_order_ids   = 10000;
    cfg.price_band_low  = 0;
    cfg.price_band_high = 0;

    JournalEntry e1;
    e1.action = make_new_order(1000, 1, 100, "BUY", 1005000, 10000, "LIMIT", "GTC");
    {
        ParsedExpectation exp;
        exp.event_type = "ORDER_ACCEPTED";
        exp.fields["ord_id"]    = "1";
        exp.fields["cl_ord_id"] = "1";
        exp.fields["ts"]        = "1000";
        e1.expectations.push_back(exp);
    }
    {
        ParsedExpectation exp;
        exp.event_type = "DEPTH_UPDATE";
        exp.fields["side"]   = "BUY";
        exp.fields["price"]  = "1005000";
        exp.fields["qty"]    = "10000";
        exp.fields["count"]  = "1";
        exp.fields["action"] = "ADD";
        exp.fields["ts"]     = "1000";
        e1.expectations.push_back(exp);
    }

    JournalEntry e2;
    e2.action = make_cancel(3000, 1);
    {
        ParsedExpectation exp;
        exp.event_type = "ORDER_CANCEL_REJECTED";
        exp.fields["ord_id"] = "1";
        exp.fields["ts"]     = "3000";
        exp.fields["reason"] = "UNKNOWN_ORDER";
        e2.expectations.push_back(exp);
    }

    std::vector<JournalEntry> entries = {e1, e2};
    std::string serialized = JournalWriter::to_string(cfg, entries);

    // Parse back
    Journal parsed = JournalParser::parse_string(serialized);

    // Verify config
    EXPECT_EQ(parsed.config.match_algo,      cfg.match_algo);
    EXPECT_EQ(parsed.config.tick_size,       cfg.tick_size);
    EXPECT_EQ(parsed.config.lot_size,        cfg.lot_size);
    EXPECT_EQ(parsed.config.max_orders,      cfg.max_orders);
    EXPECT_EQ(parsed.config.max_levels,      cfg.max_levels);
    EXPECT_EQ(parsed.config.max_order_ids,   cfg.max_order_ids);
    EXPECT_EQ(parsed.config.price_band_low,  cfg.price_band_low);
    EXPECT_EQ(parsed.config.price_band_high, cfg.price_band_high);

    // Verify entries
    ASSERT_EQ(parsed.entries.size(), 2u);

    // Entry 0: NEW_ORDER
    EXPECT_EQ(parsed.entries[0].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(parsed.entries[0].action.get_int("ts"), 1000);
    EXPECT_EQ(parsed.entries[0].action.get_int("cl_ord_id"), 1);
    EXPECT_EQ(parsed.entries[0].action.get_int("account_id"), 100);
    EXPECT_EQ(parsed.entries[0].action.get_str("side"), "BUY");
    EXPECT_EQ(parsed.entries[0].action.get_int("price"), 1005000);
    EXPECT_EQ(parsed.entries[0].action.get_int("qty"), 10000);
    EXPECT_EQ(parsed.entries[0].action.get_str("type"), "LIMIT");
    EXPECT_EQ(parsed.entries[0].action.get_str("tif"), "GTC");

    ASSERT_EQ(parsed.entries[0].expectations.size(), 2u);
    EXPECT_EQ(parsed.entries[0].expectations[0].event_type, "ORDER_ACCEPTED");
    EXPECT_EQ(parsed.entries[0].expectations[0].get_int("ord_id"), 1);
    EXPECT_EQ(parsed.entries[0].expectations[0].get_int("cl_ord_id"), 1);
    EXPECT_EQ(parsed.entries[0].expectations[0].get_int("ts"), 1000);
    EXPECT_EQ(parsed.entries[0].expectations[1].event_type, "DEPTH_UPDATE");
    EXPECT_EQ(parsed.entries[0].expectations[1].get_str("side"), "BUY");
    EXPECT_EQ(parsed.entries[0].expectations[1].get_int("price"), 1005000);
    EXPECT_EQ(parsed.entries[0].expectations[1].get_str("action"), "ADD");

    // Entry 1: CANCEL
    EXPECT_EQ(parsed.entries[1].action.type, ParsedAction::Cancel);
    EXPECT_EQ(parsed.entries[1].action.get_int("ts"), 3000);
    EXPECT_EQ(parsed.entries[1].action.get_int("ord_id"), 1);

    ASSERT_EQ(parsed.entries[1].expectations.size(), 1u);
    EXPECT_EQ(parsed.entries[1].expectations[0].event_type, "ORDER_CANCEL_REJECTED");
    EXPECT_EQ(parsed.entries[1].expectations[0].get_str("reason"), "UNKNOWN_ORDER");
}

// ---------------------------------------------------------------------------
// File write round-trip
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, WriteToFileAndParseBack) {
    const char* tmp_path = "/tmp/journal_writer_test.journal";

    ParsedConfig cfg = default_config();
    cfg.match_algo = "PRO_RATA";
    cfg.tick_size  = 50;

    JournalEntry e1;
    e1.action = make_new_order(100, 7, 42, "SELL", 2000000, 30000, "LIMIT", "IOC");
    {
        ParsedExpectation exp;
        exp.event_type = "ORDER_ACCEPTED";
        exp.fields["ord_id"]    = "1";
        exp.fields["cl_ord_id"] = "7";
        exp.fields["ts"]        = "100";
        e1.expectations.push_back(exp);
    }

    std::vector<JournalEntry> entries = {e1};
    JournalWriter::write(tmp_path, cfg, entries);

    Journal parsed = JournalParser::parse(tmp_path);
    std::remove(tmp_path);

    EXPECT_EQ(parsed.config.match_algo, "PRO_RATA");
    EXPECT_EQ(parsed.config.tick_size, 50);
    ASSERT_EQ(parsed.entries.size(), 1u);
    EXPECT_EQ(parsed.entries[0].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(parsed.entries[0].action.get_int("cl_ord_id"), 7);
    ASSERT_EQ(parsed.entries[0].expectations.size(), 1u);
    EXPECT_EQ(parsed.entries[0].expectations[0].event_type, "ORDER_ACCEPTED");
    EXPECT_EQ(parsed.entries[0].expectations[0].get_int("cl_ord_id"), 7);
}

TEST(JournalWriterTest, WriteToFileThrowsOnBadPath) {
    ParsedConfig cfg = default_config();
    std::vector<JournalEntry> entries;
    EXPECT_THROW(
        JournalWriter::write("/nonexistent_dir/test.journal", cfg, entries),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, EmptyEntriesList) {
    ParsedConfig cfg = default_config();
    std::vector<JournalEntry> entries;
    std::string out = JournalWriter::to_string(cfg, entries);
    // Should still emit a CONFIG line
    EXPECT_NE(out.find("CONFIG"), std::string::npos);
    // Should parse without errors
    Journal j = JournalParser::parse_string(out);
    EXPECT_TRUE(j.entries.empty());
}

TEST(JournalWriterTest, EntryWithNoExpectations) {
    ParsedConfig cfg = default_config();
    JournalEntry e;
    e.action = make_cancel(1000, 5);
    std::vector<JournalEntry> entries = {e};
    std::string out = JournalWriter::to_string(cfg, entries);

    Journal j = JournalParser::parse_string(out);
    ASSERT_EQ(j.entries.size(), 1u);
    EXPECT_TRUE(j.entries[0].expectations.empty());
}

TEST(JournalWriterTest, MultipleEntriesPreserveOrder) {
    ParsedConfig cfg = default_config();

    JournalEntry e1;
    e1.action = make_new_order(1000, 1, 100, "BUY", 1005000, 10000, "LIMIT", "GTC");

    JournalEntry e2;
    e2.action = make_new_order(2000, 2, 200, "SELL", 1005000, 10000, "LIMIT", "GTC");

    JournalEntry e3;
    e3.action = make_cancel(3000, 1);

    std::vector<JournalEntry> entries = {e1, e2, e3};
    std::string out = JournalWriter::to_string(cfg, entries);
    Journal j = JournalParser::parse_string(out);

    ASSERT_EQ(j.entries.size(), 3u);
    EXPECT_EQ(j.entries[0].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(j.entries[0].action.get_int("cl_ord_id"), 1);
    EXPECT_EQ(j.entries[1].action.type, ParsedAction::NewOrder);
    EXPECT_EQ(j.entries[1].action.get_int("cl_ord_id"), 2);
    EXPECT_EQ(j.entries[2].action.type, ParsedAction::Cancel);
    EXPECT_EQ(j.entries[2].action.get_int("ord_id"), 1);
}

// ---------------------------------------------------------------------------
// iLink3 ACTION serialization (E2E extension)
// ---------------------------------------------------------------------------

static ParsedAction make_ilink3_new_order(int64_t ts, const std::string& instrument,
                                           int64_t cl_ord_id, const std::string& account,
                                           const std::string& side, int64_t price,
                                           int64_t qty, const std::string& type,
                                           const std::string& tif) {
    ParsedAction a;
    a.type = ParsedAction::ILink3NewOrder;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["instrument"] = instrument;
    a.fields["cl_ord_id"]  = std::to_string(cl_ord_id);
    a.fields["account"]    = account;
    a.fields["side"]       = side;
    a.fields["price"]      = std::to_string(price);
    a.fields["qty"]        = std::to_string(qty);
    a.fields["type"]       = type;
    a.fields["tif"]        = tif;
    return a;
}

static ParsedAction make_ilink3_cancel(int64_t ts, const std::string& instrument,
                                        int64_t cl_ord_id, int64_t orig_cl_ord_id) {
    ParsedAction a;
    a.type = ParsedAction::ILink3Cancel;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["instrument"]      = instrument;
    a.fields["cl_ord_id"]       = std::to_string(cl_ord_id);
    a.fields["orig_cl_ord_id"]  = std::to_string(orig_cl_ord_id);
    return a;
}

static ParsedAction make_ilink3_replace(int64_t ts, const std::string& instrument,
                                         int64_t cl_ord_id, int64_t orig_cl_ord_id,
                                         int64_t price, int64_t qty) {
    ParsedAction a;
    a.type = ParsedAction::ILink3Replace;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["instrument"]      = instrument;
    a.fields["cl_ord_id"]       = std::to_string(cl_ord_id);
    a.fields["orig_cl_ord_id"]  = std::to_string(orig_cl_ord_id);
    a.fields["price"]           = std::to_string(price);
    a.fields["qty"]             = std::to_string(qty);
    return a;
}

static ParsedAction make_ilink3_mass_cancel(int64_t ts, const std::string& instrument,
                                             const std::string& account) {
    ParsedAction a;
    a.type = ParsedAction::ILink3MassCancel;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["instrument"] = instrument;
    a.fields["account"]    = account;
    return a;
}

TEST(JournalWriterTest, ILink3NewOrderActionLine) {
    ParsedAction a = make_ilink3_new_order(1000, "ES", 1, "FIRM_A",
                                            "BUY", 50000000, 10000, "LIMIT", "DAY");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ILINK3_NEW_ORDER"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
    EXPECT_NE(line.find("instrument=ES"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("account=FIRM_A"), std::string::npos);
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    EXPECT_NE(line.find("price=50000000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
    EXPECT_NE(line.find("type=LIMIT"), std::string::npos);
    EXPECT_NE(line.find("tif=DAY"), std::string::npos);
}

TEST(JournalWriterTest, ILink3CancelActionLine) {
    ParsedAction a = make_ilink3_cancel(2000, "ES", 2, 1);
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ILINK3_CANCEL"), std::string::npos);
    EXPECT_NE(line.find("ts=2000"), std::string::npos);
    EXPECT_NE(line.find("instrument=ES"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=2"), std::string::npos);
    EXPECT_NE(line.find("orig_cl_ord_id=1"), std::string::npos);
}

TEST(JournalWriterTest, ILink3ReplaceActionLine) {
    ParsedAction a = make_ilink3_replace(3000, "ES", 3, 1, 50010000, 10000);
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ILINK3_REPLACE"), std::string::npos);
    EXPECT_NE(line.find("ts=3000"), std::string::npos);
    EXPECT_NE(line.find("instrument=ES"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=3"), std::string::npos);
    EXPECT_NE(line.find("orig_cl_ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("price=50010000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
}

TEST(JournalWriterTest, ILink3MassCancelActionLine) {
    ParsedAction a = make_ilink3_mass_cancel(4000, "ES", "FIRM_A");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ILINK3_MASS_CANCEL"), std::string::npos);
    EXPECT_NE(line.find("ts=4000"), std::string::npos);
    EXPECT_NE(line.find("instrument=ES"), std::string::npos);
    EXPECT_NE(line.find("account=FIRM_A"), std::string::npos);
}

// ---------------------------------------------------------------------------
// iLink3 round-trip: writer → parser
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, ILink3RoundTripThroughParser) {
    ParsedConfig cfg = default_config();

    JournalEntry e1;
    e1.action = make_ilink3_new_order(1000, "ES", 1, "FIRM_A",
                                       "BUY", 50000000, 10000, "LIMIT", "DAY");
    e1.expectations.push_back(ParsedExpectation{"EXEC_NEW", {{"ord_id", "1"}, {"cl_ord_id", "1"}, {"status", "NEW"}, {"instrument", "ES"}}});
    e1.expectations.push_back(ParsedExpectation{"MD_BOOK_ADD", {{"instrument", "ES"}, {"side", "BUY"}, {"price", "50000000"}, {"qty", "10000"}, {"num_orders", "1"}}});

    JournalEntry e2;
    e2.action = make_ilink3_cancel(2000, "ES", 2, 1);
    e2.expectations.push_back(ParsedExpectation{"EXEC_CANCELLED", {{"ord_id", "1"}, {"cl_ord_id", "2"}, {"status", "CANCELLED"}}});
    e2.expectations.push_back(ParsedExpectation{"MD_BOOK_DELETE", {{"instrument", "ES"}, {"side", "BUY"}, {"price", "50000000"}}});

    JournalEntry e3;
    e3.action = make_ilink3_mass_cancel(5000, "ES", "FIRM_A");

    std::vector<JournalEntry> entries = {e1, e2, e3};
    std::string out = JournalWriter::to_string(cfg, entries);
    Journal j = JournalParser::parse_string(out);

    ASSERT_EQ(j.entries.size(), 3u);
    EXPECT_EQ(j.entries[0].action.type, ParsedAction::ILink3NewOrder);
    EXPECT_EQ(j.entries[0].action.get_str("instrument"), "ES");
    EXPECT_EQ(j.entries[0].action.get_int("cl_ord_id"), 1);
    ASSERT_EQ(j.entries[0].expectations.size(), 2u);
    EXPECT_EQ(j.entries[0].expectations[0].event_type, "EXEC_NEW");
    EXPECT_EQ(j.entries[0].expectations[1].event_type, "MD_BOOK_ADD");

    EXPECT_EQ(j.entries[1].action.type, ParsedAction::ILink3Cancel);
    ASSERT_EQ(j.entries[1].expectations.size(), 2u);
    EXPECT_EQ(j.entries[1].expectations[0].event_type, "EXEC_CANCELLED");
    EXPECT_EQ(j.entries[1].expectations[1].event_type, "MD_BOOK_DELETE");

    EXPECT_EQ(j.entries[2].action.type, ParsedAction::ILink3MassCancel);
    EXPECT_EQ(j.entries[2].expectations.size(), 0u);
}

// ---------------------------------------------------------------------------
// ICE FIX ACTION serialization (E2E extension)
// ---------------------------------------------------------------------------

static ParsedAction make_ice_fix_new_order(int64_t ts, const std::string& instrument,
                                            int64_t cl_ord_id, const std::string& account,
                                            const std::string& side, int64_t price,
                                            int64_t qty, const std::string& type,
                                            const std::string& tif) {
    ParsedAction a;
    a.type = ParsedAction::IceFixNewOrder;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["instrument"] = instrument;
    a.fields["cl_ord_id"]  = std::to_string(cl_ord_id);
    a.fields["account"]    = account;
    a.fields["side"]       = side;
    a.fields["price"]      = std::to_string(price);
    a.fields["qty"]        = std::to_string(qty);
    a.fields["type"]       = type;
    a.fields["tif"]        = tif;
    return a;
}

static ParsedAction make_ice_fix_cancel(int64_t ts, const std::string& instrument,
                                         int64_t cl_ord_id, int64_t orig_cl_ord_id,
                                         const std::string& side) {
    ParsedAction a;
    a.type = ParsedAction::IceFixCancel;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["instrument"]      = instrument;
    a.fields["cl_ord_id"]       = std::to_string(cl_ord_id);
    a.fields["orig_cl_ord_id"]  = std::to_string(orig_cl_ord_id);
    a.fields["side"]            = side;
    return a;
}

static ParsedAction make_ice_fix_replace(int64_t ts, const std::string& instrument,
                                          int64_t cl_ord_id, int64_t orig_cl_ord_id,
                                          int64_t price, int64_t qty,
                                          const std::string& side) {
    ParsedAction a;
    a.type = ParsedAction::IceFixReplace;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["instrument"]      = instrument;
    a.fields["cl_ord_id"]       = std::to_string(cl_ord_id);
    a.fields["orig_cl_ord_id"]  = std::to_string(orig_cl_ord_id);
    a.fields["price"]           = std::to_string(price);
    a.fields["qty"]             = std::to_string(qty);
    a.fields["side"]            = side;
    return a;
}

static ParsedAction make_ice_fix_mass_cancel(int64_t ts, const std::string& instrument,
                                              const std::string& account) {
    ParsedAction a;
    a.type = ParsedAction::IceFixMassCancel;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["instrument"] = instrument;
    a.fields["account"]    = account;
    return a;
}

TEST(JournalWriterTest, IceFixNewOrderActionLine) {
    ParsedAction a = make_ice_fix_new_order(1000, "CL", 1, "HEDGE_A",
                                             "BUY", 72500000, 10000, "LIMIT", "DAY");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ICE_FIX_NEW_ORDER"), std::string::npos);
    EXPECT_NE(line.find("ts=1000"), std::string::npos);
    EXPECT_NE(line.find("instrument=CL"), std::string::npos);
    EXPECT_NE(line.find("cl_ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("account=HEDGE_A"), std::string::npos);
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    EXPECT_NE(line.find("price=72500000"), std::string::npos);
    EXPECT_NE(line.find("qty=10000"), std::string::npos);
}

TEST(JournalWriterTest, IceFixCancelActionLine) {
    ParsedAction a = make_ice_fix_cancel(2000, "CL", 2, 1, "BUY");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ICE_FIX_CANCEL"), std::string::npos);
    EXPECT_NE(line.find("orig_cl_ord_id=1"), std::string::npos);
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
}

TEST(JournalWriterTest, IceFixReplaceActionLine) {
    ParsedAction a = make_ice_fix_replace(3000, "CL", 3, 1, 73000000, 20000, "BUY");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ICE_FIX_REPLACE"), std::string::npos);
    EXPECT_NE(line.find("price=73000000"), std::string::npos);
    EXPECT_NE(line.find("qty=20000"), std::string::npos);
}

TEST(JournalWriterTest, IceFixMassCancelActionLine) {
    ParsedAction a = make_ice_fix_mass_cancel(4000, "CL", "HEDGE_A");
    std::string line = JournalWriter::action_to_action_line(a);
    EXPECT_NE(line.find("ACTION ICE_FIX_MASS_CANCEL"), std::string::npos);
    EXPECT_NE(line.find("account=HEDGE_A"), std::string::npos);
}

// ---------------------------------------------------------------------------
// ICE FIX round-trip: writer -> parser
// ---------------------------------------------------------------------------

TEST(JournalWriterTest, IceFixRoundTripThroughParser) {
    ParsedConfig cfg = default_config();

    JournalEntry e1;
    e1.action = make_ice_fix_new_order(1000, "CL", 1, "HEDGE_A",
                                        "BUY", 72500000, 10000, "LIMIT", "DAY");
    e1.expectations.push_back(ParsedExpectation{
        "ICE_EXEC_NEW", {{"ord_id", "1"}, {"cl_ord_id", "1"}, {"instrument", "CL"}}});
    e1.expectations.push_back(ParsedExpectation{
        "ICE_MD_ADD", {{"instrument", "CL"}, {"side", "BUY"},
                       {"price", "72500000"}, {"qty", "10000"}, {"num_orders", "1"}}});

    JournalEntry e2;
    e2.action = make_ice_fix_cancel(2000, "CL", 2, 1, "BUY");
    e2.expectations.push_back(ParsedExpectation{
        "ICE_EXEC_CANCELLED", {{"ord_id", "1"}, {"cl_ord_id", "2"}}});
    e2.expectations.push_back(ParsedExpectation{
        "ICE_MD_REMOVE", {{"instrument", "CL"}, {"side", "BUY"},
                          {"price", "72500000"}}});

    JournalEntry e3;
    e3.action = make_ice_fix_mass_cancel(5000, "CL", "HEDGE_A");

    std::vector<JournalEntry> entries = {e1, e2, e3};
    std::string out = JournalWriter::to_string(cfg, entries);
    Journal j = JournalParser::parse_string(out);

    ASSERT_EQ(j.entries.size(), 3u);
    EXPECT_EQ(j.entries[0].action.type, ParsedAction::IceFixNewOrder);
    EXPECT_EQ(j.entries[0].action.get_str("instrument"), "CL");
    EXPECT_EQ(j.entries[0].action.get_int("cl_ord_id"), 1);
    ASSERT_EQ(j.entries[0].expectations.size(), 2u);
    EXPECT_EQ(j.entries[0].expectations[0].event_type, "ICE_EXEC_NEW");
    EXPECT_EQ(j.entries[0].expectations[1].event_type, "ICE_MD_ADD");

    EXPECT_EQ(j.entries[1].action.type, ParsedAction::IceFixCancel);
    ASSERT_EQ(j.entries[1].expectations.size(), 2u);
    EXPECT_EQ(j.entries[1].expectations[0].event_type, "ICE_EXEC_CANCELLED");
    EXPECT_EQ(j.entries[1].expectations[1].event_type, "ICE_MD_REMOVE");

    EXPECT_EQ(j.entries[2].action.type, ParsedAction::IceFixMassCancel);
    EXPECT_EQ(j.entries[2].expectations.size(), 0u);
}

}  // namespace exchange
