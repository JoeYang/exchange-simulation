#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_encoder.h"
#include "ice/impact/impact_messages.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace exchange::ice::impact {
namespace {

// Recording visitor that captures decoded messages.
struct RecordingVisitor {
    std::vector<BundleStart>    bundle_starts;
    std::vector<BundleEnd>      bundle_ends;
    std::vector<AddModifyOrder> add_modify_orders;
    std::vector<OrderWithdrawal> withdrawals;
    std::vector<DealTrade>      trades;
    std::vector<MarketStatus>   statuses;
    std::vector<SnapshotOrder>  snapshots;
    std::vector<PriceLevel>     price_levels;
    std::vector<InstrumentDefinition> instrument_defs;

    void on_bundle_start(const BundleStart& m)    { bundle_starts.push_back(m); }
    void on_bundle_end(const BundleEnd& m)        { bundle_ends.push_back(m); }
    void on_add_modify_order(const AddModifyOrder& m) { add_modify_orders.push_back(m); }
    void on_order_withdrawal(const OrderWithdrawal& m) { withdrawals.push_back(m); }
    void on_deal_trade(const DealTrade& m)        { trades.push_back(m); }
    void on_market_status(const MarketStatus& m)  { statuses.push_back(m); }
    void on_snapshot_order(const SnapshotOrder& m) { snapshots.push_back(m); }
    void on_price_level(const PriceLevel& m)      { price_levels.push_back(m); }
    void on_instrument_def(const InstrumentDefinition& m) { instrument_defs.push_back(m); }
};

// --- Single message decode ---

TEST(ImpactDecoderTest, DecodeAddModifyOrder) {
    AddModifyOrder orig{};
    orig.instrument_id = 42;
    orig.order_id = 1001;
    orig.sequence_within_msg = 1;
    orig.side = static_cast<uint8_t>(Side::Buy);
    orig.price = 50000000;
    orig.quantity = 10;
    orig.is_implied = static_cast<uint8_t>(YesNo::No);
    orig.is_rfq = static_cast<uint8_t>(YesNo::No);
    orig.order_entry_date_time = 1700000000000000000LL;

    char buf[128];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, static_cast<size_t>(end - buf), v);
    EXPECT_EQ(consumed, static_cast<size_t>(end - buf));
    ASSERT_EQ(v.add_modify_orders.size(), 1u);

    const auto& m = v.add_modify_orders[0];
    EXPECT_EQ(m.instrument_id, 42);
    EXPECT_EQ(m.order_id, 1001);
    EXPECT_EQ(m.price, 50000000);
    EXPECT_EQ(m.quantity, 10u);
    EXPECT_EQ(m.side, static_cast<uint8_t>(Side::Buy));
}

TEST(ImpactDecoderTest, DecodeOrderWithdrawal) {
    OrderWithdrawal orig{};
    orig.instrument_id = 7;
    orig.order_id = 555;
    orig.sequence_within_msg = 2;
    orig.side = static_cast<uint8_t>(Side::Sell);
    orig.price = 40000000;
    orig.quantity = 5;

    char buf[64];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, static_cast<size_t>(end - buf), v);
    EXPECT_EQ(consumed, static_cast<size_t>(end - buf));
    ASSERT_EQ(v.withdrawals.size(), 1u);
    EXPECT_EQ(v.withdrawals[0].order_id, 555);
    EXPECT_EQ(v.withdrawals[0].side, static_cast<uint8_t>(Side::Sell));
}

TEST(ImpactDecoderTest, DecodeDealTrade) {
    DealTrade orig{};
    orig.instrument_id = 10;
    orig.deal_id = 99;
    orig.price = 51000000;
    orig.quantity = 3;
    orig.aggressor_side = static_cast<uint8_t>(Side::Buy);
    orig.timestamp = 1700000000000000000LL;

    char buf[64];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    decode_messages(buf, static_cast<size_t>(end - buf), v);
    ASSERT_EQ(v.trades.size(), 1u);
    EXPECT_EQ(v.trades[0].deal_id, 99);
    EXPECT_EQ(v.trades[0].price, 51000000);
    EXPECT_EQ(v.trades[0].quantity, 3u);
}

TEST(ImpactDecoderTest, DecodeMarketStatus) {
    MarketStatus orig{};
    orig.instrument_id = 20;
    orig.trading_status = static_cast<uint8_t>(TradingStatus::Halt);

    char buf[32];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    decode_messages(buf, static_cast<size_t>(end - buf), v);
    ASSERT_EQ(v.statuses.size(), 1u);
    EXPECT_EQ(v.statuses[0].instrument_id, 20);
    EXPECT_EQ(v.statuses[0].trading_status, static_cast<uint8_t>(TradingStatus::Halt));
}

