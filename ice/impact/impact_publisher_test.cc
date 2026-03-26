#include "ice/impact/impact_publisher.h"
#include "ice/impact/impact_messages.h"

#include <cstring>

#include "gtest/gtest.h"

namespace exchange::ice::impact {
namespace {

// ---------------------------------------------------------------------------
// Helpers to decode bundled packets
// ---------------------------------------------------------------------------

// Decode the inner message (past BundleStart header) from a packet.
template <typename MsgT>
MsgT decode_inner(const ImpactPacket& pkt) {
    const char* p = pkt.bytes() + wire_size<BundleStart>();
    MsgT msg{};
    decode(p, pkt.len - wire_size<BundleStart>(), msg);
    return msg;
}

BundleStart decode_bundle_start(const ImpactPacket& pkt) {
    BundleStart bs{};
    decode(pkt.bytes(), pkt.len, bs);
    return bs;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, DefaultConstruction) {
    ImpactFeedPublisher pub;
    EXPECT_EQ(pub.packet_count(), 0u);
    EXPECT_EQ(pub.seq_num(), 0u);
}

TEST(ImpactPublisherTest, ConstructionWithInstrumentId) {
    ImpactFeedPublisher pub(12345);
    EXPECT_EQ(pub.context().instrument_id, 12345);
}

// ---------------------------------------------------------------------------
// DepthUpdate -> AddModifyOrder
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, DepthUpdateAdd) {
    ImpactFeedPublisher pub(100);

    DepthUpdate evt{};
    evt.side = exchange::Side::Buy;
    evt.price = 45000000;
    evt.total_qty = 50000;
    evt.order_count = 3;
    evt.action = DepthUpdate::Add;
    evt.ts = 1000000000;

    pub.on_depth_update(evt);

    ASSERT_EQ(pub.packet_count(), 1u);
    EXPECT_EQ(pub.seq_num(), 1u);

    auto bs = decode_bundle_start(pub.packets()[0]);
    EXPECT_EQ(bs.sequence_number, 1u);
    EXPECT_EQ(bs.message_count, 1u);
    EXPECT_EQ(bs.timestamp, 1000000000);

    auto msg = decode_inner<AddModifyOrder>(pub.packets()[0]);
    EXPECT_EQ(msg.instrument_id, 100);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.price, 45000000);
    EXPECT_EQ(msg.quantity, 5u);  // 50000 / 10000
}

TEST(ImpactPublisherTest, DepthUpdateRemove) {
    ImpactFeedPublisher pub(100);

    DepthUpdate evt{};
    evt.side = exchange::Side::Sell;
    evt.price = 45010000;
    evt.total_qty = 0;
    evt.order_count = 0;
    evt.action = DepthUpdate::Remove;
    evt.ts = 2000000000;

    pub.on_depth_update(evt);

    ASSERT_EQ(pub.packet_count(), 1u);

    auto msg = decode_inner<OrderWithdrawal>(pub.packets()[0]);
    EXPECT_EQ(msg.instrument_id, 100);
    EXPECT_EQ(msg.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(msg.price, 45010000);
    EXPECT_EQ(msg.quantity, 0u);
}

// ---------------------------------------------------------------------------
// Trade -> DealTrade
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, TradeEvent) {
    ImpactFeedPublisher pub(200);

    Trade evt{};
    evt.price = 45005000;
    evt.quantity = 30000;
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.aggressor_side = exchange::Side::Buy;
    evt.ts = 3000000000;

    pub.on_trade(evt);

    ASSERT_EQ(pub.packet_count(), 1u);

    auto msg = decode_inner<DealTrade>(pub.packets()[0]);
    EXPECT_EQ(msg.instrument_id, 200);
    EXPECT_EQ(msg.deal_id, 1);
    EXPECT_EQ(msg.price, 45005000);
    EXPECT_EQ(msg.quantity, 3u);
    EXPECT_EQ(msg.aggressor_side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(msg.timestamp, 3000000000);
}

TEST(ImpactPublisherTest, DealIdIncrementsAcrossTrades) {
    ImpactFeedPublisher pub(200);

    Trade evt{};
    evt.price = 45000000;
    evt.quantity = 10000;
    evt.aggressor_side = exchange::Side::Sell;
    evt.ts = 1;

    pub.on_trade(evt);
    pub.on_trade(evt);
    pub.on_trade(evt);

    ASSERT_EQ(pub.packet_count(), 3u);

    auto msg1 = decode_inner<DealTrade>(pub.packets()[0]);
    auto msg2 = decode_inner<DealTrade>(pub.packets()[1]);
    auto msg3 = decode_inner<DealTrade>(pub.packets()[2]);
    EXPECT_EQ(msg1.deal_id, 1);
    EXPECT_EQ(msg2.deal_id, 2);
    EXPECT_EQ(msg3.deal_id, 3);
}

// ---------------------------------------------------------------------------
// MarketStatus
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, MarketStatusContinuous) {
    ImpactFeedPublisher pub(300);

    exchange::MarketStatus evt{};
    evt.state = SessionState::Continuous;
    evt.ts = 4000000000;

    pub.on_market_status(evt);

    ASSERT_EQ(pub.packet_count(), 1u);

    auto msg = decode_inner<impact::MarketStatus>(pub.packets()[0]);
    EXPECT_EQ(msg.instrument_id, 300);
    EXPECT_EQ(msg.trading_status,
              static_cast<uint8_t>(TradingStatus::Continuous));
}

