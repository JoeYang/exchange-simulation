#include "exchange-core/match_algo.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal Order suitable for matching tests.
// remaining_quantity is set equal to quantity; filled_quantity starts at 0.
static Order make_order(OrderId id, Quantity qty) {
    Order o{};
    o.id                = id;
    o.quantity          = qty;
    o.remaining_quantity = qty;
    o.filled_quantity   = 0;
    o.prev              = nullptr;
    o.next              = nullptr;
    o.level             = nullptr;
    return o;
}

// Link a sequence of orders into a doubly-linked list and attach them to
// a PriceLevel (head/tail only — total_quantity and order_count are set
// to the sum/count of the provided orders so tests can inspect them).
static void build_level(PriceLevel& level, Order** orders, size_t n) {
    level.head        = nullptr;
    level.tail        = nullptr;
    level.order_count = 0;
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
// Single resting order, aggressor fully fills it
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, SingleOrderFullFill) {
    PriceLevel level{};
    level.price = 1000000;  // 100.0000

    Order o1 = make_order(1, 10000);  // 1.0000 units
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 10000;
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].price, 1000000);
    EXPECT_EQ(results[0].quantity, 10000);
    EXPECT_EQ(results[0].resting_remaining, 0);

    EXPECT_EQ(o1.filled_quantity,    10000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Single resting order, aggressor partially fills it (aggressor runs out first)
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, SingleOrderPartialFillAggressorExhausted) {
    PriceLevel level{};
    level.price = 500000;  // 50.0000

    Order o1 = make_order(1, 50000);  // 5.0000 units
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 20000;  // aggressor only wants 2.0000
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 20000);
    EXPECT_EQ(results[0].resting_remaining, 30000);  // 5.0 - 2.0 = 3.0

    EXPECT_EQ(o1.filled_quantity,    20000);
    EXPECT_EQ(o1.remaining_quantity, 30000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Single resting order, resting order is partially filled (resting runs out
// but aggressor still has quantity) — aggressor has more than resting can give
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, SingleOrderPartialFillRestingExhausted) {
    PriceLevel level{};
    level.price = 200000;

    Order o1 = make_order(1, 10000);  // 1.0000 unit resting
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 30000;  // aggressor wants 3.0000 but only 1.0000 available
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].quantity, 10000);
    EXPECT_EQ(results[0].resting_remaining, 0);

    EXPECT_EQ(o1.filled_quantity,    10000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(remaining, 20000);  // 3.0 - 1.0 = 2.0 still unfilled
}

// ---------------------------------------------------------------------------
// Multiple orders — aggressor fills first completely then partially fills second
// (FIFO order is head → tail)
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, MultipleOrdersFifoOrder) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10000);  // oldest, fills first
    Order o2 = make_order(2, 20000);  // second
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    // aggressor fills o1 fully and takes 5000 from o2
    Quantity remaining = 15000;
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 2u);

    // First fill: o1 fully consumed
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 10000);
    EXPECT_EQ(results[0].resting_remaining, 0);

    // Second fill: o2 partially filled
    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].quantity, 5000);
    EXPECT_EQ(results[1].resting_remaining, 15000);

    EXPECT_EQ(o1.filled_quantity,    10000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.filled_quantity,    5000);
    EXPECT_EQ(o2.remaining_quantity, 15000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Aggressor sweeps all orders at the level and still has remaining quantity
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, LevelExhaustion) {
    PriceLevel level{};
    level.price = 750000;

    Order o1 = make_order(1, 10000);
    Order o2 = make_order(2, 10000);
    Order o3 = make_order(3, 10000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 50000;  // wants 5.0, only 3.0 available
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(results[i].quantity, 10000);
        EXPECT_EQ(results[i].resting_remaining, 0);
        EXPECT_EQ(results[i].price, 750000);
    }
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[2].resting_order, &o3);

    EXPECT_EQ(remaining, 20000);  // 5.0 - 3.0 = 2.0 left over
}

// ---------------------------------------------------------------------------
// Exact fill — aggressor quantity equals exactly one order's remaining
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, ExactFillRemainingBecomesZero) {
    PriceLevel level{};
    level.price = 300000;

    Order o1 = make_order(42, 25000);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 25000;
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].quantity, 25000);
    EXPECT_EQ(results[0].resting_remaining, 0);
    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o1.filled_quantity, 25000);
}

// ---------------------------------------------------------------------------
// Large aggressor sweeps many orders at level — confirms FIFO traversal order
// and correct quantity accounting across all fills
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, LargeAggressorSweepsAllOrders) {
    PriceLevel level{};
    level.price = 1000000;

    static const size_t N = 6;
    Order orders_storage[N];
    Order* ptrs[N];
    Quantity total_resting = 0;

    for (size_t i = 0; i < N; ++i) {
        orders_storage[i] = make_order(static_cast<OrderId>(i + 1),
                                       static_cast<Quantity>((i + 1) * 5000));
        ptrs[i] = &orders_storage[i];
        total_resting += orders_storage[i].remaining_quantity;
    }
    build_level(level, ptrs, N);

    // Aggressor wants far more than the level holds
    Quantity remaining = total_resting + 100000;
    Quantity original_remaining = remaining;
    FillResult results[16]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, N);
    Quantity filled_total = 0;
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(results[i].resting_order, ptrs[i]) << "FIFO order violated at index " << i;
        EXPECT_EQ(results[i].resting_remaining, 0);
        EXPECT_EQ(results[i].price, level.price);
        filled_total += results[i].quantity;
    }
    EXPECT_EQ(filled_total, total_resting);
    EXPECT_EQ(remaining, original_remaining - total_resting);
}

