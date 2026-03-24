#include "exchange-core/object_pool.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Simple test struct — does NOT depend on types.h.
// Large enough to hold a pointer on all target platforms (>= 8 bytes on 64-bit).
// ---------------------------------------------------------------------------
// Sized to guarantee sizeof(Slot) >= sizeof(void*) on 64-bit platforms.
struct Slot {
    int      id{0};
    float    value{0.0f};
    uint32_t extra{0};  // makes sizeof(Slot) == 12; the pool only needs >=8
};

static_assert(sizeof(Slot) >= sizeof(Slot*),
              "Slot must be large enough for the free-list pointer");

// ---------------------------------------------------------------------------
// Helper: fully allocate a pool and return all pointers.
// ---------------------------------------------------------------------------
template <std::size_t Cap>
static std::vector<Slot*> drain(exchange::ObjectPool<Slot, Cap>& pool) {
    std::vector<Slot*> ptrs;
    ptrs.reserve(Cap);
    for (std::size_t i = 0; i < Cap; ++i) {
        Slot* p = pool.allocate();
        EXPECT_NE(p, nullptr) << "Expected non-null at index " << i;
        ptrs.push_back(p);
    }
    return ptrs;
}

// ---------------------------------------------------------------------------
// 1. Allocate and deallocate a single object.
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, AllocateDeallocateSingle) {
    exchange::ObjectPool<Slot, 8> pool;

    EXPECT_EQ(pool.capacity(), 8u);
    EXPECT_EQ(pool.available(), 8u);

    Slot* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 7u);

    p->id = 42;
    p->value = 3.14f;
    EXPECT_EQ(p->id, 42);
    EXPECT_FLOAT_EQ(p->value, 3.14f);

    pool.deallocate(p);
    EXPECT_EQ(pool.available(), 8u);
}

// ---------------------------------------------------------------------------
// 2. Allocate to full capacity; next allocation returns nullptr.
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, ExhaustionReturnsNullptr) {
    exchange::ObjectPool<Slot, 4> pool;

    auto ptrs = drain(pool);
    EXPECT_EQ(pool.available(), 0u);

    Slot* overflow = pool.allocate();
    EXPECT_EQ(overflow, nullptr);

    // Still exhausted after a failed allocation attempt.
    EXPECT_EQ(pool.available(), 0u);
}

// ---------------------------------------------------------------------------
// 3. Deallocate one slot from a full pool, then reallocate — recycling works.
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, RecyclingAfterExhaustion) {
    exchange::ObjectPool<Slot, 4> pool;
    auto ptrs = drain(pool);

    // Pool is full — give back the second slot.
    pool.deallocate(ptrs[1]);
    EXPECT_EQ(pool.available(), 1u);

    Slot* recycled = pool.allocate();
    ASSERT_NE(recycled, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // The recycled pointer should be the one we just freed.
    EXPECT_EQ(recycled, ptrs[1]);

    // Write into it to confirm the memory is usable.
    recycled->id = 99;
    EXPECT_EQ(recycled->id, 99);
}

// ---------------------------------------------------------------------------
// 4. Reset returns all slots (available == capacity after reset).
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, ResetReturnsAllSlots) {
    exchange::ObjectPool<Slot, 16> pool;
    drain(pool);
    EXPECT_EQ(pool.available(), 0u);

    pool.reset();
    EXPECT_EQ(pool.available(), 16u);
    EXPECT_EQ(pool.capacity(), 16u);

    // Can allocate again after reset.
    Slot* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 15u);
}

// ---------------------------------------------------------------------------
// 5. available() tracks correctly through allocate/deallocate sequence.
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, AvailableCountTracksCorrectly) {
    exchange::ObjectPool<Slot, 8> pool;
    EXPECT_EQ(pool.available(), 8u);

    std::vector<Slot*> held;
    for (int i = 0; i < 5; ++i) {
        held.push_back(pool.allocate());
        EXPECT_EQ(pool.available(), static_cast<std::size_t>(8 - i - 1));
    }
    EXPECT_EQ(pool.available(), 3u);

    // Return two.
    pool.deallocate(held[0]);
    EXPECT_EQ(pool.available(), 4u);
    pool.deallocate(held[2]);
    EXPECT_EQ(pool.available(), 5u);

    // Allocate two more.
    pool.allocate();
    pool.allocate();
    EXPECT_EQ(pool.available(), 3u);
}

// ---------------------------------------------------------------------------
// 6. Pointers returned by allocate() all fall within storage (no OOB).
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, PointersAreWithinStorage) {
    constexpr std::size_t Cap = 10;
    exchange::ObjectPool<Slot, Cap> pool;

    auto ptrs = drain(pool);
    EXPECT_EQ(ptrs.size(), Cap);

    // All pointers must be distinct.
    std::unordered_set<Slot*> seen(ptrs.begin(), ptrs.end());
    EXPECT_EQ(seen.size(), Cap);
}

// ---------------------------------------------------------------------------
// 7. Multiple reset / reallocate cycles work correctly.
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, MultipleResetCycles) {
    exchange::ObjectPool<Slot, 4> pool;

    for (int cycle = 0; cycle < 3; ++cycle) {
        EXPECT_EQ(pool.available(), 4u) << "cycle " << cycle;
        drain(pool);
        EXPECT_EQ(pool.available(), 0u) << "cycle " << cycle;
        EXPECT_EQ(pool.allocate(), nullptr) << "cycle " << cycle;
        pool.reset();
    }
}

// ---------------------------------------------------------------------------
// 8. capacity() is always the compile-time Capacity.
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, CapacityIsConstant) {
    exchange::ObjectPool<Slot, 7> pool;
    EXPECT_EQ(pool.capacity(), 7u);
    pool.allocate();
    EXPECT_EQ(pool.capacity(), 7u);
    pool.reset();
    EXPECT_EQ(pool.capacity(), 7u);
}

// ---------------------------------------------------------------------------
// 9. Deallocate and reallocate in LIFO order (free list property).
// ---------------------------------------------------------------------------
TEST(ObjectPoolTest, LifoRecycleOrder) {
    exchange::ObjectPool<Slot, 4> pool;
    auto ptrs = drain(pool);

    // Return in order: ptrs[0], ptrs[1], ptrs[2], ptrs[3].
    // Because the free list is LIFO, we expect to get them back in reverse.
    pool.deallocate(ptrs[0]);
    pool.deallocate(ptrs[1]);
    pool.deallocate(ptrs[2]);
    pool.deallocate(ptrs[3]);

    EXPECT_EQ(pool.allocate(), ptrs[3]);
    EXPECT_EQ(pool.allocate(), ptrs[2]);
    EXPECT_EQ(pool.allocate(), ptrs[1]);
    EXPECT_EQ(pool.allocate(), ptrs[0]);
}
