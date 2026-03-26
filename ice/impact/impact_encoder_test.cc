#include "ice/impact/impact_encoder.h"

#include <cstring>

#include "gtest/gtest.h"

namespace exchange::ice::impact {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ImpactEncodeContext MakeCtx() {
    ImpactEncodeContext ctx{};
    ctx.instrument_id = 12345;
    return ctx;
}

// Decode the BundleStart from the beginning of a buffer.
BundleStart decode_bundle_start(const char* buf) {
    BundleStart bs{};
    decode(buf, wire_size<BundleStart>(), bs);
    return bs;
}

// Decode the BundleEnd from a position in the buffer.
BundleEnd decode_bundle_end(const char* buf) {
    BundleEnd be{};
    decode(buf, wire_size<BundleEnd>(), be);
    return be;
}

// ---------------------------------------------------------------------------
// Price / quantity conversion
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, PriceConversion) {
    // Engine price is direct pass-through to iMpact.
    EXPECT_EQ(engine_price_to_wire(45002500), 45002500);
    EXPECT_EQ(engine_price_to_wire(0), 0);
    EXPECT_EQ(engine_price_to_wire(-10000), -10000);
}

TEST(ImpactEncoderTest, QuantityConversion) {
    EXPECT_EQ(engine_qty_to_wire(10000), 1u);    // 1 lot
    EXPECT_EQ(engine_qty_to_wire(100000), 10u);   // 10 lots
    EXPECT_EQ(engine_qty_to_wire(0), 0u);
}

// ---------------------------------------------------------------------------
// Side encoding
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, SideEncoding) {
    EXPECT_EQ(encode_side(exchange::Side::Buy),
              static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(encode_side(exchange::Side::Sell),
              static_cast<uint8_t>(Side::Sell));
}

// ---------------------------------------------------------------------------
// Trading status encoding
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, TradingStatusEncoding) {
    EXPECT_EQ(encode_trading_status(SessionState::PreOpen),
              static_cast<uint8_t>(TradingStatus::PreOpen));
    EXPECT_EQ(encode_trading_status(SessionState::OpeningAuction),
              static_cast<uint8_t>(TradingStatus::PreOpen));
    EXPECT_EQ(encode_trading_status(SessionState::Continuous),
              static_cast<uint8_t>(TradingStatus::Continuous));
    EXPECT_EQ(encode_trading_status(SessionState::Halt),
              static_cast<uint8_t>(TradingStatus::Halt));
    EXPECT_EQ(encode_trading_status(SessionState::VolatilityAuction),
              static_cast<uint8_t>(TradingStatus::Halt));
    EXPECT_EQ(encode_trading_status(SessionState::Closed),
              static_cast<uint8_t>(TradingStatus::Closed));
    EXPECT_EQ(encode_trading_status(SessionState::PreClose),
              static_cast<uint8_t>(TradingStatus::Closed));
    EXPECT_EQ(encode_trading_status(SessionState::ClosingAuction),
              static_cast<uint8_t>(TradingStatus::Closed));
}

// ---------------------------------------------------------------------------
// DepthUpdate -> AddModifyOrder (Add/Update action)
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeDepthUpdateAdd) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    DepthUpdate evt{};
    evt.side = exchange::Side::Buy;
    evt.price = 45002500;      // 4500.2500 in engine scale
    evt.total_qty = 100000;    // 10 lots
    evt.order_count = 5;
    evt.action = DepthUpdate::Add;
    evt.ts = 1000000000;

    size_t n = encode_depth_update(buf, sizeof(buf), evt, ctx);
    // BundleStart(3+14) + AddModifyOrder(3+39) + BundleEnd(3+4) = 66
    EXPECT_EQ(n, 66u);

    // Verify BundleStart.
    auto bs = decode_bundle_start(buf);
    EXPECT_EQ(bs.sequence_number, 1u);
    EXPECT_EQ(bs.message_count, 1u);
    EXPECT_EQ(bs.timestamp, 1000000000);

    // Verify AddModifyOrder.
    const char* p = buf + wire_size<BundleStart>();
    AddModifyOrder msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.price, 45002500);
    EXPECT_EQ(msg.quantity, 10u);
    EXPECT_EQ(msg.is_implied, static_cast<uint8_t>(YesNo::No));
    EXPECT_EQ(msg.is_rfq, static_cast<uint8_t>(YesNo::No));
    EXPECT_EQ(msg.order_entry_date_time, 1000000000);
    EXPECT_EQ(msg.sequence_within_msg, 1u);

    // Verify BundleEnd.
    auto be = decode_bundle_end(after);
    EXPECT_EQ(be.sequence_number, 1u);

    // Context updated.
    EXPECT_EQ(ctx.seq_num, 1u);
    EXPECT_EQ(ctx.order_seq, 1u);
}

