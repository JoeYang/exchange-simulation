#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_encoder.h"
#include "ice/impact/impact_messages.h"

#include <gtest/gtest.h>

#include <cstring>

namespace exchange::ice::impact {
namespace {

// Recording visitor for round-trip verification.
struct Visitor {
    BundleStart    bs{};
    BundleEnd      be{};
    AddModifyOrder amo{};
    OrderWithdrawal ow{};
    DealTrade      dt{};
    MarketStatus   ms{};
    SnapshotOrder  so{};
    PriceLevel     pl{};
    InstrumentDefinition idef{};

    int bs_count{0}, be_count{0}, amo_count{0}, ow_count{0};
    int dt_count{0}, ms_count{0}, so_count{0}, pl_count{0};
    int idef_count{0};

    void on_bundle_start(const BundleStart& m)    { bs = m; ++bs_count; }
    void on_bundle_end(const BundleEnd& m)        { be = m; ++be_count; }
    void on_add_modify_order(const AddModifyOrder& m) { amo = m; ++amo_count; }
    void on_order_withdrawal(const OrderWithdrawal& m) { ow = m; ++ow_count; }
    void on_deal_trade(const DealTrade& m)        { dt = m; ++dt_count; }
    void on_market_status(const MarketStatus& m)  { ms = m; ++ms_count; }
    void on_snapshot_order(const SnapshotOrder& m) { so = m; ++so_count; }
    void on_price_level(const PriceLevel& m)      { pl = m; ++pl_count; }
    void on_instrument_def(const InstrumentDefinition& m) { idef = m; ++idef_count; }
};

// ---------------------------------------------------------------------------
// Raw struct round-trip: encode -> decode for each message type.
// ---------------------------------------------------------------------------

TEST(ImpactCodecTest, RoundTripBundleStart) {
    BundleStart orig{};
    orig.sequence_number = 42;
    orig.message_count = 3;
    orig.timestamp = 1700000000000000000LL;

    char buf[64];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    BundleStart decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.sequence_number, 42u);
    EXPECT_EQ(decoded.message_count, 3u);
    EXPECT_EQ(decoded.timestamp, 1700000000000000000LL);
}

TEST(ImpactCodecTest, RoundTripBundleEnd) {
    BundleEnd orig{};
    orig.sequence_number = 99;

    char buf[16];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    BundleEnd decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.sequence_number, 99u);
}

TEST(ImpactCodecTest, RoundTripAddModifyOrder) {
    AddModifyOrder orig{};
    orig.instrument_id = 100;
    orig.order_id = 5555;
    orig.sequence_within_msg = 7;
    orig.side = static_cast<uint8_t>(Side::Sell);
    orig.price = 45670000;
    orig.quantity = 25;
    orig.is_implied = static_cast<uint8_t>(YesNo::Yes);
    orig.is_rfq = static_cast<uint8_t>(YesNo::No);
    orig.order_entry_date_time = 1700000000000000000LL;

    char buf[64];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    AddModifyOrder decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.instrument_id, 100);
    EXPECT_EQ(decoded.order_id, 5555);
    EXPECT_EQ(decoded.sequence_within_msg, 7u);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(decoded.price, 45670000);
    EXPECT_EQ(decoded.quantity, 25u);
    EXPECT_EQ(decoded.is_implied, static_cast<uint8_t>(YesNo::Yes));
    EXPECT_EQ(decoded.is_rfq, static_cast<uint8_t>(YesNo::No));
    EXPECT_EQ(decoded.order_entry_date_time, 1700000000000000000LL);
}

TEST(ImpactCodecTest, RoundTripOrderWithdrawal) {
    OrderWithdrawal orig{};
    orig.instrument_id = 7;
    orig.order_id = 999;
    orig.sequence_within_msg = 3;
    orig.side = static_cast<uint8_t>(Side::Buy);
    orig.price = 12340000;
    orig.quantity = 50;

    char buf[64];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    OrderWithdrawal decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.instrument_id, 7);
    EXPECT_EQ(decoded.order_id, 999);
    EXPECT_EQ(decoded.sequence_within_msg, 3u);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(decoded.price, 12340000);
    EXPECT_EQ(decoded.quantity, 50u);
}

TEST(ImpactCodecTest, RoundTripDealTrade) {
    DealTrade orig{};
    orig.instrument_id = 42;
    orig.deal_id = 8888;
    orig.price = 51000000;
    orig.quantity = 10;
    orig.aggressor_side = static_cast<uint8_t>(Side::Sell);
    orig.timestamp = 1700000000000000000LL;

    char buf[64];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    DealTrade decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.instrument_id, 42);
    EXPECT_EQ(decoded.deal_id, 8888);
    EXPECT_EQ(decoded.price, 51000000);
    EXPECT_EQ(decoded.quantity, 10u);
    EXPECT_EQ(decoded.aggressor_side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(decoded.timestamp, 1700000000000000000LL);
}