TEST(ImpactDecoderTest, DecodeSnapshotOrder) {
    SnapshotOrder orig{};
    orig.instrument_id = 15;
    orig.order_id = 777;
    orig.side = static_cast<uint8_t>(Side::Buy);
    orig.price = 60000000;
    orig.quantity = 20;
    orig.sequence = 1;

    char buf[64];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    decode_messages(buf, static_cast<size_t>(end - buf), v);
    ASSERT_EQ(v.snapshots.size(), 1u);
    EXPECT_EQ(v.snapshots[0].order_id, 777);
    EXPECT_EQ(v.snapshots[0].price, 60000000);
}

TEST(ImpactDecoderTest, DecodePriceLevel) {
    PriceLevel orig{};
    orig.instrument_id = 5;
    orig.side = static_cast<uint8_t>(Side::Sell);
    orig.price = 45000000;
    orig.quantity = 100;
    orig.order_count = 7;

    char buf[32];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    decode_messages(buf, static_cast<size_t>(end - buf), v);
    ASSERT_EQ(v.price_levels.size(), 1u);
    EXPECT_EQ(v.price_levels[0].price, 45000000);
    EXPECT_EQ(v.price_levels[0].order_count, 7u);
}

TEST(ImpactDecoderTest, DecodeInstrumentDefinition) {
    InstrumentDefinition orig{};
    orig.instrument_id = 1;
    std::memset(orig.symbol, 0, sizeof(orig.symbol));
    std::memcpy(orig.symbol, "B", 1);
    std::memset(orig.description, 0, sizeof(orig.description));
    std::memcpy(orig.description, "Brent Crude", 11);
    std::memset(orig.product_group, 0, sizeof(orig.product_group));
    std::memcpy(orig.product_group, "Energy", 6);
    orig.tick_size = 100;
    orig.lot_size = 10000;
    orig.max_order_size = 50000000;
    orig.match_algo = 0;
    std::memset(orig.currency, 0, sizeof(orig.currency));
    std::memcpy(orig.currency, "USD", 3);

    char buf[128];
    char* end = encode(buf, sizeof(buf), orig);
    ASSERT_NE(end, nullptr);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, static_cast<size_t>(end - buf), v);
    EXPECT_EQ(consumed, static_cast<size_t>(end - buf));
    ASSERT_EQ(v.instrument_defs.size(), 1u);

    const auto& m = v.instrument_defs[0];
    EXPECT_EQ(m.instrument_id, 1);
    EXPECT_EQ(std::string(m.symbol, 1), "B");
    EXPECT_EQ(m.tick_size, 100);
    EXPECT_EQ(m.lot_size, 10000);
    EXPECT_EQ(m.match_algo, 0u);
    EXPECT_EQ(std::string(m.currency, 3), "USD");
}

TEST(ImpactDecoderTest, DecodeInstrumentDefInterleavedWithOtherMsgs) {
    // Encode an InstrumentDefinition followed by a MarketStatus.
    // Verify both are decoded correctly.
    char buf[256];
    char* p = buf;

    InstrumentDefinition idef{};
    idef.instrument_id = 7;
    std::memset(idef.symbol, 0, sizeof(idef.symbol));
    std::memcpy(idef.symbol, "I", 1);
    idef.tick_size = 50;
    idef.lot_size = 10000;
    idef.max_order_size = 100000000;
    idef.match_algo = 1;  // GTBPR
    std::memset(idef.currency, 0, sizeof(idef.currency));
    std::memcpy(idef.currency, "EUR", 3);
    std::memset(idef.description, 0, sizeof(idef.description));
    std::memset(idef.product_group, 0, sizeof(idef.product_group));

    p = encode(p, sizeof(buf) - static_cast<size_t>(p - buf), idef);
    ASSERT_NE(p, nullptr);

    MarketStatus ms{};
    ms.instrument_id = 7;
    ms.trading_status = static_cast<uint8_t>(TradingStatus::Continuous);
    p = encode(p, sizeof(buf) - static_cast<size_t>(p - buf), ms);
    ASSERT_NE(p, nullptr);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, static_cast<size_t>(p - buf), v);
    EXPECT_EQ(consumed, static_cast<size_t>(p - buf));

    ASSERT_EQ(v.instrument_defs.size(), 1u);
    EXPECT_EQ(v.instrument_defs[0].instrument_id, 7);
    EXPECT_EQ(v.instrument_defs[0].match_algo, 1u);

    ASSERT_EQ(v.statuses.size(), 1u);
    EXPECT_EQ(v.statuses[0].instrument_id, 7);
}

// --- Multi-message stream decode ---