TEST(ImpactEncoderTest, EncodeDepthUpdateSellUpdate) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    DepthUpdate evt{};
    evt.side = exchange::Side::Sell;
    evt.price = 45010000;
    evt.total_qty = 50000;
    evt.order_count = 3;
    evt.action = DepthUpdate::Update;
    evt.ts = 2000000000;

    size_t n = encode_depth_update(buf, sizeof(buf), evt, ctx);
    EXPECT_EQ(n, 66u);  // same size: AddModifyOrder

    const char* p = buf + wire_size<BundleStart>();
    AddModifyOrder msg{};
    decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(msg.price, 45010000);
    EXPECT_EQ(msg.quantity, 5u);
}

// ---------------------------------------------------------------------------
// DepthUpdate -> OrderWithdrawal (Remove action)
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeDepthUpdateRemove) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    DepthUpdate evt{};
    evt.side = exchange::Side::Buy;
    evt.price = 44990000;
    evt.total_qty = 0;
    evt.order_count = 0;
    evt.action = DepthUpdate::Remove;
    evt.ts = 3000000000;

    size_t n = encode_depth_update(buf, sizeof(buf), evt, ctx);
    // BundleStart(17) + OrderWithdrawal(3+29=32) + BundleEnd(7) = 56
    EXPECT_EQ(n, 56u);

    const char* p = buf + wire_size<BundleStart>();
    OrderWithdrawal msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.price, 44990000);
    EXPECT_EQ(msg.quantity, 0u);
}

// ---------------------------------------------------------------------------
// OrderCancelled -> OrderWithdrawal
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeOrderCancelled) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    OrderCancelled evt{};
    evt.id = 42;
    evt.ts = 4000000000;
    evt.reason = CancelReason::UserRequested;

    size_t n = encode_order_cancelled(
        buf, sizeof(buf), evt,
        exchange::Side::Sell, 45005000, 30000,
        ctx);
    // BundleStart(17) + OrderWithdrawal(32) + BundleEnd(7) = 56
    EXPECT_EQ(n, 56u);

    const char* p = buf + wire_size<BundleStart>();
    OrderWithdrawal msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.order_id, 42);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(msg.price, 45005000);
    EXPECT_EQ(msg.quantity, 3u);  // 30000 / 10000

    auto be = decode_bundle_end(after);
    EXPECT_EQ(be.sequence_number, 1u);
}

// ---------------------------------------------------------------------------
// Trade -> DealTrade
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeTrade) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    Trade evt{};
    evt.price = 45005000;
    evt.quantity = 30000;
    evt.aggressor_id = 100;
    evt.resting_id = 50;
    evt.aggressor_side = exchange::Side::Buy;
    evt.ts = 5000000000;

    size_t n = encode_trade(buf, sizeof(buf), evt, ctx);
    // BundleStart(17) + DealTrade(3+33=36) + BundleEnd(7) = 60
    EXPECT_EQ(n, 60u);

    auto bs = decode_bundle_start(buf);
    EXPECT_EQ(bs.sequence_number, 1u);
    EXPECT_EQ(bs.timestamp, 5000000000);

    const char* p = buf + wire_size<BundleStart>();
    DealTrade msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.deal_id, 1);
    EXPECT_EQ(msg.price, 45005000);
    EXPECT_EQ(msg.quantity, 3u);
    EXPECT_EQ(msg.aggressor_side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.timestamp, 5000000000);

    auto be = decode_bundle_end(after);
    EXPECT_EQ(be.sequence_number, 1u);
}

TEST(ImpactEncoderTest, TradeSellAggressor) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    Trade evt{};
    evt.price = 44990000;
    evt.quantity = 10000;
    evt.aggressor_side = exchange::Side::Sell;
    evt.ts = 6000000000;

    encode_trade(buf, sizeof(buf), evt, ctx);

    const char* p = buf + wire_size<BundleStart>();
    DealTrade msg{};
    decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    EXPECT_EQ(msg.aggressor_side, static_cast<uint8_t>(Side::Sell));
}