// ---------------------------------------------------------------------------
// Empty level — match is a no-op
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, EmptyLevelNoFills) {
    PriceLevel level{};
    level.price = 1000000;
    level.head = nullptr;
    level.tail = nullptr;
    level.order_count = 0;
    level.total_quantity = 0;

    Quantity remaining = 10000;
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(remaining, 10000);  // unchanged
}

// ---------------------------------------------------------------------------
// Zero remaining aggressor — match is a no-op even if level has orders
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, ZeroRemainingAggressorNoFills) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10000);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 0;
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(o1.filled_quantity, 0);
    EXPECT_EQ(o1.remaining_quantity, 10000);
}

// ---------------------------------------------------------------------------
// Fill result price field is always the level price (not order price)
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, FillResultPriceIsLevelPrice) {
    PriceLevel level{};
    level.price = 1234567;

    Order o1 = make_order(1, 10000);
    o1.price = 9999999;  // order's own price field — should NOT appear in fill
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 10000;
    FillResult results[8]{};
    size_t count = 0;

    FifoMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].price, 1234567);  // level price, not order price
}

// ---------------------------------------------------------------------------
// Accumulated count — multiple calls append to the same results array
// ---------------------------------------------------------------------------
TEST(FifoMatchTest, AccumulatedCountAcrossMultipleCalls) {
    PriceLevel level1{};
    level1.price = 1000000;
    Order o1 = make_order(1, 10000);
    Order* l1_orders[] = {&o1};
    build_level(level1, l1_orders, 1);

    PriceLevel level2{};
    level2.price = 900000;
    Order o2 = make_order(2, 10000);
    Order* l2_orders[] = {&o2};
    build_level(level2, l2_orders, 1);

    FillResult results[8]{};
    size_t count = 0;

    Quantity remaining1 = 10000;
    FifoMatch::match(level1, remaining1, results, count);
    EXPECT_EQ(count, 1u);

    Quantity remaining2 = 10000;
    FifoMatch::match(level2, remaining2, results, count);
    EXPECT_EQ(count, 2u);

    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].price, 1000000);
    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].price, 900000);
}

// ===========================================================================
// ProRataMatch tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Single order at level gets full fill
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, SingleOrderGetsFullFill) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10000);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 10000;
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].price, 1000000);
    EXPECT_EQ(results[0].quantity, 10000);
    EXPECT_EQ(results[0].resting_remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 10000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Three orders, proportional split with no remainder
// Orders: 60, 40. Aggressor: 50. Expected: floor(60*50/100)=30, floor(40*50/100)=20.
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, ProportionalAllocationNoRemainder) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 60000);  // 60% of level
    Order o2 = make_order(2, 40000);  // 40% of level
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);
    // total_quantity = 100000

    Quantity remaining = 50000;  // aggressor wants 50%
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    // floor(60000 * 50000 / 100000) = 30000
    // floor(40000 * 50000 / 100000) = 20000
    // total allocated = 50000, no remainder
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 30000);
    EXPECT_EQ(results[0].resting_remaining, 30000);  // 60000 - 30000

    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].quantity, 20000);
    EXPECT_EQ(results[1].resting_remaining, 20000);  // 40000 - 20000

    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o1.remaining_quantity, 30000);
    EXPECT_EQ(o2.filled_quantity, 20000);
    EXPECT_EQ(o2.remaining_quantity, 20000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Remainder distributed in FIFO order.
// Orders: 33, 33, 34 (total 100). Aggressor: 10.
// floor(33*10/100)=3, floor(33*10/100)=3, floor(34*10/100)=3. Total=9, remainder=1.
// Remainder goes to o1 (FIFO).
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, RemainderDistributedByFifo) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 33000);
    Order o2 = make_order(2, 33000);
    Order o3 = make_order(3, 34000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);
    // total_quantity = 100000

    Quantity remaining = 10000;
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    // floor(33000*10000/100000) = 3300
    // floor(33000*10000/100000) = 3300
    // floor(34000*10000/100000) = 3400
    // total = 10000, no remainder
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 3300);
    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].quantity, 3300);
    EXPECT_EQ(results[2].resting_order, &o3);
    EXPECT_EQ(results[2].quantity, 3400);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Remainder with actual rounding residual.
