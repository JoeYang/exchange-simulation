#include "exchange-core/composite_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct CountingOrderListener : OrderListenerBase {
    int accepted_count{0};
    int rejected_count{0};
    int filled_count{0};
    int partially_filled_count{0};
    int cancelled_count{0};
    int cancel_rejected_count{0};
    int modified_count{0};
    int modify_rejected_count{0};

    void on_order_accepted(const OrderAccepted&) { ++accepted_count; }
    void on_order_rejected(const OrderRejected&) { ++rejected_count; }
    void on_order_filled(const OrderFilled&) { ++filled_count; }
    void on_order_partially_filled(const OrderPartiallyFilled&) { ++partially_filled_count; }
    void on_order_cancelled(const OrderCancelled&) { ++cancelled_count; }
    void on_order_cancel_rejected(const OrderCancelRejected&) { ++cancel_rejected_count; }
    void on_order_modified(const OrderModified&) { ++modified_count; }
    void on_order_modify_rejected(const OrderModifyRejected&) { ++modify_rejected_count; }
};

struct CountingMdListener : MarketDataListenerBase {
    int top_of_book_count{0};
    int depth_update_count{0};
    int order_book_action_count{0};
    int trade_count{0};

    void on_top_of_book(const TopOfBook&) { ++top_of_book_count; }
    void on_depth_update(const DepthUpdate&) { ++depth_update_count; }
    void on_order_book_action(const OrderBookAction&) { ++order_book_action_count; }
    void on_trade(const Trade&) { ++trade_count; }
};

// A listener that only overrides on_order_filled — used to test independent dispatch.
struct FillOnlyListener : OrderListenerBase {
    int filled_count{0};
    void on_order_filled(const OrderFilled&) { ++filled_count; }
};

// A market-data listener that only overrides on_trade.
struct TradeOnlyListener : MarketDataListenerBase {
    int trade_count{0};
    void on_trade(const Trade&) { ++trade_count; }
};

// ---------------------------------------------------------------------------
// CompositeOrderListener — single listener
// ---------------------------------------------------------------------------

TEST(CompositeOrderListenerTest, SingleListenerForwardsAllEvents) {
    CountingOrderListener a;
    CompositeOrderListener<CountingOrderListener> composite(&a);

    composite.on_order_accepted(OrderAccepted{});
    composite.on_order_rejected(OrderRejected{});
    composite.on_order_filled(OrderFilled{});
    composite.on_order_partially_filled(OrderPartiallyFilled{});
    composite.on_order_cancelled(OrderCancelled{});
    composite.on_order_cancel_rejected(OrderCancelRejected{});
    composite.on_order_modified(OrderModified{});
    composite.on_order_modify_rejected(OrderModifyRejected{});

    EXPECT_EQ(a.accepted_count, 1);
    EXPECT_EQ(a.rejected_count, 1);
    EXPECT_EQ(a.filled_count, 1);
    EXPECT_EQ(a.partially_filled_count, 1);
    EXPECT_EQ(a.cancelled_count, 1);
    EXPECT_EQ(a.cancel_rejected_count, 1);
    EXPECT_EQ(a.modified_count, 1);
    EXPECT_EQ(a.modify_rejected_count, 1);
}

// ---------------------------------------------------------------------------
// CompositeOrderListener — two listeners
// ---------------------------------------------------------------------------

TEST(CompositeOrderListenerTest, TwoListenersBothReceiveOrderFilled) {
    CountingOrderListener a, b;
    CompositeOrderListener<CountingOrderListener, CountingOrderListener> composite(&a, &b);

    composite.on_order_filled(OrderFilled{});

    EXPECT_EQ(a.filled_count, 1);
    EXPECT_EQ(b.filled_count, 1);
}

