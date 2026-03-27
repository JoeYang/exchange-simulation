#include "exchange-core/match_algo.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Order make_order(OrderId id, Quantity qty) {
    Order o{};
    o.id                 = id;
    o.quantity           = qty;
    o.remaining_quantity = qty;
    o.filled_quantity    = 0;
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
// 1. Remainder goes to largest order, not oldest (FIFO).
// ---------------------------------------------------------------------------

TEST(AllocationMatchTest, RemainderToLargestOrder) {
    // Orders: o1=30000 (oldest), o2=50000 (largest), o3=20000
    // Total = 100000. Aggressor wants 60000.
    // Pro-rata: floor(30000*60000/100000)=18000, floor(50000*60000/100000)=30000,
    //           floor(20000*60000/100000)=12000. Sum=60000, no remainder.
    // But let's create a scenario with remainder:
    // Orders: o1=30000, o2=50000, o3=20000. Total=100000. Aggressor=61000.
    // Pro-rata: floor(30000*61000/100000)=18300, floor(50000*61000/100000)=30500,
    //           floor(20000*61000/100000)=12200. Sum=61000. No remainder either.
    // Need rounding remainder. Try: o1=33333, o2=50000, o3=16667. Total=100000.
    // Aggressor=50000.
    // floor(33333*50000/100000)=16666, floor(50000*50000/100000)=25000,
    // floor(16667*50000/100000)=8333. Sum=49999. Remainder=1.
    // Largest order is o2 (50000). Remainder 1 goes to o2.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 33333);
    Order o2 = make_order(2, 50000);
    Order o3 = make_order(3, 16667);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 50000;
    FillResult results[16]{};
    size_t count = 0;

    AllocationMatch::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    // o2 is largest: gets 25000 + 1 remainder = 25001
    EXPECT_EQ(o1.filled_quantity, 16666);
    EXPECT_EQ(o2.filled_quantity, 25001);
    EXPECT_EQ(o3.filled_quantity, 8333);

    Quantity total = o1.filled_quantity + o2.filled_quantity + o3.filled_quantity;
    EXPECT_EQ(total, 50000);
}

// ---------------------------------------------------------------------------
// 2. Tie on size breaks to FIFO (oldest wins).
// ---------------------------------------------------------------------------

TEST(AllocationMatchTest, TieOnSizeBreaksToFifo) {
    // Two equal orders: o1=50000 (older), o2=50000. Total=100000.
    // Aggressor=99999. to_fill = min(99999,100000)=99999.
    // Pro-rata: floor(50000*99999/100000)=49999 each. Sum=99998. Remainder=1.
    // Tie on size: both have 50000. FIFO tiebreak: o1 is older -> gets remainder.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 50000);
    Order o2 = make_order(2, 50000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 99999;
    FillResult results[8]{};
    size_t count = 0;

    AllocationMatch::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 50000);  // 49999 + 1 remainder (FIFO tiebreak)
    EXPECT_EQ(o2.filled_quantity, 49999);
}

// ---------------------------------------------------------------------------
// 3. Single order gets everything.
// ---------------------------------------------------------------------------

TEST(AllocationMatchTest, SingleOrderGetsAll) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100000);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 50000;
    FillResult results[8]{};
    size_t count = 0;

    AllocationMatch::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(o1.filled_quantity, 50000);
}

// ---------------------------------------------------------------------------
// 4. Multiple remainder lots go to largest, then next largest.
// ---------------------------------------------------------------------------

TEST(AllocationMatchTest, MultipleRemainderLots) {
    // o1=10, o2=7, o3=3. Total=20. Aggressor=19. to_fill=19.
    // Pro-rata: floor(10*19/20)=9, floor(7*19/20)=6, floor(3*19/20)=2.
    // Sum=17. Remainder=2.
    // Largest remaining_qty: o1(10)>o2(7)>o3(3).
    // Remainder lot 1 -> o1. Remainder lot 2 -> o2.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10);
    Order o2 = make_order(2, 7);
    Order o3 = make_order(3, 3);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 19;
    FillResult results[16]{};
    size_t count = 0;

    AllocationMatch::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 10);  // 9 + 1 remainder
    EXPECT_EQ(o2.filled_quantity, 7);   // 6 + 1 remainder
    EXPECT_EQ(o3.filled_quantity, 2);   // 2, no remainder
}

}  // namespace
}  // namespace exchange