TEST(ImpactDecoderTest, DecodeMultipleMessages) {
    char buf[256];
    char* p = buf;

    BundleStart bs{};
    bs.sequence_number = 1;
    bs.message_count = 1;
    bs.timestamp = 1700000000000000000LL;
    p = encode(p, sizeof(buf) - static_cast<size_t>(p - buf), bs);
    ASSERT_NE(p, nullptr);

    AddModifyOrder amo{};
    amo.instrument_id = 42;
    amo.order_id = 100;
    amo.sequence_within_msg = 1;
    amo.side = static_cast<uint8_t>(Side::Buy);
    amo.price = 50000000;
    amo.quantity = 10;
    amo.is_implied = 0;
    amo.is_rfq = 0;
    amo.order_entry_date_time = 1700000000000000000LL;
    p = encode(p, sizeof(buf) - static_cast<size_t>(p - buf), amo);
    ASSERT_NE(p, nullptr);

    BundleEnd be{};
    be.sequence_number = 1;
    p = encode(p, sizeof(buf) - static_cast<size_t>(p - buf), be);
    ASSERT_NE(p, nullptr);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, static_cast<size_t>(p - buf), v);
    EXPECT_EQ(consumed, static_cast<size_t>(p - buf));

    EXPECT_EQ(v.bundle_starts.size(), 1u);
    EXPECT_EQ(v.bundle_starts[0].sequence_number, 1u);
    EXPECT_EQ(v.add_modify_orders.size(), 1u);
    EXPECT_EQ(v.bundle_ends.size(), 1u);
}

// --- Encoder round-trip: encode via encoder, decode via decoder ---

TEST(ImpactDecoderTest, RoundTripDepthUpdate) {
    DepthUpdate evt{};
    evt.side = exchange::Side::Buy;
    evt.price = 50000000;
    evt.total_qty = 100000;  // 10 lots
    evt.order_count = 3;
    evt.action = DepthUpdate::Add;
    evt.ts = 1700000000000000000LL;

    ImpactEncodeContext enc_ctx{};
    enc_ctx.instrument_id = 42;

    char buf[MAX_IMPACT_ENCODED_SIZE];
    size_t len = encode_depth_update(buf, sizeof(buf), evt, enc_ctx);
    ASSERT_GT(len, 0u);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, len, v);
    EXPECT_EQ(consumed, len);

    ASSERT_EQ(v.bundle_starts.size(), 1u);
    ASSERT_EQ(v.price_levels.size(), 1u);
    ASSERT_EQ(v.bundle_ends.size(), 1u);

    EXPECT_EQ(v.price_levels[0].instrument_id, 42);
    EXPECT_EQ(v.price_levels[0].price, 50000000);
    EXPECT_EQ(v.price_levels[0].quantity, 10u);
    EXPECT_EQ(v.price_levels[0].order_count, 3u);
    EXPECT_EQ(v.price_levels[0].side, static_cast<uint8_t>(Side::Buy));
}

TEST(ImpactDecoderTest, RoundTripTrade) {
    Trade evt{};
    evt.price = 51000000;
    evt.quantity = 30000;  // 3 lots
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.aggressor_side = exchange::Side::Sell;
    evt.ts = 1700000000000000000LL;

    ImpactEncodeContext enc_ctx{};
    enc_ctx.instrument_id = 7;

    char buf[MAX_IMPACT_ENCODED_SIZE];
    size_t len = encode_trade(buf, sizeof(buf), evt, enc_ctx);
    ASSERT_GT(len, 0u);

    RecordingVisitor v;
    decode_messages(buf, len, v);

    ASSERT_EQ(v.trades.size(), 1u);
    EXPECT_EQ(v.trades[0].instrument_id, 7);
    EXPECT_EQ(v.trades[0].price, 51000000);
    EXPECT_EQ(v.trades[0].quantity, 3u);
    EXPECT_EQ(v.trades[0].aggressor_side, static_cast<uint8_t>(Side::Sell));
}

// --- Error handling ---

TEST(ImpactDecoderTest, EmptyBufferReturnsZero) {
    RecordingVisitor v;
    EXPECT_EQ(decode_messages(nullptr, 0, v), 0u);
}

TEST(ImpactDecoderTest, TruncatedHeaderReturnsConsumed) {
    char buf[2] = {static_cast<char>(MessageType::AddModifyOrder), 0};
    RecordingVisitor v;
    // Buffer too short for header — stops at 0
    EXPECT_EQ(decode_messages(buf, 2, v), 0u);
}

TEST(ImpactDecoderTest, UnknownMessageTypeSkips) {
    // Craft a message with unknown type 'Z'
    ImpactMessageHeader hdr{};
    hdr.msg_type = 'Z';
    hdr.body_length = sizeof(ImpactMessageHeader) + 4;  // 4 bytes of payload

    char buf[16];
    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memset(buf + sizeof(hdr), 0, 4);

    RecordingVisitor v;
    size_t consumed = decode_messages(buf, sizeof(hdr) + 4, v);
    // Should skip unknown message and consume all bytes
    EXPECT_EQ(consumed, sizeof(hdr) + 4u);
}

}  // namespace
}  // namespace exchange::ice::impact