// Orders: 33, 33, 34 (total 100 units in raw qty). Aggressor: 7.
// floor(33*7/100)=2, floor(33*7/100)=2, floor(34*7/100)=2. Total=6, remainder=1.
// Remainder (1) goes to o1 first (FIFO). o1 gets 3, o2 gets 2, o3 gets 2.
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, RemainderGoesToFirstOrderFifo) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 330);
    Order o2 = make_order(2, 330);
    Order o3 = make_order(3, 340);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);
    // total_quantity = 1000

    Quantity remaining = 7;  // aggressor wants 7
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    // floor(330*7/1000) = floor(2.31) = 2
    // floor(330*7/1000) = 2
    // floor(340*7/1000) = floor(2.38) = 2
    // total = 6, remainder = 1
    // remainder goes to o1 (FIFO)
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 3);  // 2 + 1 remainder
    EXPECT_EQ(results[0].resting_remaining, 327);

    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].quantity, 2);
    EXPECT_EQ(results[1].resting_remaining, 328);

    EXPECT_EQ(results[2].resting_order, &o3);
    EXPECT_EQ(results[2].quantity, 2);
    EXPECT_EQ(results[2].resting_remaining, 338);

    EXPECT_EQ(o1.filled_quantity, 3);
    EXPECT_EQ(o1.remaining_quantity, 327);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Equal-sized orders degenerate to equal split + FIFO remainder.
// 4 orders of 25 each (total 100). Aggressor: 10.
// floor(25*10/100) = 2 each. Total allocated = 8. Remainder = 2.
// Remainder distributed: o1 gets +1, o2 gets +1.
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, EqualSizesEqualSplitFifoRemainder) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 25);
    Order o2 = make_order(2, 25);
    Order o3 = make_order(3, 25);
    Order o4 = make_order(4, 25);
    Order* orders[] = {&o1, &o2, &o3, &o4};
    build_level(level, orders, 4);
    // total_quantity = 100

    Quantity remaining = 10;
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    // floor(25*10/100) = 2 each → total 8, remainder 2
    // o1 gets +1, o2 gets +1 (FIFO)
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 3);  // 2 + 1 remainder
    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].quantity, 3);  // 2 + 1 remainder
    EXPECT_EQ(results[2].resting_order, &o3);
    EXPECT_EQ(results[2].quantity, 2);
    EXPECT_EQ(results[3].resting_order, &o4);
    EXPECT_EQ(results[3].quantity, 2);

    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Small aggressor: all orders get 0 base allocation, remainder goes FIFO.
// 3 orders of 100 each (total 300). Aggressor: 2.
// floor(100*2/300) = 0 for each. Total allocated = 0. Remainder = 2.
// o1 gets 1, o2 gets 1 (FIFO, zero-allocation orders are eligible for remainder).
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, SmallAggressorAllZeroBaseRemainderFifo) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100);
    Order o2 = make_order(2, 100);
    Order o3 = make_order(3, 100);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);
    // total_quantity = 300

    Quantity remaining = 2;
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    // floor(100*2/300) = 0 for all → no base fills
    // remainder = 2, distributed FIFO: o1 gets 1, o2 gets 1
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 1);
    EXPECT_EQ(results[0].resting_remaining, 99);

    EXPECT_EQ(results[1].resting_order, &o2);
    EXPECT_EQ(results[1].quantity, 1);
    EXPECT_EQ(results[1].resting_remaining, 99);

    EXPECT_EQ(o1.filled_quantity, 1);
    EXPECT_EQ(o1.remaining_quantity, 99);
    EXPECT_EQ(o2.filled_quantity, 1);
    EXPECT_EQ(o2.remaining_quantity, 99);
    EXPECT_EQ(o3.filled_quantity, 0);
    EXPECT_EQ(o3.remaining_quantity, 100);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Aggressor exactly equals level total — full sweep.
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, AggressorEqualsLevelTotal) {
    PriceLevel level{};
    level.price = 500000;

    Order o1 = make_order(1, 60000);
    Order o2 = make_order(2, 40000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);
    // total_quantity = 100000

    Quantity remaining = 100000;  // exactly the level total
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(results[0].quantity, 60000);
    EXPECT_EQ(results[0].resting_remaining, 0);
    EXPECT_EQ(results[1].quantity, 40000);
    EXPECT_EQ(results[1].resting_remaining, 0);

    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.remaining_quantity, 0);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Empty level — match is a no-op
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, EmptyLevelNoFills) {
    PriceLevel level{};
    level.price = 1000000;
    level.head = nullptr;
    level.tail = nullptr;
    level.order_count = 0;
    level.total_quantity = 0;

    Quantity remaining = 10000;
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(remaining, 10000);
}

// ---------------------------------------------------------------------------
// Zero remaining aggressor — match is a no-op even if level has orders
// ---------------------------------------------------------------------------
TEST(ProRataMatchTest, ZeroRemainingAggressorNoFills) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10000);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 0;
    FillResult results[16]{};
    size_t count = 0;

    ProRataMatch::match(level, remaining, results, count);

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(o1.filled_quantity, 0);
    EXPECT_EQ(o1.remaining_quantity, 10000);
}

}  // namespace
}  // namespace exchange
