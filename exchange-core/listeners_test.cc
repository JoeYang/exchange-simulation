#include "exchange-core/listeners.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Verify base classes can be instantiated and all no-op methods compile and
// can be called without side effects.

TEST(ListenersTest, OrderListenerBaseNoOps) {
    OrderListenerBase listener;
    listener.on_order_accepted(OrderAccepted{});
    listener.on_order_rejected(OrderRejected{});
    listener.on_order_filled(OrderFilled{});
    listener.on_order_partially_filled(OrderPartiallyFilled{});
    listener.on_order_cancelled(OrderCancelled{});
    listener.on_order_cancel_rejected(OrderCancelRejected{});
    listener.on_order_modified(OrderModified{});
    listener.on_order_modify_rejected(OrderModifyRejected{});
    // All calls are no-ops; reaching here without crash or compile error is the assertion.
}

TEST(ListenersTest, MarketDataListenerBaseNoOps) {
    MarketDataListenerBase listener;
    listener.on_top_of_book(TopOfBook{});
    listener.on_depth_update(DepthUpdate{});
    listener.on_order_book_action(OrderBookAction{});
    listener.on_trade(Trade{});
}

// Verify a derived class can override individual methods by name hiding.
// Non-overridden methods on the derived type must still forward to the
// base no-op (i.e., they must compile and not crash).

class TestOrderListener : public OrderListenerBase {
public:
    int accepted_count{0};
    void on_order_accepted(const OrderAccepted&) { ++accepted_count; }
};

TEST(ListenersTest, DerivedOrderListenerOverrideByNameHiding) {
    TestOrderListener listener;

    listener.on_order_accepted(OrderAccepted{.id = 1, .client_order_id = 1, .ts = 100});
    EXPECT_EQ(listener.accepted_count, 1);

    listener.on_order_accepted(OrderAccepted{.id = 2, .client_order_id = 2, .ts = 200});
    EXPECT_EQ(listener.accepted_count, 2);

    // Non-overridden method falls through to the base no-op.
    listener.on_order_rejected(OrderRejected{});
    listener.on_order_filled(OrderFilled{});
    listener.on_order_partially_filled(OrderPartiallyFilled{});
    listener.on_order_cancelled(OrderCancelled{});
    listener.on_order_cancel_rejected(OrderCancelRejected{});
    listener.on_order_modified(OrderModified{});
    listener.on_order_modify_rejected(OrderModifyRejected{});

    // Only on_order_accepted increments the counter.
    EXPECT_EQ(listener.accepted_count, 2);
}

class TestMdListener : public MarketDataListenerBase {
public:
    int trade_count{0};
    void on_trade(const Trade&) { ++trade_count; }
};

TEST(ListenersTest, DerivedMdListenerOverrideByNameHiding) {
    TestMdListener listener;

    listener.on_trade(Trade{.price = 1005000, .quantity = 10000,
                            .aggressor_id = 1, .resting_id = 2,
                            .aggressor_side = Side::Buy, .ts = 500});
    EXPECT_EQ(listener.trade_count, 1);

    // Non-overridden methods fall through to base no-ops.
    listener.on_top_of_book(TopOfBook{});
    listener.on_depth_update(DepthUpdate{});
    listener.on_order_book_action(OrderBookAction{});

    EXPECT_EQ(listener.trade_count, 1);
}

// Verify the base type can be used via pointer-to-base (even though the
// engine template dispatches directly, this confirms the inheritance chain
// is well-formed).
TEST(ListenersTest, OrderListenerBaseUsableViaBasePointer) {
    TestOrderListener concrete;
    OrderListenerBase* base_ptr = &concrete;

    // Called through base pointer: resolves to OrderListenerBase::on_order_accepted
    // (name hiding, not virtual -- this is intentional; confirms no virtual overhead).
    base_ptr->on_order_accepted(OrderAccepted{.id = 3, .client_order_id = 3, .ts = 300});
    // count is NOT incremented because name hiding does not dispatch virtually.
    EXPECT_EQ(concrete.accepted_count, 0);
}

TEST(ListenersTest, MarketDataListenerBaseUsableViaBasePointer) {
    TestMdListener concrete;
    MarketDataListenerBase* base_ptr = &concrete;

    base_ptr->on_trade(Trade{});
    // count is NOT incremented: name hiding, not virtual dispatch.
    EXPECT_EQ(concrete.trade_count, 0);
}

}  // namespace
}  // namespace exchange
