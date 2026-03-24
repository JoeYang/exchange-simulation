#include "exchange-core/spsc_ring_buffer.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace exchange {
namespace {

TEST(SpscRingBufferTest, InitialState) {
    SpscRingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.full());
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBufferTest, PushAndPopSingleItem) {
    SpscRingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.try_push(42));
    EXPECT_EQ(rb.size(), 1u);

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(rb.empty());
}

TEST(SpscRingBufferTest, FillToCapacity) {
    SpscRingBuffer<int, 4> rb;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(rb.try_push(i));
    }
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.try_push(99));  // full — must return false
}

TEST(SpscRingBufferTest, PopFromEmptyReturnsFalse) {
    SpscRingBuffer<int, 4> rb;
    int val = -1;
    EXPECT_FALSE(rb.try_pop(val));
    EXPECT_EQ(val, -1);  // must remain unchanged
}

TEST(SpscRingBufferTest, FillAndDrainCompletely) {
    SpscRingBuffer<int, 8> rb;
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(rb.try_push(i));
    }
    EXPECT_TRUE(rb.full());
    for (int i = 0; i < 8; ++i) {
        int val = -1;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBufferTest, WrapAround) {
    SpscRingBuffer<int, 4> rb;
    // Fill and drain twice to force wrap-around of the internal indices
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_TRUE(rb.try_push(i + round * 10));
        }
        for (int i = 0; i < 4; ++i) {
            int val = -1;
            EXPECT_TRUE(rb.try_pop(val));
            EXPECT_EQ(val, i + round * 10);
        }
    }
    EXPECT_TRUE(rb.empty());
}

TEST(SpscRingBufferTest, FifoOrdering) {
    SpscRingBuffer<int, 8> rb;
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));

    int a = 0, b = 0, c = 0;
    EXPECT_TRUE(rb.try_pop(a));
    EXPECT_TRUE(rb.try_pop(b));
    EXPECT_TRUE(rb.try_pop(c));

    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
    EXPECT_EQ(c, 3);
}

TEST(SpscRingBufferTest, SizeTracking) {
    SpscRingBuffer<int, 8> rb;
    EXPECT_EQ(rb.size(), 0u);

    rb.try_push(1);
    EXPECT_EQ(rb.size(), 1u);

    rb.try_push(2);
    EXPECT_EQ(rb.size(), 2u);

    int val = 0;
    rb.try_pop(val);
    EXPECT_EQ(rb.size(), 1u);

    rb.try_pop(val);
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBufferTest, ConcurrentProducerConsumer) {
    SpscRingBuffer<int, 1024> rb;
    constexpr int kCount = 10000;

    std::thread producer([&]() {
        for (int i = 0; i < kCount; ++i) {
            while (!rb.try_push(i)) {
                // spin — wait for consumer to drain some slots
            }
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    std::thread consumer([&]() {
        int val = 0;
        for (int i = 0; i < kCount; ++i) {
            while (!rb.try_pop(val)) {
                // spin — wait for producer
            }
            received.push_back(val);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

// Compile-time check: power-of-2 capacity static_assert.
// Uncomment to verify that non-power-of-2 fails to compile:
// static_assert check is enforced via static_assert inside SpscRingBuffer.
// SpscRingBuffer<int, 3> invalid_rb;  // Should not compile.
TEST(SpscRingBufferTest, PowerOfTwoCapacityCompiles) {
    // These all have power-of-2 capacities — must compile fine
    SpscRingBuffer<int, 1> rb1;
    SpscRingBuffer<int, 2> rb2;
    SpscRingBuffer<int, 4> rb4;
    SpscRingBuffer<int, 8> rb8;
    SpscRingBuffer<int, 16> rb16;
    SpscRingBuffer<int, 1024> rb1024;
    (void)rb1; (void)rb2; (void)rb4; (void)rb8; (void)rb16; (void)rb1024;
}

}  // namespace
}  // namespace exchange
