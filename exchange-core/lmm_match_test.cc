#include "exchange-core/match_algo.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

static Order make_order(OrderId id, Quantity qty, bool mm = false) {
    Order o{};
    o.id                 = id;
    o.quantity           = qty;
    o.remaining_quantity = qty;
    o.filled_quantity    = 0;
    o.is_market_maker    = mm;
    o.prev               = nullptr;
    o.next               = nullptr;
    o.level              = nullptr;
    return o;
}

static void build_level(PriceLevel& level, Order** orders, size_t n) {
    level.head           = nullptr;
    level.tail           = nullptr;
    level.order_count    = 0;
    level.total_quantity = 0;

    for (size_t i = 0; i < n; ++i) {
        orders[i]->level = &level;
        orders[i]->prev  = (i == 0) ? nullptr : orders[i - 1];
        orders[i]->next  = nullptr;

        if (i == 0) {
            level.head = orders[i];
        } else {
            orders[i - 1]->next = orders[i];
        }
        level.tail = orders[i];
        level.total_quantity += orders[i]->remaining_quantity;
        ++level.order_count;
    }
}

// ---------------------------------------------------------------------------
// 1 MM + 2 regular orders, 40% MM priority
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, MmGetsPriorityAllocation) {
    PriceLevel level{};
    level.price = 1000000;

    // MM order: 100 qty, Regular1: 100 qty, Regular2: 100 qty
    Order mm  = make_order(1, 100, /*mm=*/true);
    Order r1  = make_order(2, 100);
    Order r2  = make_order(3, 100);
    Order* orders[] = {&r1, &mm, &r2};  // FIFO: r1 first, then mm, then r2
    build_level(level, orders, 3);

    Quantity remaining = 200;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<40>::match(level, remaining, results, count);

    // MM allocation = floor(200 * 40 / 100) = 80
    // Phase 1: MM gets 80 (has 100 resting, so 80 filled)
    // Phase 2: FIFO across all with remaining 120:
    //   r1 gets min(120, 100) = 100, remaining = 20
    //   mm gets min(20, 20) = 20, remaining = 0
    //   r2 gets nothing
    EXPECT_EQ(remaining, 0);

    // MM total fill: 80 (phase 1) + 20 (phase 2) = 100
    EXPECT_EQ(mm.filled_quantity, 100);
    EXPECT_EQ(mm.remaining_quantity, 0);

    // r1 total fill: 100 (phase 2)
    EXPECT_EQ(r1.filled_quantity, 100);
    EXPECT_EQ(r1.remaining_quantity, 0);

    // r2: no fill
    EXPECT_EQ(r2.filled_quantity, 0);
    EXPECT_EQ(r2.remaining_quantity, 100);
}

// ---------------------------------------------------------------------------
// MM resting < priority allocation: MM gets full resting, rest FIFO
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, MmRestingLessThanPriority) {
    PriceLevel level{};
    level.price = 1000000;

    // MM: 30 qty, Regular: 200 qty
    Order mm = make_order(1, 30, /*mm=*/true);
    Order r1 = make_order(2, 200);
    Order* orders[] = {&r1, &mm};
    build_level(level, orders, 2);

    Quantity remaining = 100;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<40>::match(level, remaining, results, count);

    // MM allocation = floor(100 * 40 / 100) = 40, capped at mm_resting=30
    // Phase 1: MM gets 30
    // Phase 2: remaining = 70, FIFO: r1 gets 70
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(mm.filled_quantity, 30);
    EXPECT_EQ(mm.remaining_quantity, 0);
    EXPECT_EQ(r1.filled_quantity, 70);
    EXPECT_EQ(r1.remaining_quantity, 130);
}

// ---------------------------------------------------------------------------
// No MM orders: pure FIFO behavior
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, NoMmOrdersPureFifo) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 50);
    Order o2 = make_order(2, 50);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 80;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<40>::match(level, remaining, results, count);

    // No MMs => mm_alloc capped at 0 => pure FIFO
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 50);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.filled_quantity, 30);
    EXPECT_EQ(o2.remaining_quantity, 20);
}

// ---------------------------------------------------------------------------
// All orders are MM: FIFO among MMs
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, AllOrdersMm) {
    PriceLevel level{};
    level.price = 1000000;

    Order mm1 = make_order(1, 60, true);
    Order mm2 = make_order(2, 40, true);
    Order* orders[] = {&mm1, &mm2};
    build_level(level, orders, 2);

    Quantity remaining = 80;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<50>::match(level, remaining, results, count);

    // MM allocation = floor(80 * 50 / 100) = 40, capped at 100 => 40
    // Phase 1: mm1 gets 40 (FIFO first)
    // Phase 2: remaining = 40:
    //   mm1 gets min(40, 20) = 20, remaining = 20
    //   mm2 gets min(20, 40) = 20, remaining = 0
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(mm1.filled_quantity, 60);
    EXPECT_EQ(mm1.remaining_quantity, 0);
    EXPECT_EQ(mm2.filled_quantity, 20);
    EXPECT_EQ(mm2.remaining_quantity, 20);
}

