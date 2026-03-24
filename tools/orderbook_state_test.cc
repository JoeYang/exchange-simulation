#include "tools/orderbook_state.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helpers to build RecordedEvent instances
// ---------------------------------------------------------------------------

RecordedEvent make_depth_add(Side side, Price price, Quantity qty,
                             uint32_t count, Timestamp ts = 1000) {
    DepthUpdate e{};
    e.side        = side;
    e.price       = price;
    e.total_qty   = qty;
    e.order_count = count;
    e.action      = DepthUpdate::Add;
    e.ts          = ts;
    return e;
}

RecordedEvent make_depth_update(Side side, Price price, Quantity qty,
                                uint32_t count, Timestamp ts = 2000) {
    DepthUpdate e{};
    e.side        = side;
    e.price       = price;
    e.total_qty   = qty;
    e.order_count = count;
    e.action      = DepthUpdate::Update;
    e.ts          = ts;
    return e;
}

RecordedEvent make_depth_remove(Side side, Price price, Timestamp ts = 3000) {
    DepthUpdate e{};
    e.side        = side;
    e.price       = price;
    e.total_qty   = 0;
    e.order_count = 0;
    e.action      = DepthUpdate::Remove;
    e.ts          = ts;
    return e;
}

RecordedEvent make_trade(Price price, Quantity qty, OrderId agg, OrderId rest,
                         Timestamp ts = 4000) {
    Trade e{};
    e.price        = price;
    e.quantity     = qty;
    e.aggressor_id = agg;
    e.resting_id   = rest;
    e.aggressor_side = Side::Buy;
    e.ts           = ts;
    return e;
}

RecordedEvent make_order_accepted(OrderId id, uint64_t cl_id = 1,
                                  Timestamp ts = 100) {
    OrderAccepted e{};
    e.id              = id;
    e.client_order_id = cl_id;
    e.ts              = ts;
    return e;
}

RecordedEvent make_order_book_action(OrderId id, Side side, Price price,
                                     Quantity qty, OrderBookAction::Action action,
                                     Timestamp ts = 500) {
    OrderBookAction e{};
    e.id     = id;
    e.side   = side;
    e.price  = price;
    e.qty    = qty;
    e.action = action;
    e.ts     = ts;
    return e;
}

RecordedEvent make_order_cancelled(OrderId id, CancelReason reason,
                                   Timestamp ts = 600) {
    OrderCancelled e{};
    e.id     = id;
    e.reason = reason;
    e.ts     = ts;
    return e;
}

// ---------------------------------------------------------------------------
// DepthUpdate::Add tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, DepthAddBidAppearsInBids) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));

    ASSERT_EQ(state.bids().size(), 1u);
    auto it = state.bids().begin();
    EXPECT_EQ(it->second.price,       1005000);
    EXPECT_EQ(it->second.total_qty,   10000);
    EXPECT_EQ(it->second.order_count, 1u);
}

TEST(OrderbookStateTest, DepthAddAskAppearsInAsks) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Sell, 1010000, 20000, 2));

    ASSERT_EQ(state.asks().size(), 1u);
    auto it = state.asks().begin();
    EXPECT_EQ(it->second.price,       1010000);
    EXPECT_EQ(it->second.total_qty,   20000);
    EXPECT_EQ(it->second.order_count, 2u);
}

TEST(OrderbookStateTest, DepthAddDoesNotPolluteCrossedSide) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));

    EXPECT_TRUE(state.asks().empty());
    EXPECT_TRUE(state.bids().size() == 1u);
}

TEST(OrderbookStateTest, MultipleBidLevelsOrderedDescending) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1000000, 10000, 1));
    state.apply(make_depth_add(Side::Buy, 1005000, 20000, 2));
    state.apply(make_depth_add(Side::Buy,  995000, 30000, 3));

    ASSERT_EQ(state.bids().size(), 3u);
    auto it = state.bids().begin();
    EXPECT_EQ(it->second.price, 1005000);  // highest first
    ++it;
    EXPECT_EQ(it->second.price, 1000000);
    ++it;
    EXPECT_EQ(it->second.price, 995000);
}

