#include "krx/fast/fast_publisher.h"
#include "krx/fast/fast_decoder.h"

#include <gtest/gtest.h>

namespace exchange::krx::fast {
namespace {

// Recording visitor to verify published packets decode correctly.
struct RecordingVisitor : public FastDecoderVisitorBase {
    std::vector<FastQuote>    quotes;
    std::vector<FastTrade>    trades;
    std::vector<FastStatus>   statuses;
    std::vector<FastSnapshot> snapshots;

    void on_quote(const FastQuote& q) { quotes.push_back(q); }
    void on_trade(const FastTrade& t) { trades.push_back(t); }
    void on_status(const FastStatus& s) { statuses.push_back(s); }
    void on_snapshot(const FastSnapshot& s) { snapshots.push_back(s); }
};

// ---------------------------------------------------------------------------
// on_top_of_book -> FastQuote
// ---------------------------------------------------------------------------

TEST(FastPublisher, TopOfBookProducesQuote) {
    FastFeedPublisher pub;

    TopOfBook tob{};
    tob.best_bid = 1005000;
    tob.bid_qty  = 100000;
    tob.best_ask = 1010000;
    tob.ask_qty  = 50000;
    tob.ts       = 42;

    pub.on_top_of_book(tob);
    ASSERT_EQ(pub.packet_count(), 1u);

    // Decode the packet
    const auto& pkt = pub.packets()[0];
    RecordingVisitor visitor;
    decode_message(pkt.bytes(), pkt.len, visitor);
    ASSERT_EQ(visitor.quotes.size(), 1u);

    const auto& q = visitor.quotes[0];
    EXPECT_EQ(q.bid_price, 1005000);
    EXPECT_EQ(q.bid_qty, 100000);
    EXPECT_EQ(q.ask_price, 1010000);
    EXPECT_EQ(q.ask_qty, 50000);
    EXPECT_EQ(q.timestamp, 42);
}

// ---------------------------------------------------------------------------
// on_trade -> FastTrade
// ---------------------------------------------------------------------------

TEST(FastPublisher, TradeProducesFastTrade) {
    FastFeedPublisher pub;

    Trade t{};
    t.price          = 2500000;
    t.quantity        = 10000;
    t.aggressor_side = Side::Sell;
    t.ts             = 99;

    pub.on_trade(t);
    ASSERT_EQ(pub.packet_count(), 1u);

    const auto& pkt = pub.packets()[0];
    RecordingVisitor visitor;
    decode_message(pkt.bytes(), pkt.len, visitor);
    ASSERT_EQ(visitor.trades.size(), 1u);

    const auto& ft = visitor.trades[0];
    EXPECT_EQ(ft.price, 2500000);
    EXPECT_EQ(ft.quantity, 10000);
    EXPECT_EQ(ft.aggressor_side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(ft.timestamp, 99);
}

// ---------------------------------------------------------------------------
// on_market_status -> FastStatus
// ---------------------------------------------------------------------------

TEST(FastPublisher, MarketStatusProducesFastStatus) {
    FastFeedPublisher pub;

    exchange::MarketStatus ms{};
    ms.state = SessionState::Halt;
    ms.ts    = 777;

    pub.on_market_status(ms);
    ASSERT_EQ(pub.packet_count(), 1u);

    const auto& pkt = pub.packets()[0];
    RecordingVisitor visitor;
    decode_message(pkt.bytes(), pkt.len, visitor);
    ASSERT_EQ(visitor.statuses.size(), 1u);

    EXPECT_EQ(visitor.statuses[0].session_state,
              static_cast<uint8_t>(SessionState::Halt));
    EXPECT_EQ(visitor.statuses[0].timestamp, 777);
}

// ---------------------------------------------------------------------------
// publish_snapshot -> FastSnapshot
// ---------------------------------------------------------------------------

TEST(FastPublisher, SnapshotPublish) {
    FastFeedPublisher pub;

    TopOfBook tob{};
    tob.best_bid = 500000;
    tob.bid_qty  = 100000;
    tob.best_ask = 510000;
    tob.ask_qty  = 200000;
    tob.ts       = 123;

    pub.publish_snapshot(tob);
    ASSERT_EQ(pub.packet_count(), 1u);

    const auto& pkt = pub.packets()[0];
    RecordingVisitor visitor;
    decode_message(pkt.bytes(), pkt.len, visitor);
    ASSERT_EQ(visitor.snapshots.size(), 1u);

    const auto& s = visitor.snapshots[0];
    EXPECT_EQ(s.bid_price, 500000);
    EXPECT_EQ(s.bid_qty, 100000);
    EXPECT_EQ(s.ask_price, 510000);
    EXPECT_EQ(s.ask_qty, 200000);
    EXPECT_EQ(s.bid_count, 1u);
    EXPECT_EQ(s.ask_count, 1u);
    EXPECT_EQ(s.timestamp, 123);
}

// ---------------------------------------------------------------------------
// Multiple events accumulate, clear resets
// ---------------------------------------------------------------------------

TEST(FastPublisher, MultipleEventsAccumulate) {
    FastFeedPublisher pub;

    TopOfBook tob{};
    tob.best_bid = 100;
    tob.ts = 1;
    pub.on_top_of_book(tob);

    Trade t{};
    t.price = 200;
    t.ts = 2;
    pub.on_trade(t);

    exchange::MarketStatus ms{};
    ms.state = SessionState::Continuous;
    ms.ts = 3;
    pub.on_market_status(ms);

    EXPECT_EQ(pub.packet_count(), 3u);

    pub.clear();
    EXPECT_EQ(pub.packet_count(), 0u);
}

// ---------------------------------------------------------------------------
// Passthrough events produce no packets
// ---------------------------------------------------------------------------

TEST(FastPublisher, PassthroughEventsIgnored) {
    FastFeedPublisher pub;

    pub.on_depth_update(DepthUpdate{});
    pub.on_order_book_action(OrderBookAction{});
    pub.on_indicative_price(IndicativePrice{});
    pub.on_lock_limit_triggered(LockLimitTriggered{});

    EXPECT_EQ(pub.packet_count(), 0u);
}

// ---------------------------------------------------------------------------
// Empty book snapshot
// ---------------------------------------------------------------------------

TEST(FastPublisher, EmptyBookSnapshot) {
    FastFeedPublisher pub;

    TopOfBook tob{};  // all zeros
    pub.publish_snapshot(tob);
    ASSERT_EQ(pub.packet_count(), 1u);

    const auto& pkt = pub.packets()[0];
    RecordingVisitor visitor;
    decode_message(pkt.bytes(), pkt.len, visitor);
    ASSERT_EQ(visitor.snapshots.size(), 1u);
    EXPECT_EQ(visitor.snapshots[0].bid_count, 0u);
    EXPECT_EQ(visitor.snapshots[0].ask_count, 0u);
}

}  // namespace
}  // namespace exchange::krx::fast
