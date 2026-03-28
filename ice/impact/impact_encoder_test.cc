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
// DepthUpdate -> PriceLevel (Add/Update action)
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
    // BundleStart(3+14) + PriceLevel(3+19) + BundleEnd(3+4) = 46
    EXPECT_EQ(n, 46u);

    // Verify BundleStart.
    auto bs = decode_bundle_start(buf);
    EXPECT_EQ(bs.sequence_number, 1u);
    EXPECT_EQ(bs.message_count, 1u);
    EXPECT_EQ(bs.timestamp, 1000000000);

    // Verify PriceLevel.
    const char* p = buf + wire_size<BundleStart>();
    PriceLevel msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.price, 45002500);
    EXPECT_EQ(msg.quantity, 10u);
    EXPECT_EQ(msg.order_count, 5u);

    // Verify BundleEnd.
    auto be = decode_bundle_end(after);
    EXPECT_EQ(be.sequence_number, 1u);

    // Context updated.
    EXPECT_EQ(ctx.seq_num, 1u);
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
    EXPECT_EQ(n, 46u);  // same size: PriceLevel

    const char* p = buf + wire_size<BundleStart>();
    PriceLevel msg{};
    decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(msg.price, 45010000);
    EXPECT_EQ(msg.quantity, 5u);
    EXPECT_EQ(msg.order_count, 3u);
}

// ---------------------------------------------------------------------------
// DepthUpdate -> PriceLevel (Remove action: qty=0, order_count=0)
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
    // BundleStart(17) + PriceLevel(3+19=22) + BundleEnd(7) = 46
    EXPECT_EQ(n, 46u);

    const char* p = buf + wire_size<BundleStart>();
    PriceLevel msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(msg.instrument_id, 12345);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.price, 44990000);
    EXPECT_EQ(msg.quantity, 0u);
    EXPECT_EQ(msg.order_count, 0u);
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

    OrderCancelled oc{};
    oc.id = 99;
    oc.ts = 3;
    oc.reason = CancelReason::UserRequested;
    encode_order_cancelled(buf, sizeof(buf), oc,
                           exchange::Side::Buy, 45000000, 10000, ctx);
    EXPECT_EQ(ctx.seq_num, 3u);

    exchange::MarketStatus ms{};
    ms.state = SessionState::Halt;
    ms.ts = 4;
    encode_market_status(buf, sizeof(buf), ms, ctx);
    EXPECT_EQ(ctx.seq_num, 4u);
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

// ---------------------------------------------------------------------------
// InstrumentDefinition encoding
// ---------------------------------------------------------------------------

TEST(ImpactEncoderTest, EncodeInstrumentDefinitionBrent) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    ImpactEncodeContext ctx{};

    IceProductConfig brent{};
    brent.instrument_id = 1;
    brent.symbol = "B";
    brent.description = "Brent Crude Futures";
    brent.product_group = "Energy";
    brent.tick_size = 100;
    brent.lot_size = 10000;
    brent.max_order_size = 50000000;
    brent.match_algo = IceMatchAlgo::FIFO;

    size_t n = encode_instrument_definition(buf, sizeof(buf), brent, ctx);
    // BundleStart(3+14) + InstrumentDefinition(3+89) + BundleEnd(3+4) = 116
    EXPECT_EQ(n, 116u);

    // Verify BundleStart
    auto bs = decode_bundle_start(buf);
    EXPECT_EQ(bs.sequence_number, 1u);
    EXPECT_EQ(bs.message_count, 1u);

    // Decode the InstrumentDefinition
    const char* p = buf + wire_size<BundleStart>();
    InstrumentDefinition msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);

    EXPECT_EQ(msg.instrument_id, 1);
    EXPECT_EQ(std::string(msg.symbol, 1), "B");
    EXPECT_EQ(msg.symbol[1], '\0');
    EXPECT_EQ(std::string(msg.description, 19), "Brent Crude Futures");
    EXPECT_EQ(std::string(msg.product_group, 6), "Energy");
    EXPECT_EQ(msg.tick_size, 100);
    EXPECT_EQ(msg.lot_size, 10000);
    EXPECT_EQ(msg.max_order_size, 50000000);
    EXPECT_EQ(msg.match_algo, 0u);  // FIFO
    EXPECT_EQ(std::string(msg.currency, 3), "USD");

    // Verify BundleEnd
    auto be = decode_bundle_end(after);
    EXPECT_EQ(be.sequence_number, 1u);

    EXPECT_EQ(ctx.seq_num, 1u);
}

TEST(ImpactEncoderTest, EncodeInstrumentDefinitionGTBPR) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    ImpactEncodeContext ctx{};

    IceProductConfig euribor{};
    euribor.instrument_id = 7;
    euribor.symbol = "I";
    euribor.description = "Three-Month Euribor";
    euribor.product_group = "STIR";
    euribor.tick_size = 50;
    euribor.lot_size = 10000;
    euribor.max_order_size = 100000000;
    euribor.match_algo = IceMatchAlgo::GTBPR;

    size_t n = encode_instrument_definition(buf, sizeof(buf), euribor, ctx);
    EXPECT_EQ(n, 116u);

    const char* p = buf + wire_size<BundleStart>();
    InstrumentDefinition msg{};
    auto* after = decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    ASSERT_NE(after, nullptr);

    EXPECT_EQ(msg.instrument_id, 7);
    EXPECT_EQ(msg.match_algo, 1u);  // GTBPR
    EXPECT_EQ(msg.tick_size, 50);
    EXPECT_EQ(std::string(msg.currency, 3), "USD");
}

TEST(ImpactEncoderTest, EncodeInstrumentDefinitionGBPCurrency) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    ImpactEncodeContext ctx{};

    IceProductConfig cocoa{};
    cocoa.instrument_id = 4;
    cocoa.symbol = "C";
    cocoa.description = "London Cocoa";
    cocoa.product_group = "Softs";
    cocoa.tick_size = 10000;
    cocoa.lot_size = 10000;
    cocoa.max_order_size = 10000000;
    cocoa.match_algo = IceMatchAlgo::FIFO;

    size_t n = encode_instrument_definition(buf, sizeof(buf), cocoa, ctx);
    EXPECT_EQ(n, 116u);

    const char* p = buf + wire_size<BundleStart>();
    InstrumentDefinition msg{};
    decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);
    EXPECT_EQ(std::string(msg.currency, 3), "GBP");
}

TEST(ImpactEncoderTest, EncodeInstrumentDefinitionSymbolTruncation) {
    char buf[MAX_IMPACT_ENCODED_SIZE];
    ImpactEncodeContext ctx{};

    IceProductConfig product{};
    product.instrument_id = 99;
    product.symbol = "LONGERSYM";  // 9 chars, field is 8
    product.description = "Test";
    product.product_group = "Test";
    product.tick_size = 100;
    product.lot_size = 10000;
    product.max_order_size = 10000000;
    product.match_algo = IceMatchAlgo::FIFO;

    size_t n = encode_instrument_definition(buf, sizeof(buf), product, ctx);
    EXPECT_EQ(n, 116u);

    const char* p = buf + wire_size<BundleStart>();
    InstrumentDefinition msg{};
    decode(p, sizeof(buf) - wire_size<BundleStart>(), msg);

    // Should be truncated to 7 chars + null
    EXPECT_EQ(std::string(msg.symbol, 7), "LONGERS");
    EXPECT_EQ(msg.symbol[7], '\0');
}

}  // namespace
}  // namespace exchange::ice::impact
