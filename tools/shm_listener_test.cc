#include "tools/shm_listener.h"

#include "gtest/gtest.h"

namespace exchange {
namespace {

// Unique segment name per test to avoid collisions when tests run in parallel.
std::string SegName(const char* suffix) {
    return std::string("/exchange_shm_listener_test_") + suffix;
}

// ---------------------------------------------------------------------------
// SharedMemoryOrderListener — each event type flows through to consumer
// ---------------------------------------------------------------------------

TEST(ShmListenerTest, OrderAccepted) {
    const auto name = SegName("accepted");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderAccepted ev{1, 42, 1000};
    listener.on_order_accepted(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(received));
    EXPECT_EQ(std::get<OrderAccepted>(received), ev);
    EXPECT_FALSE(consumer.poll(received));
}

TEST(ShmListenerTest, OrderRejected) {
    const auto name = SegName("rejected");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderRejected ev{99, 2000, RejectReason::InvalidPrice};
    listener.on_order_rejected(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(received));
    EXPECT_EQ(std::get<OrderRejected>(received), ev);
}

TEST(ShmListenerTest, OrderFilled) {
    const auto name = SegName("filled");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderFilled ev{1, 2, 1005000, 10000, 3000};
    listener.on_order_filled(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderFilled>(received));
    EXPECT_EQ(std::get<OrderFilled>(received), ev);
}

TEST(ShmListenerTest, OrderPartiallyFilled) {
    const auto name = SegName("partfilled");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderPartiallyFilled ev{1, 2, 1005000, 5000, 5000, 5000, 4000};
    listener.on_order_partially_filled(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderPartiallyFilled>(received));
    EXPECT_EQ(std::get<OrderPartiallyFilled>(received), ev);
}

TEST(ShmListenerTest, OrderCancelled) {
    const auto name = SegName("cancelled");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderCancelled ev{5, 5000, CancelReason::UserRequested};
    listener.on_order_cancelled(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderCancelled>(received));
    EXPECT_EQ(std::get<OrderCancelled>(received), ev);
}

TEST(ShmListenerTest, OrderCancelRejected) {
    const auto name = SegName("cancelrej");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderCancelRejected ev{7, 77, 6000, RejectReason::UnknownOrder};
    listener.on_order_cancel_rejected(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderCancelRejected>(received));
    EXPECT_EQ(std::get<OrderCancelRejected>(received), ev);
}

TEST(ShmListenerTest, OrderModified) {
    const auto name = SegName("modified");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderModified ev{3, 33, 1010000, 8000, 7000};
    listener.on_order_modified(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderModified>(received));
    EXPECT_EQ(std::get<OrderModified>(received), ev);
}

TEST(ShmListenerTest, OrderModifyRejected) {
    const auto name = SegName("modifyrej");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    OrderModifyRejected ev{4, 44, 8000, RejectReason::UnknownOrder};
    listener.on_order_modify_rejected(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderModifyRejected>(received));
    EXPECT_EQ(std::get<OrderModifyRejected>(received), ev);
}

// ---------------------------------------------------------------------------
// SharedMemoryMdListener — each event type flows through to consumer
// ---------------------------------------------------------------------------

TEST(ShmListenerTest, TopOfBook) {
    const auto name = SegName("tob");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryMdListener listener(producer);

    TopOfBook ev{1004000, 20000, 1005000, 15000, 9000};
    listener.on_top_of_book(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<TopOfBook>(received));
    EXPECT_EQ(std::get<TopOfBook>(received), ev);
    EXPECT_FALSE(consumer.poll(received));
}

TEST(ShmListenerTest, DepthUpdate) {
    const auto name = SegName("depth");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryMdListener listener(producer);

    DepthUpdate ev{Side::Buy, 1004000, 30000, 3, DepthUpdate::Action::Add, 10000};
    listener.on_depth_update(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<DepthUpdate>(received));
    EXPECT_EQ(std::get<DepthUpdate>(received), ev);
}

TEST(ShmListenerTest, OrderBookAction) {
    const auto name = SegName("bookaction");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryMdListener listener(producer);

    OrderBookAction ev{10, Side::Sell, 1006000, 5000, OrderBookAction::Action::Add, 11000};
    listener.on_order_book_action(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<OrderBookAction>(received));
    EXPECT_EQ(std::get<OrderBookAction>(received), ev);
}

TEST(ShmListenerTest, Trade) {
    const auto name = SegName("trade");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryMdListener listener(producer);

    Trade ev{1005000, 10000, 1, 2, Side::Buy, 12000};
    listener.on_trade(ev);

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    ASSERT_TRUE(std::holds_alternative<Trade>(received));
    EXPECT_EQ(std::get<Trade>(received), ev);
}

// ---------------------------------------------------------------------------
// Mixed: order and md listener sharing one producer, events interleave in FIFO
// ---------------------------------------------------------------------------

TEST(ShmListenerTest, MixedListenersFifoOrder) {
    const auto name = SegName("mixed");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener order_listener(producer);
    SharedMemoryMdListener md_listener(producer);

    OrderAccepted accepted{1, 10, 100};
    TopOfBook tob{1004000, 20000, 1005000, 15000, 200};
    OrderFilled filled{1, 2, 1005000, 10000, 300};
    Trade trade{1005000, 10000, 1, 2, Side::Buy, 400};

    order_listener.on_order_accepted(accepted);
    md_listener.on_top_of_book(tob);
    order_listener.on_order_filled(filled);
    md_listener.on_trade(trade);

    RecordedEvent r1, r2, r3, r4;
    ASSERT_TRUE(consumer.poll(r1));
    ASSERT_TRUE(consumer.poll(r2));
    ASSERT_TRUE(consumer.poll(r3));
    ASSERT_TRUE(consumer.poll(r4));

    EXPECT_EQ(r1, RecordedEvent{accepted});
    EXPECT_EQ(r2, RecordedEvent{tob});
    EXPECT_EQ(r3, RecordedEvent{filled});
    EXPECT_EQ(r4, RecordedEvent{trade});

    RecordedEvent extra;
    EXPECT_FALSE(consumer.poll(extra));
}

// ---------------------------------------------------------------------------
// Full buffer: publish returns false; listener silently drops
// ---------------------------------------------------------------------------

TEST(ShmListenerTest, FullBufferDropsSilently) {
    const auto name = SegName("fullbuf");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    SharedMemoryOrderListener listener(producer);

    // Fill the ring buffer entirely.
    constexpr size_t kCapacity = 65536;
    OrderAccepted ev{1, 1, 0};
    size_t pushed = 0;
    while (producer.publish(RecordedEvent{ev})) {
        ++pushed;
    }
    ASSERT_EQ(pushed, kCapacity);

    // One more call via listener must not crash or throw even though buffer is full.
    EXPECT_NO_THROW(listener.on_order_accepted(ev));

    // Drain one slot and verify subsequent listener call succeeds.
    RecordedEvent out;
    ASSERT_TRUE(consumer.poll(out));
    EXPECT_NO_THROW(listener.on_order_accepted(ev));
}

}  // namespace
}  // namespace exchange
