#include "tools/shm_transport.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>
#include <string>

#include "gtest/gtest.h"

namespace exchange {
namespace {

// Unique segment name per test to avoid collisions when tests run in parallel.
// We use the TEST_NAME macro pattern via a helper.
std::string SegName(const char* suffix) {
    return std::string("/exchange_shm_test_") + suffix;
}

// Helper: returns true if the named shm segment is accessible.
bool SegmentExists(const std::string& name) {
    int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd == -1) return false;
    ::close(fd);
    return true;
}

// Build a simple RecordedEvent (OrderAccepted) for round-trip tests.
RecordedEvent MakeAccepted(OrderId id, uint64_t cl_id, Timestamp ts) {
    return OrderAccepted{id, cl_id, ts};
}

// ---------------------------------------------------------------------------
// Segment creation
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, ProducerCreatesSegment) {
    const auto name = SegName("create");
    {
        ShmProducer producer(name);
        EXPECT_TRUE(SegmentExists(name));
    }
    // Segment should be gone after producer destructs.
    EXPECT_FALSE(SegmentExists(name));
}

// ---------------------------------------------------------------------------
// Round-trip: single event
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, RoundTripSingleEvent) {
    const auto name = SegName("roundtrip");
    ShmProducer producer(name);
    ShmConsumer consumer(name);

    RecordedEvent sent = MakeAccepted(1, 42, 1000);
    ASSERT_TRUE(producer.publish(sent));

    RecordedEvent received;
    ASSERT_TRUE(consumer.poll(received));
    EXPECT_EQ(sent, received);

    // Buffer should now be empty.
    RecordedEvent extra;
    EXPECT_FALSE(consumer.poll(extra));
}

// ---------------------------------------------------------------------------
// FIFO ordering across multiple events
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, FifoOrdering) {
    const auto name = SegName("fifo");
    ShmProducer producer(name);
    ShmConsumer consumer(name);

    constexpr int kCount = 8;
    for (int i = 0; i < kCount; ++i) {
        RecordedEvent ev = MakeAccepted(static_cast<OrderId>(i + 1),
                                        static_cast<uint64_t>(i + 100),
                                        static_cast<Timestamp>(i * 1000));
        ASSERT_TRUE(producer.publish(ev)) << "push failed at i=" << i;
    }

    for (int i = 0; i < kCount; ++i) {
        RecordedEvent ev;
        ASSERT_TRUE(consumer.poll(ev)) << "pop failed at i=" << i;
        const auto& accepted = std::get<OrderAccepted>(ev);
        EXPECT_EQ(accepted.id, static_cast<OrderId>(i + 1));
        EXPECT_EQ(accepted.client_order_id, static_cast<uint64_t>(i + 100));
        EXPECT_EQ(accepted.ts, static_cast<Timestamp>(i * 1000));
    }

    // Empty after draining.
    RecordedEvent extra;
    EXPECT_FALSE(consumer.poll(extra));
}

// ---------------------------------------------------------------------------
// Multiple event types in order
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, MultipleEventTypesPreserveOrder) {
    const auto name = SegName("types");
    ShmProducer producer(name);
    ShmConsumer consumer(name);

    RecordedEvent ev1 = OrderAccepted{1, 10, 100};
    RecordedEvent ev2 = OrderFilled{1, 2, 1005000, 10000, 200};
    RecordedEvent ev3 = Trade{1005000, 10000, 1, 2, Side::Buy, 300};

    ASSERT_TRUE(producer.publish(ev1));
    ASSERT_TRUE(producer.publish(ev2));
    ASSERT_TRUE(producer.publish(ev3));

    RecordedEvent r1, r2, r3;
    ASSERT_TRUE(consumer.poll(r1));
    ASSERT_TRUE(consumer.poll(r2));
    ASSERT_TRUE(consumer.poll(r3));

    EXPECT_EQ(ev1, r1);
    EXPECT_EQ(ev2, r2);
    EXPECT_EQ(ev3, r3);
}

// ---------------------------------------------------------------------------
// Producer destroy removes segment; consumer open after that must fail
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, ProducerDestroyUnlinksSegment) {
    const auto name = SegName("unlink");
    {
        ShmProducer producer(name);
        EXPECT_TRUE(SegmentExists(name));
    }
    EXPECT_FALSE(SegmentExists(name));

    // Attempting to attach a consumer after the producer is gone must throw.
    EXPECT_THROW(ShmConsumer consumer2(name), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Consumer with no producer fails gracefully
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, ConsumerWithNoProducerThrows) {
    const auto name = SegName("noproducer");
    // Ensure the segment does not exist.
    ::shm_unlink(name.c_str());  // ignore errors

    EXPECT_THROW(
        { ShmConsumer consumer(name); },
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// Fill buffer — try_push returns false when full
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, FillBufferReturnsFalse) {
    const auto name = SegName("full");
    ShmProducer producer(name);
    ShmConsumer consumer(name);

    constexpr size_t kCapacity = 65536;
    RecordedEvent ev = MakeAccepted(1, 1, 0);

    size_t pushed = 0;
    while (producer.publish(ev)) {
        ++pushed;
    }
    EXPECT_EQ(pushed, kCapacity) << "expected exactly " << kCapacity << " pushes before full";

    // Ring is full — one more publish must fail.
    EXPECT_FALSE(producer.publish(ev));

    // Drain one slot and verify we can push again.
    RecordedEvent out;
    ASSERT_TRUE(consumer.poll(out));
    EXPECT_TRUE(producer.publish(ev));
}

// ---------------------------------------------------------------------------
// name() accessor
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, NameAccessor) {
    const auto name = SegName("name");
    ShmProducer producer(name);
    ShmConsumer consumer(name);
    EXPECT_EQ(producer.name(), name);
    EXPECT_EQ(consumer.name(), name);
}

// ---------------------------------------------------------------------------
// Interleaved publish/poll
// ---------------------------------------------------------------------------

TEST(ShmTransportTest, InterleavedPublishPoll) {
    const auto name = SegName("interleave");
    ShmProducer producer(name);
    ShmConsumer consumer(name);

    for (int i = 0; i < 100; ++i) {
        RecordedEvent sent = MakeAccepted(static_cast<OrderId>(i), 0, 0);
        ASSERT_TRUE(producer.publish(sent));
        RecordedEvent received;
        ASSERT_TRUE(consumer.poll(received));
        EXPECT_EQ(sent, received);
    }
}

}  // namespace
}  // namespace exchange