TEST(ImpactCodecTest, RoundTripMarketStatus) {
    MarketStatus orig{};
    orig.instrument_id = 15;
    orig.trading_status = static_cast<uint8_t>(TradingStatus::Halt);

    char buf[16];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    MarketStatus decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.instrument_id, 15);
    EXPECT_EQ(decoded.trading_status, static_cast<uint8_t>(TradingStatus::Halt));
}

TEST(ImpactCodecTest, RoundTripSnapshotOrder) {
    SnapshotOrder orig{};
    orig.instrument_id = 20;
    orig.order_id = 7777;
    orig.side = static_cast<uint8_t>(Side::Buy);
    orig.price = 60000000;
    orig.quantity = 100;
    orig.sequence = 5;

    char buf[64];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    SnapshotOrder decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.instrument_id, 20);
    EXPECT_EQ(decoded.order_id, 7777);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(decoded.price, 60000000);
    EXPECT_EQ(decoded.quantity, 100u);
    EXPECT_EQ(decoded.sequence, 5u);
}

TEST(ImpactCodecTest, RoundTripPriceLevel) {
    PriceLevel orig{};
    orig.instrument_id = 9;
    orig.side = static_cast<uint8_t>(Side::Sell);
    orig.price = 33330000;
    orig.quantity = 200;
    orig.order_count = 12;

    char buf[32];
    ASSERT_NE(encode(buf, sizeof(buf), orig), nullptr);

    PriceLevel decoded{};
    ASSERT_NE(decode(buf, sizeof(buf), decoded), nullptr);
    EXPECT_EQ(decoded.instrument_id, 9);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(decoded.price, 33330000);
    EXPECT_EQ(decoded.quantity, 200u);
    EXPECT_EQ(decoded.order_count, 12u);
}

// ---------------------------------------------------------------------------
// Engine event -> encoder -> decoder -> verify: full pipeline round-trip.
// ---------------------------------------------------------------------------