TEST(CompositeOrderListenerTest, TwoListenersBothReceiveAllEvents) {
    CountingOrderListener a, b;
    CompositeOrderListener<CountingOrderListener, CountingOrderListener> composite(&a, &b);

    composite.on_order_accepted(OrderAccepted{});
    composite.on_order_rejected(OrderRejected{});
    composite.on_order_filled(OrderFilled{});
    composite.on_order_partially_filled(OrderPartiallyFilled{});
    composite.on_order_cancelled(OrderCancelled{});
    composite.on_order_cancel_rejected(OrderCancelRejected{});
    composite.on_order_modified(OrderModified{});
    composite.on_order_modify_rejected(OrderModifyRejected{});

    EXPECT_EQ(a.accepted_count, 1);
    EXPECT_EQ(a.filled_count, 1);
    EXPECT_EQ(a.cancelled_count, 1);
    EXPECT_EQ(b.accepted_count, 1);
    EXPECT_EQ(b.filled_count, 1);
    EXPECT_EQ(b.cancelled_count, 1);
}

// ---------------------------------------------------------------------------
// CompositeOrderListener — three listeners
// ---------------------------------------------------------------------------

TEST(CompositeOrderListenerTest, ThreeListenersAllReceiveEvent) {
    CountingOrderListener a, b, c;
    CompositeOrderListener<CountingOrderListener, CountingOrderListener, CountingOrderListener>
        composite(&a, &b, &c);

    composite.on_order_filled(OrderFilled{});
    composite.on_order_filled(OrderFilled{});

    EXPECT_EQ(a.filled_count, 2);
    EXPECT_EQ(b.filled_count, 2);
    EXPECT_EQ(c.filled_count, 2);
}

// ---------------------------------------------------------------------------
// CompositeOrderListener — independent dispatch (mixed listener types)
// ---------------------------------------------------------------------------

TEST(CompositeOrderListenerTest, MixedListenersDispatchedIndependently) {
    CountingOrderListener full;
    FillOnlyListener fill_only;
    CompositeOrderListener<CountingOrderListener, FillOnlyListener> composite(&full, &fill_only);

    // on_order_filled fires both
    composite.on_order_filled(OrderFilled{});
    EXPECT_EQ(full.filled_count, 1);
    EXPECT_EQ(fill_only.filled_count, 1);

    // on_order_accepted only fires in full (FillOnlyListener inherits no-op)
    composite.on_order_accepted(OrderAccepted{});
    EXPECT_EQ(full.accepted_count, 1);

    // Counts are independent — fill_only.filled_count is not affected by accepted
    EXPECT_EQ(fill_only.filled_count, 1);
}

// ---------------------------------------------------------------------------
// CompositeOrderListener — empty composite (zero listeners)
// ---------------------------------------------------------------------------

TEST(CompositeOrderListenerTest, EmptyCompositeCompileAndNocrash) {
    CompositeOrderListener<> composite;

    // These must not crash
    composite.on_order_accepted(OrderAccepted{});
    composite.on_order_rejected(OrderRejected{});
    composite.on_order_filled(OrderFilled{});
    composite.on_order_partially_filled(OrderPartiallyFilled{});
    composite.on_order_cancelled(OrderCancelled{});
    composite.on_order_cancel_rejected(OrderCancelRejected{});
    composite.on_order_modified(OrderModified{});
    composite.on_order_modify_rejected(OrderModifyRejected{});
}

// ---------------------------------------------------------------------------
// CompositeOrderListener — call count correctness (multiple fires)
// ---------------------------------------------------------------------------

TEST(CompositeOrderListenerTest, CallCountMatchesFireCount) {
    CountingOrderListener a, b;
    CompositeOrderListener<CountingOrderListener, CountingOrderListener> composite(&a, &b);

    for (int i = 0; i < 5; ++i) composite.on_order_filled(OrderFilled{});

    EXPECT_EQ(a.filled_count, 5);
    EXPECT_EQ(b.filled_count, 5);
}

// ---------------------------------------------------------------------------
// CompositeMdListener — single listener
// ---------------------------------------------------------------------------

TEST(CompositeMdListenerTest, SingleListenerForwardsAllEvents) {
    CountingMdListener a;
    CompositeMdListener<CountingMdListener> composite(&a);

    composite.on_top_of_book(TopOfBook{});
    composite.on_depth_update(DepthUpdate{});
    composite.on_order_book_action(OrderBookAction{});
    composite.on_trade(Trade{});

    EXPECT_EQ(a.top_of_book_count, 1);
    EXPECT_EQ(a.depth_update_count, 1);
    EXPECT_EQ(a.order_book_action_count, 1);
    EXPECT_EQ(a.trade_count, 1);
}