TEST(ImpactPublisherTest, MarketStatusHalt) {
    ImpactFeedPublisher pub(300);

    exchange::MarketStatus evt{};
    evt.state = SessionState::Halt;
    evt.ts = 5000000000;

    pub.on_market_status(evt);

    ASSERT_EQ(pub.packet_count(), 1u);

    auto msg = decode_inner<impact::MarketStatus>(pub.packets()[0]);
    EXPECT_EQ(msg.trading_status,
              static_cast<uint8_t>(TradingStatus::Halt));
}

// ---------------------------------------------------------------------------
// Passthrough events produce no packets
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, TopOfBookIgnored) {
    ImpactFeedPublisher pub(100);

    TopOfBook evt{};
    evt.best_bid = 45000000;
    evt.bid_qty = 10000;
    evt.best_ask = 45010000;
    evt.ask_qty = 10000;
    evt.ts = 1;

    pub.on_top_of_book(evt);
    EXPECT_EQ(pub.packet_count(), 0u);
}

TEST(ImpactPublisherTest, OrderBookActionIgnored) {
    ImpactFeedPublisher pub(100);

    OrderBookAction evt{};
    evt.id = 1;
    evt.side = exchange::Side::Buy;
    evt.price = 45000000;
    evt.qty = 10000;
    evt.action = OrderBookAction::Add;
    evt.ts = 1;

    pub.on_order_book_action(evt);
    EXPECT_EQ(pub.packet_count(), 0u);
}

TEST(ImpactPublisherTest, IndicativePriceIgnored) {
    ImpactFeedPublisher pub(100);

    IndicativePrice evt{};
    evt.price = 45000000;
    evt.ts = 1;

    pub.on_indicative_price(evt);
    EXPECT_EQ(pub.packet_count(), 0u);
}

// ---------------------------------------------------------------------------
// Sequence numbers increment across mixed event types
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, SeqNumIncrementsAcrossEventTypes) {
    ImpactFeedPublisher pub(100);

    DepthUpdate du{};
    du.action = DepthUpdate::Add;
    du.side = exchange::Side::Buy;
    du.ts = 1;
    pub.on_depth_update(du);

    Trade tr{};
    tr.price = 45000000;
    tr.quantity = 10000;
    tr.aggressor_side = exchange::Side::Buy;
    tr.ts = 2;
    pub.on_trade(tr);

    exchange::MarketStatus ms{};
    ms.state = SessionState::Halt;
    ms.ts = 3;
    pub.on_market_status(ms);

    EXPECT_EQ(pub.packet_count(), 3u);
    EXPECT_EQ(pub.seq_num(), 3u);

    // Verify each bundle has incrementing sequence numbers.
    auto bs1 = decode_bundle_start(pub.packets()[0]);
    auto bs2 = decode_bundle_start(pub.packets()[1]);
    auto bs3 = decode_bundle_start(pub.packets()[2]);
    EXPECT_EQ(bs1.sequence_number, 1u);
    EXPECT_EQ(bs2.sequence_number, 2u);
    EXPECT_EQ(bs3.sequence_number, 3u);
}

// ---------------------------------------------------------------------------
// Clear and accessors
// ---------------------------------------------------------------------------

TEST(ImpactPublisherTest, ClearResetsPackets) {
    ImpactFeedPublisher pub(100);

    DepthUpdate du{};
    du.action = DepthUpdate::Add;
    du.side = exchange::Side::Buy;
    du.ts = 1;
    pub.on_depth_update(du);

    EXPECT_EQ(pub.packet_count(), 1u);

    pub.clear();
    EXPECT_EQ(pub.packet_count(), 0u);

    // seq_num is NOT reset by clear — it tracks lifetime sequence.
    EXPECT_EQ(pub.seq_num(), 1u);
}

TEST(ImpactPublisherTest, ContextReflectsInstrumentId) {
    ImpactFeedPublisher pub(42);
    EXPECT_EQ(pub.context().instrument_id, 42);
}

}  // namespace
}  // namespace exchange::ice::impact