TEST(OrderbookStateTest, MultipleAskLevelsOrderedAscending) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Sell, 1010000, 10000, 1));
    state.apply(make_depth_add(Side::Sell, 1005000, 20000, 2));
    state.apply(make_depth_add(Side::Sell, 1015000, 30000, 3));

    ASSERT_EQ(state.asks().size(), 3u);
    auto it = state.asks().begin();
    EXPECT_EQ(it->second.price, 1005000);  // lowest first
    ++it;
    EXPECT_EQ(it->second.price, 1010000);
    ++it;
    EXPECT_EQ(it->second.price, 1015000);
}

// ---------------------------------------------------------------------------
// DepthUpdate::Update tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, DepthUpdateChangesQtyAndCount) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));
    state.apply(make_depth_update(Side::Buy, 1005000, 25000, 3));

    ASSERT_EQ(state.bids().size(), 1u);
    const auto& lvl = state.bids().begin()->second;
    EXPECT_EQ(lvl.total_qty,   25000);
    EXPECT_EQ(lvl.order_count, 3u);
}

TEST(OrderbookStateTest, DepthUpdateWithoutPriorAddInsertsLevel) {
    // Update with no preceding Add is still valid — upsert semantics.
    OrderbookState state;
    state.apply(make_depth_update(Side::Sell, 1010000, 50000, 5));

    ASSERT_EQ(state.asks().size(), 1u);
    EXPECT_EQ(state.asks().begin()->second.total_qty, 50000);
}

// ---------------------------------------------------------------------------
// DepthUpdate::Remove tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, DepthRemoveBidErasesLevel) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));
    state.apply(make_depth_remove(Side::Buy, 1005000));

    EXPECT_TRUE(state.bids().empty());
}

TEST(OrderbookStateTest, DepthRemoveAskErasesLevel) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Sell, 1010000, 10000, 1));
    state.apply(make_depth_remove(Side::Sell, 1010000));

    EXPECT_TRUE(state.asks().empty());
}

TEST(OrderbookStateTest, DepthRemoveOnlyTargetedLevel) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));
    state.apply(make_depth_add(Side::Buy, 1000000, 20000, 2));
    state.apply(make_depth_remove(Side::Buy, 1005000));

    ASSERT_EQ(state.bids().size(), 1u);
    EXPECT_EQ(state.bids().begin()->second.price, 1000000);
}

TEST(OrderbookStateTest, DepthRemoveNonExistentLevelIsNoop) {
    OrderbookState state;
    // Remove a price that was never added — should not crash or leave garbage.
    EXPECT_NO_FATAL_FAILURE(
        state.apply(make_depth_remove(Side::Buy, 9999999)));
    EXPECT_TRUE(state.bids().empty());
}

// ---------------------------------------------------------------------------
// Trade tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, TradeAppearsInRecentTrades) {
    OrderbookState state;
    state.apply(make_trade(1005000, 10000, 2, 1));

    ASSERT_EQ(state.recent_trades().size(), 1u);
    const auto& t = state.recent_trades().front();
    EXPECT_EQ(t.price,        1005000);
    EXPECT_EQ(t.quantity,     10000);
    EXPECT_EQ(t.aggressor_id, 2u);
    EXPECT_EQ(t.resting_id,   1u);
    EXPECT_EQ(t.ts,           4000);
}

TEST(OrderbookStateTest, MultipleTradesOrdered) {
    OrderbookState state;
    state.apply(make_trade(1005000, 10000, 2, 1, 4000));
    state.apply(make_trade(1006000, 5000,  3, 1, 5000));

    ASSERT_EQ(state.recent_trades().size(), 2u);
    EXPECT_EQ(state.recent_trades()[0].price, 1005000);
    EXPECT_EQ(state.recent_trades()[1].price, 1006000);
}

TEST(OrderbookStateTest, TradesCapAtMaxTrades) {
    OrderbookState state;
    // kMaxTrades = 20; push 25 trades
    for (int i = 0; i < 25; ++i) {
        state.apply(make_trade(1000000 + i * 100, 10000,
                               static_cast<OrderId>(i + 2),
                               1,
                               static_cast<Timestamp>(1000 + i)));
    }

    ASSERT_EQ(state.recent_trades().size(), 20u);
    // Oldest 5 should have been evicted; first remaining trade is index 5.
    EXPECT_EQ(state.recent_trades().front().aggressor_id, 7u);
    EXPECT_EQ(state.recent_trades().back().aggressor_id,  26u);
}