// ---------------------------------------------------------------------------
// CompositeMdListener — two listeners
// ---------------------------------------------------------------------------

TEST(CompositeMdListenerTest, TwoListenersBothReceiveTrade) {
    CountingMdListener a, b;
    CompositeMdListener<CountingMdListener, CountingMdListener> composite(&a, &b);

    composite.on_trade(Trade{});

    EXPECT_EQ(a.trade_count, 1);
    EXPECT_EQ(b.trade_count, 1);
}

TEST(CompositeMdListenerTest, TwoListenersBothReceiveAllEvents) {
    CountingMdListener a, b;
    CompositeMdListener<CountingMdListener, CountingMdListener> composite(&a, &b);

    composite.on_top_of_book(TopOfBook{});
    composite.on_depth_update(DepthUpdate{});
    composite.on_order_book_action(OrderBookAction{});
    composite.on_trade(Trade{});

    EXPECT_EQ(a.top_of_book_count, 1);
    EXPECT_EQ(a.depth_update_count, 1);
    EXPECT_EQ(a.order_book_action_count, 1);
    EXPECT_EQ(a.trade_count, 1);
    EXPECT_EQ(b.top_of_book_count, 1);
    EXPECT_EQ(b.depth_update_count, 1);
    EXPECT_EQ(b.order_book_action_count, 1);
    EXPECT_EQ(b.trade_count, 1);
}

// ---------------------------------------------------------------------------
// CompositeMdListener — three listeners
// ---------------------------------------------------------------------------

TEST(CompositeMdListenerTest, ThreeListenersAllReceiveEvent) {
    CountingMdListener a, b, c;
    CompositeMdListener<CountingMdListener, CountingMdListener, CountingMdListener>
        composite(&a, &b, &c);

    composite.on_trade(Trade{});
    composite.on_trade(Trade{});
    composite.on_trade(Trade{});

    EXPECT_EQ(a.trade_count, 3);
    EXPECT_EQ(b.trade_count, 3);
    EXPECT_EQ(c.trade_count, 3);
}

// ---------------------------------------------------------------------------
// CompositeMdListener — independent dispatch (mixed listener types)
// ---------------------------------------------------------------------------

TEST(CompositeMdListenerTest, MixedListenersDispatchedIndependently) {
    CountingMdListener full;
    TradeOnlyListener trade_only;
    CompositeMdListener<CountingMdListener, TradeOnlyListener> composite(&full, &trade_only);

    composite.on_trade(Trade{});
    EXPECT_EQ(full.trade_count, 1);
    EXPECT_EQ(trade_only.trade_count, 1);

    // on_top_of_book only fires in full (TradeOnlyListener inherits no-op)
    composite.on_top_of_book(TopOfBook{});
    EXPECT_EQ(full.top_of_book_count, 1);
    // trade_only.trade_count unchanged
    EXPECT_EQ(trade_only.trade_count, 1);
}

// ---------------------------------------------------------------------------
// CompositeMdListener — empty composite (zero listeners)
// ---------------------------------------------------------------------------

TEST(CompositeMdListenerTest, EmptyCompositeCompileAndNocrash) {
    CompositeMdListener<> composite;

    // These must not crash
    composite.on_top_of_book(TopOfBook{});
    composite.on_depth_update(DepthUpdate{});
    composite.on_order_book_action(OrderBookAction{});
    composite.on_trade(Trade{});
}

// ---------------------------------------------------------------------------
// CompositeMdListener — call count correctness
// ---------------------------------------------------------------------------

TEST(CompositeMdListenerTest, CallCountMatchesFireCount) {
    CountingMdListener a, b;
    CompositeMdListener<CountingMdListener, CountingMdListener> composite(&a, &b);

    for (int i = 0; i < 7; ++i) composite.on_trade(Trade{});

    EXPECT_EQ(a.trade_count, 7);
    EXPECT_EQ(b.trade_count, 7);
}

}  // namespace
}  // namespace exchange
