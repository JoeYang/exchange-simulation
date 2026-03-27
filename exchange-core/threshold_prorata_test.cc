#include "exchange-core/match_algo.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helpers (same pattern as match_algo_test.cc)
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

// Alias for the threshold pro-rata match with a specific threshold.
// ThresholdProRataMatch is templated on MinThreshold (minimum allocation
// in quantity units; orders receiving less than this from the proportional
// pass get zero, and their share is redistributed via FIFO).

// ---------------------------------------------------------------------------
// 1. Three orders, threshold=20000: orders below threshold get 0,
//    remainder distributed via FIFO.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, OrdersBelowThresholdGetZeroRemainderViaFifo) {
    // Three orders: 100000, 30000, 20000 = total 150000
    // Aggressor wants 60000.
    // Pro-rata base: floor(100000*60000/150000) = 40000
    //                floor( 30000*60000/150000) = 12000
    //                floor( 20000*60000/150000) =  8000
    // With threshold=20000: order2 gets 12000 < 20000 -> zeroed
    //                        order3 gets  8000 < 20000 -> zeroed
    //                        order1 gets 40000 >= 20000 -> kept
    // Redistribution pool = 60000 - 40000 = 20000, via FIFO.
    // FIFO: order1 gets min(20000, remaining_qty=60000) = 20000.
    // Total: order1=60000, order2=0, order3=0.

    using Algo = ThresholdProRataMatch<20000>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100000);
    Order o2 = make_order(2, 30000);
    Order o3 = make_order(3, 20000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 60000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);

    // Verify total filled = 60000.
    Quantity total_filled = 0;
    for (size_t i = 0; i < count; ++i) {
        total_filled += results[i].quantity;
    }
    EXPECT_EQ(total_filled, 60000);

    // Order 1 should get all 60000 (40000 pro-rata + 20000 FIFO remainder).
    EXPECT_EQ(o1.filled_quantity, 60000);
    EXPECT_EQ(o1.remaining_quantity, 40000);
    // Orders 2 and 3 get nothing.
    EXPECT_EQ(o2.filled_quantity, 0);
    EXPECT_EQ(o3.filled_quantity, 0);
}

// ---------------------------------------------------------------------------
// 2. All orders above threshold -- standard pro-rata behavior.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, AllAboveThresholdStandardProRata) {
    // Three orders: 100000, 80000, 60000 = total 240000
    // Aggressor wants 120000. Threshold=10000.
    // Pro-rata: floor(100000*120000/240000) = 50000  >= 10000 OK
    //           floor( 80000*120000/240000) = 40000  >= 10000 OK
    //           floor( 60000*120000/240000) = 30000  >= 10000 OK
    // Sum = 120000, no remainder. All via pro-rata.

    using Algo = ThresholdProRataMatch<10000>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100000);
    Order o2 = make_order(2, 80000);
    Order o3 = make_order(3, 60000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 120000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 50000);
    EXPECT_EQ(o2.filled_quantity, 40000);
    EXPECT_EQ(o3.filled_quantity, 30000);
}

// ---------------------------------------------------------------------------
// 3. All orders below threshold -- all via FIFO.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, AllBelowThresholdAllViaFifo) {
    // Three equal orders: 10000 each = total 30000.
    // Aggressor wants 20000. Threshold=20000.
    // Pro-rata: floor(10000*20000/30000) = 6666 each.
    // All < 20000 threshold -> all zeroed.
    // Entire 20000 via FIFO: order1 gets 10000, order2 gets 10000.

    using Algo = ThresholdProRataMatch<20000>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10000);
    Order o2 = make_order(2, 10000);
    Order o3 = make_order(3, 10000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 20000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    // FIFO: o1 fully filled (10000), o2 gets remaining 10000.
    EXPECT_EQ(o1.filled_quantity, 10000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.filled_quantity, 10000);
    EXPECT_EQ(o2.remaining_quantity, 0);
    EXPECT_EQ(o3.filled_quantity, 0);
}

// ---------------------------------------------------------------------------
// 4. Single order at level -- gets everything regardless of threshold.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, SingleOrderGetsEverything) {
    using Algo = ThresholdProRataMatch<50000>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100000);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    Quantity remaining = 30000;
    FillResult results[8]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].quantity, 30000);
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o1.remaining_quantity, 70000);
}

// ---------------------------------------------------------------------------
// 5. Exact threshold boundary: order gets exactly threshold amount.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, ExactThresholdBoundary) {
    // Two orders: 50000, 50000 = total 100000.
    // Aggressor wants 60000. Threshold=30000.
    // Pro-rata: floor(50000*60000/100000) = 30000 each.
    // Both exactly == threshold -> both kept.
    // Sum = 60000, no remainder.

    using Algo = ThresholdProRataMatch<30000>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 50000);
    Order o2 = make_order(2, 50000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 60000;
    FillResult results[8]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o2.filled_quantity, 30000);
}

// ---------------------------------------------------------------------------
// 6. Threshold=0 behaves like standard ProRata.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, ZeroThresholdIsStandardProRata) {
    using Algo = ThresholdProRataMatch<0>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 60000);
    Order o2 = make_order(2, 40000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 50000;
    FillResult results[8]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    // floor(60000*50000/100000) = 30000
    // floor(40000*50000/100000) = 20000
    // sum = 50000, no remainder
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o2.filled_quantity, 20000);
}

// ---------------------------------------------------------------------------
// 7. Aggressor wants more than level total -- fills entire level.
// ---------------------------------------------------------------------------

TEST(ThresholdProRataTest, AggressorExceedsLevel) {
    using Algo = ThresholdProRataMatch<10000>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 30000);
    Order o2 = make_order(2, 20000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 100000;
    FillResult results[8]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    // Should fill entire level (50000) and leave 50000 remaining.
    EXPECT_EQ(remaining, 50000);
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.filled_quantity, 20000);
    EXPECT_EQ(o2.remaining_quantity, 0);
}

}  // namespace
}  // namespace exchange