// ---------------------------------------------------------------------------
// OrderBookAction / order event → event_log_ tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, OrderBookActionAppearsInEventLog) {
    OrderbookState state;
    state.apply(make_order_book_action(1, Side::Buy, 1005000, 10000,
                                       OrderBookAction::Add));

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderBookAction"),
              std::string::npos);
    EXPECT_EQ(state.event_log().front().ts, 500);
}

TEST(OrderbookStateTest, OrderAcceptedAppearsInEventLog) {
    OrderbookState state;
    state.apply(make_order_accepted(42, 1, 100));

    ASSERT_EQ(state.event_log().size(), 1u);
    const auto& entry = state.event_log().front();
    EXPECT_NE(entry.description.find("OrderAccepted"), std::string::npos);
    EXPECT_NE(entry.description.find("42"),            std::string::npos);
    EXPECT_EQ(entry.ts, 100);
}

TEST(OrderbookStateTest, OrderCancelledAppearsInEventLog) {
    OrderbookState state;
    state.apply(make_order_cancelled(7, CancelReason::UserRequested, 600));

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderCancelled"),
              std::string::npos);
}

TEST(OrderbookStateTest, EventLogCapsAtMaxEvents) {
    OrderbookState state;
    // kMaxEvents = 50; push 55 entries
    for (int i = 0; i < 55; ++i) {
        state.apply(make_order_accepted(static_cast<OrderId>(i + 1),
                                        static_cast<uint64_t>(i),
                                        static_cast<Timestamp>(i)));
    }

    EXPECT_EQ(state.event_log().size(), 50u);
    // Oldest 5 evicted; first remaining has id=6 (i=5 → id=6)
    EXPECT_NE(state.event_log().front().description.find("6"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// best_bid / best_ask tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, BestBidEmptyReturnsZero) {
    OrderbookState state;
    EXPECT_EQ(state.best_bid(), 0);
}

TEST(OrderbookStateTest, BestAskEmptyReturnsZero) {
    OrderbookState state;
    EXPECT_EQ(state.best_ask(), 0);
}

TEST(OrderbookStateTest, BestBidReturnHighestBid) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1000000, 10000, 1));
    state.apply(make_depth_add(Side::Buy, 1005000, 20000, 2));
    state.apply(make_depth_add(Side::Buy,  995000, 30000, 3));

    EXPECT_EQ(state.best_bid(), 1005000);
}

TEST(OrderbookStateTest, BestAskReturnsLowestAsk) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Sell, 1015000, 10000, 1));
    state.apply(make_depth_add(Side::Sell, 1010000, 20000, 2));
    state.apply(make_depth_add(Side::Sell, 1020000, 30000, 3));

    EXPECT_EQ(state.best_ask(), 1010000);
}

TEST(OrderbookStateTest, BestBidUpdatesAfterRemove) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));
    state.apply(make_depth_add(Side::Buy, 1000000, 20000, 2));
    state.apply(make_depth_remove(Side::Buy, 1005000));

    EXPECT_EQ(state.best_bid(), 1000000);
}

// ---------------------------------------------------------------------------
// Composite sequence: Add → partial fill (Update) → Remove
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, SequenceAddFillRemoveMatchesExpected) {
    OrderbookState state;

    // 1) Bid appears at 100.5000 with 10 lots
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1, 1000));

    ASSERT_EQ(state.bids().size(), 1u);
    EXPECT_EQ(state.best_bid(), 1005000);

    // 2) Partial fill: qty reduced to 5 lots
    state.apply(make_depth_update(Side::Buy, 1005000, 5000, 1, 2000));

    ASSERT_EQ(state.bids().size(), 1u);
    EXPECT_EQ(state.bids().begin()->second.total_qty, 5000);

    // 3) Trade fires
    state.apply(make_trade(1005000, 5000, 2, 1, 2000));

    ASSERT_EQ(state.recent_trades().size(), 1u);
    EXPECT_EQ(state.recent_trades().front().quantity, 5000);

    // 4) Level removed (fully consumed)
    state.apply(make_depth_remove(Side::Buy, 1005000, 3000));

    EXPECT_TRUE(state.bids().empty());
    EXPECT_EQ(state.best_bid(), 0);
    // Trade still present
    EXPECT_EQ(state.recent_trades().size(), 1u);
}