TEST(ImpactEncoderTest, DealIdIncrements) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    Trade evt{};
    evt.price = 45000000;
    evt.quantity = 10000;
    evt.aggressor_side = exchange::Side::Buy;
    evt.ts = 1;

    encode_trade(buf, sizeof(buf), evt, ctx);
    const char* p1 = buf + wire_size<BundleStart>();
    DealTrade msg1{};
    decode(p1, sizeof(buf), msg1);
    EXPECT_EQ(msg1.deal_id, 1);

    encode_trade(buf, sizeof(buf), evt, ctx);
    const char* p2 = buf + wire_size<BundleStart>();
    DealTrade msg2{};
    decode(p2, sizeof(buf), msg2);
    EXPECT_EQ(msg2.deal_id, 2);

    EXPECT_EQ(ctx.next_deal_id, 3);
}

// ---------------------------------------------------------------------------
// MarketStatus -> impact::MarketStatus
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeMarketStatusContinuous) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    exchange::MarketStatus evt{};
    evt.state = SessionState::Continuous;
    evt.ts = 7000000000;

    size_t n = encode_market_status(buf, sizeof(buf), evt, ctx);
    // BundleStart(17) + MarketStatus(3+5=8) + BundleEnd(7) = 32
    EXPECT_EQ(n, 32u);

    const char* p = buf + wire_size<BundleStart>();
    impact::MarketStatus msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.trading_status,
              static_cast<uint8_t>(TradingStatus::Continuous));
}

TEST(ImpactEncoderTest, EncodeMarketStatusHalt) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    exchange::MarketStatus evt{};
    evt.state = SessionState::Halt;
    evt.ts = 8000000000;

    encode_market_status(buf, sizeof(buf), evt, ctx);

    const char* p = buf + wire_size<BundleStart>();
    impact::MarketStatus msg{};
    decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    EXPECT_EQ(msg.trading_status,
              static_cast<uint8_t>(TradingStatus::Halt));
}

TEST(ImpactEncoderTest, AllSessionStatesEncode) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();

    SessionState states[] = {
        SessionState::Closed,
        SessionState::PreOpen,
        SessionState::OpeningAuction,
        SessionState::Continuous,
        SessionState::PreClose,
        SessionState::ClosingAuction,
        SessionState::Halt,
        SessionState::VolatilityAuction,
    };

    for (auto state : states) {
        exchange::MarketStatus evt{};
        evt.state = state;
        evt.ts = 1000000000;
        size_t n = encode_market_status(buf, sizeof(buf), evt, ctx);
        EXPECT_GT(n, 0u) << "state=" << static_cast<int>(state);
    }
}

// ---------------------------------------------------------------------------
// Sequence number increments across calls
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, SeqNumIncrementsAcrossCalls) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    auto ctx = MakeCtx();
    EXPECT_EQ(ctx.seq_num, 0u);

    DepthUpdate d{};
    d.action = DepthUpdate::Add;
    d.ts = 1;
    encode_depth_update(buf, sizeof(buf), d, ctx);
    EXPECT_EQ(ctx.seq_num, 1u);

    Trade t{};
    t.quantity = 10000;
    t.aggressor_side = exchange::Side::Buy;
    t.ts = 2;
    encode_trade(buf, sizeof(buf), t, ctx);
    EXPECT_EQ(ctx.seq_num, 2u);

    exchange::MarketStatus ms{};
    ms.state = SessionState::Halt;
    ms.ts = 3;
    encode_market_status(buf, sizeof(buf), ms, ctx);
    EXPECT_EQ(ctx.seq_num, 3u);
}

// ---------------------------------------------------------------------------
// Buffer too small
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeFailsOnSmallBuffer) {
    char buf[4];  // way too small
    auto ctx = MakeCtx();

    DepthUpdate d{};
    d.action = DepthUpdate::Add;
    d.ts = 1;
    size_t n = encode_depth_update(buf, sizeof(buf), d, ctx);
    EXPECT_EQ(n, 0u);
}

}  // namespace
}  // namespace exchange::ice::impact