TEST(ImpactCodecTest, DepthUpdateRoundTrip) {
    DepthUpdate evt{};
    evt.side = exchange::Side::Buy;
    evt.price = 50000000;   // engine fixed-point
    evt.total_qty = 100000; // 10 lots at PRICE_SCALE=10000
    evt.order_count = 5;
    evt.action = DepthUpdate::Add;
    evt.ts = 1700000000000000000LL;

    ImpactEncodeContext ctx{};
    ctx.instrument_id = 42;

    char buf[MAX_IMPACT_ENCODED_SIZE];
    size_t len = encode_depth_update(buf, sizeof(buf), evt, ctx);
    ASSERT_GT(len, 0u);

    Visitor v;
    size_t consumed = decode_messages(buf, len, v);
    EXPECT_EQ(consumed, len);

    EXPECT_EQ(v.bs_count, 1);
    EXPECT_EQ(v.pl_count, 1);
    EXPECT_EQ(v.be_count, 1);
    EXPECT_EQ(v.pl.instrument_id, 42);
    EXPECT_EQ(v.pl.price, 50000000);
    EXPECT_EQ(v.pl.quantity, 10u);
    EXPECT_EQ(v.pl.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(v.pl.order_count, 5u);
    EXPECT_EQ(v.bs.sequence_number, 1u);
    EXPECT_EQ(v.bs.timestamp, 1700000000000000000LL);
}

TEST(ImpactCodecTest, OrderCancelledRoundTrip) {
    OrderCancelled evt{};
    evt.id = 555;
    evt.ts = 1700000000000000000LL;
    evt.reason = CancelReason::UserRequested;

    ImpactEncodeContext ctx{};
    ctx.instrument_id = 7;

    char buf[MAX_IMPACT_ENCODED_SIZE];
    size_t len = encode_order_cancelled(
        buf, sizeof(buf), evt,
        exchange::Side::Sell, Price{40000000}, Quantity{50000},
        ctx);
    ASSERT_GT(len, 0u);

    Visitor v;
    size_t consumed = decode_messages(buf, len, v);
    EXPECT_EQ(consumed, len);

    EXPECT_EQ(v.ow_count, 1);
    EXPECT_EQ(v.ow.instrument_id, 7);
    EXPECT_EQ(v.ow.order_id, 555);
    EXPECT_EQ(v.ow.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(v.ow.price, 40000000);
    EXPECT_EQ(v.ow.quantity, 5u);  // 50000 / PRICE_SCALE = 5
}

TEST(ImpactCodecTest, TradeRoundTrip) {
    Trade evt{};
    evt.price = 51000000;
    evt.quantity = 30000;  // 3 lots
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.aggressor_side = exchange::Side::Buy;
    evt.ts = 1700000000000000000LL;

    ImpactEncodeContext ctx{};
    ctx.instrument_id = 99;

    char buf[MAX_IMPACT_ENCODED_SIZE];
    size_t len = encode_trade(buf, sizeof(buf), evt, ctx);
    ASSERT_GT(len, 0u);

    Visitor v;
    size_t consumed = decode_messages(buf, len, v);
    EXPECT_EQ(consumed, len);

    EXPECT_EQ(v.dt_count, 1);
    EXPECT_EQ(v.dt.instrument_id, 99);
    EXPECT_EQ(v.dt.price, 51000000);
    EXPECT_EQ(v.dt.quantity, 3u);
    EXPECT_EQ(v.dt.aggressor_side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(v.dt.deal_id, 1);  // first deal
    EXPECT_EQ(v.dt.timestamp, 1700000000000000000LL);
}

TEST(ImpactCodecTest, MarketStatusRoundTrip) {
    exchange::MarketStatus evt{};
    evt.state = SessionState::Halt;
    evt.ts = 1700000000000000000LL;

    ImpactEncodeContext ctx{};
    ctx.instrument_id = 15;

    char buf[MAX_IMPACT_ENCODED_SIZE];
    size_t len = encode_market_status(buf, sizeof(buf), evt, ctx);
    ASSERT_GT(len, 0u);

    Visitor v;
    size_t consumed = decode_messages(buf, len, v);
    EXPECT_EQ(consumed, len);

    EXPECT_EQ(v.ms_count, 1);
    EXPECT_EQ(v.ms.instrument_id, 15);
    EXPECT_EQ(v.ms.trading_status, static_cast<uint8_t>(TradingStatus::Halt));
}

// ---------------------------------------------------------------------------
// Multi-event stream: encode two bundles back-to-back, decode all.
// ---------------------------------------------------------------------------

TEST(ImpactCodecTest, MultiEventStream) {
    ImpactEncodeContext ctx{};
    ctx.instrument_id = 42;

    char buf[MAX_IMPACT_ENCODED_SIZE * 2];
    char* p = buf;

    // Event 1: Trade
    Trade t{};
    t.price = 50000000;
    t.quantity = 10000;
    t.aggressor_id = 1;
    t.resting_id = 2;
    t.aggressor_side = exchange::Side::Buy;
    t.ts = 100;

    size_t len1 = encode_trade(p, sizeof(buf), t, ctx);
    ASSERT_GT(len1, 0u);
    p += len1;

    // Event 2: MarketStatus
    exchange::MarketStatus ms{};
    ms.state = SessionState::Closed;
    ms.ts = 200;

    size_t len2 = encode_market_status(
        p, sizeof(buf) - len1, ms, ctx);
    ASSERT_GT(len2, 0u);

    size_t total = len1 + len2;

    Visitor v;
    size_t consumed = decode_messages(buf, total, v);
    EXPECT_EQ(consumed, total);

    EXPECT_EQ(v.bs_count, 2);
    EXPECT_EQ(v.be_count, 2);
    EXPECT_EQ(v.dt_count, 1);
    EXPECT_EQ(v.ms_count, 1);
    EXPECT_EQ(v.dt.price, 50000000);
    EXPECT_EQ(v.ms.trading_status, static_cast<uint8_t>(TradingStatus::Closed));
}

// ---------------------------------------------------------------------------
// Type mismatch: decode with wrong type returns nullptr.
// ---------------------------------------------------------------------------

TEST(ImpactCodecTest, TypeMismatchReturnsNull) {
    AddModifyOrder amo{};
    amo.instrument_id = 1;
    amo.order_id = 2;
    amo.side = 0;
    amo.price = 100;
    amo.quantity = 1;
    amo.is_implied = 0;
    amo.is_rfq = 0;
    amo.order_entry_date_time = 0;
    amo.sequence_within_msg = 0;

    char buf[64];
    ASSERT_NE(encode(buf, sizeof(buf), amo), nullptr);

    // Try to decode as OrderWithdrawal — type mismatch
    OrderWithdrawal wrong{};
    EXPECT_EQ(decode(buf, sizeof(buf), wrong), nullptr);
}

}  // namespace
}  // namespace exchange::ice::impact