// ---------------------------------------------------------------------------
// Reset tests
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, ResetClearsAllState) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy,  1005000, 10000, 1));
    state.apply(make_depth_add(Side::Sell, 1010000, 10000, 1));
    state.apply(make_trade(1005000, 5000, 2, 1));
    state.apply(make_order_accepted(1, 1, 100));

    state.reset();

    EXPECT_TRUE(state.bids().empty());
    EXPECT_TRUE(state.asks().empty());
    EXPECT_TRUE(state.recent_trades().empty());
    EXPECT_TRUE(state.event_log().empty());
    EXPECT_EQ(state.best_bid(), 0);
    EXPECT_EQ(state.best_ask(), 0);
}

TEST(OrderbookStateTest, ResetThenReapplyWorks) {
    OrderbookState state;
    state.apply(make_depth_add(Side::Buy, 1005000, 10000, 1));
    state.reset();
    state.apply(make_depth_add(Side::Buy, 995000, 5000, 1));

    ASSERT_EQ(state.bids().size(), 1u);
    EXPECT_EQ(state.best_bid(), 995000);
}

// ---------------------------------------------------------------------------
// TopOfBook: informational only (goes to event_log_)
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, TopOfBookAppearsInEventLog) {
    OrderbookState state;
    TopOfBook tob{};
    tob.best_bid = 1005000;
    tob.bid_qty  = 10000;
    tob.best_ask = 1010000;
    tob.ask_qty  = 20000;
    tob.ts       = 9000;
    state.apply(tob);

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("TopOfBook"),
              std::string::npos);
    EXPECT_EQ(state.event_log().front().ts, 9000);
    // TopOfBook does NOT affect bids_/asks_
    EXPECT_TRUE(state.bids().empty());
    EXPECT_TRUE(state.asks().empty());
}

// ---------------------------------------------------------------------------
// OrderFilled / OrderPartiallyFilled / OrderModified / OrderRejected /
// OrderModifyRejected / OrderCancelRejected → event_log_
// ---------------------------------------------------------------------------

TEST(OrderbookStateTest, OrderFilledAppearsInEventLog) {
    OrderbookState state;
    OrderFilled e{};
    e.aggressor_id = 2;
    e.resting_id   = 1;
    e.price        = 1005000;
    e.quantity     = 10000;
    e.ts           = 200;
    state.apply(RecordedEvent{e});

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderFilled"),
              std::string::npos);
}

TEST(OrderbookStateTest, OrderPartiallyFilledAppearsInEventLog) {
    OrderbookState state;
    OrderPartiallyFilled e{};
    e.aggressor_id       = 3;
    e.resting_id         = 1;
    e.price              = 1005000;
    e.quantity           = 5000;
    e.aggressor_remaining = 5000;
    e.resting_remaining  = 0;
    e.ts                 = 300;
    state.apply(RecordedEvent{e});

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderPartiallyFilled"),
              std::string::npos);
}

TEST(OrderbookStateTest, OrderModifiedAppearsInEventLog) {
    OrderbookState state;
    OrderModified e{};
    e.id              = 5;
    e.client_order_id = 99;
    e.new_price       = 1006000;
    e.new_qty         = 15000;
    e.ts              = 400;
    state.apply(RecordedEvent{e});

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderModified"),
              std::string::npos);
}

TEST(OrderbookStateTest, OrderRejectedAppearsInEventLog) {
    OrderbookState state;
    OrderRejected e{};
    e.client_order_id = 77;
    e.reason          = RejectReason::InvalidPrice;
    e.ts              = 150;
    state.apply(RecordedEvent{e});

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderRejected"),
              std::string::npos);
    EXPECT_NE(state.event_log().front().description.find("InvalidPrice"),
              std::string::npos);
}

TEST(OrderbookStateTest, OrderModifyRejectedAppearsInEventLog) {
    OrderbookState state;
    OrderModifyRejected e{};
    e.id              = 9;
    e.client_order_id = 88;
    e.reason          = RejectReason::UnknownOrder;
    e.ts              = 250;
    state.apply(RecordedEvent{e});

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderModifyRejected"),
              std::string::npos);
}

TEST(OrderbookStateTest, OrderCancelRejectedAppearsInEventLog) {
    OrderbookState state;
    OrderCancelRejected e{};
    e.id              = 11;
    e.client_order_id = 55;
    e.reason          = RejectReason::UnknownOrder;
    e.ts              = 350;
    state.apply(RecordedEvent{e});

    ASSERT_EQ(state.event_log().size(), 1u);
    EXPECT_NE(state.event_log().front().description.find("OrderCancelRejected"),
              std::string::npos);
}

}  // namespace
}  // namespace exchange