// ---------------------------------------------------------------------------
// MM priority 0%: pure FIFO
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, ZeroPriorityPureFifo) {
    PriceLevel level{};
    level.price = 1000000;

    Order mm = make_order(1, 50, true);
    Order r1 = make_order(2, 50);
    Order* orders[] = {&r1, &mm};
    build_level(level, orders, 2);

    Quantity remaining = 60;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<0>::match(level, remaining, results, count);

    // 0% priority => mm_alloc=0 => pure FIFO: r1 first (50), then mm (10)
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(r1.filled_quantity, 50);
    EXPECT_EQ(mm.filled_quantity, 10);
}

// ---------------------------------------------------------------------------
// MM priority 100%: MM gets everything available, remainder FIFO
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, FullPriorityMmFirst) {
    PriceLevel level{};
    level.price = 1000000;

    Order r1 = make_order(1, 50);
    Order mm = make_order(2, 30, true);
    Order r2 = make_order(3, 50);
    Order* orders[] = {&r1, &mm, &r2};
    build_level(level, orders, 3);

    Quantity remaining = 100;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<100>::match(level, remaining, results, count);

    // MM allocation = floor(100 * 100 / 100) = 100, capped at mm_resting=30
    // Phase 1: MM gets 30
    // Phase 2: remaining = 70, FIFO:
    //   r1 gets 50, remaining = 20
    //   mm: remaining_quantity=0, skip
    //   r2 gets 20
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(mm.filled_quantity, 30);
    EXPECT_EQ(mm.remaining_quantity, 0);
    EXPECT_EQ(r1.filled_quantity, 50);
    EXPECT_EQ(r1.remaining_quantity, 0);
    EXPECT_EQ(r2.filled_quantity, 20);
    EXPECT_EQ(r2.remaining_quantity, 30);
}

// ---------------------------------------------------------------------------
// Rounding: fractional lots rounded down, remainder via FIFO
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, RoundingFractionalLots) {
    PriceLevel level{};
    level.price = 1000000;

    Order mm = make_order(1, 100, true);
    Order r1 = make_order(2, 100);
    Order* orders[] = {&r1, &mm};
    build_level(level, orders, 2);

    // 40% of 33 = 13.2 => floor = 13
    Quantity remaining = 33;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<40>::match(level, remaining, results, count);

    // Phase 1: MM gets 13
    // Phase 2: remaining = 20, FIFO: r1 gets 20
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(mm.filled_quantity, 13);
    EXPECT_EQ(r1.filled_quantity, 20);
}

// ---------------------------------------------------------------------------
// Multiple MMs: FIFO among them in priority pass
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, MultipleMmsFifoOrder) {
    PriceLevel level{};
    level.price = 1000000;

    Order mm1 = make_order(1, 50, true);
    Order r1  = make_order(2, 100);
    Order mm2 = make_order(3, 50, true);
    Order* orders[] = {&mm1, &r1, &mm2};
    build_level(level, orders, 3);

    Quantity remaining = 100;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<40>::match(level, remaining, results, count);

    // mm_resting = 100, mm_alloc = floor(100*40/100) = 40
    // Phase 1: mm1 gets 40 (FIFO first MM), mm2 gets 0 (alloc exhausted)
    // Phase 2: remaining=60, FIFO:
    //   mm1: remaining_qty=10, gets 10, remaining=50
    //   r1: gets 50, remaining=0
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(mm1.filled_quantity, 50);
    EXPECT_EQ(mm1.remaining_quantity, 0);
    EXPECT_EQ(r1.filled_quantity, 50);
    EXPECT_EQ(r1.remaining_quantity, 50);
    EXPECT_EQ(mm2.filled_quantity, 0);
    EXPECT_EQ(mm2.remaining_quantity, 50);
}

// ---------------------------------------------------------------------------
// Aggressor smaller than level: only partial level consumed
// ---------------------------------------------------------------------------

TEST(FifoLmmMatchTest, AggressorSmallerThanLevel) {
    PriceLevel level{};
    level.price = 1000000;

    Order r1 = make_order(1, 200);
    Order mm = make_order(2, 200, true);
    Order* orders[] = {&r1, &mm};
    build_level(level, orders, 2);

    Quantity remaining = 50;
    FillResult results[16]{};
    size_t count = 0;

    FifoLmmMatch<40>::match(level, remaining, results, count);

    // mm_alloc = floor(50*40/100) = 20
    // Phase 1: MM gets 20
    // Phase 2: remaining=30, FIFO: r1 gets 30
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(mm.filled_quantity, 20);
    EXPECT_EQ(mm.remaining_quantity, 180);
    EXPECT_EQ(r1.filled_quantity, 30);
    EXPECT_EQ(r1.remaining_quantity, 170);
}

}  // namespace
}  // namespace exchange
