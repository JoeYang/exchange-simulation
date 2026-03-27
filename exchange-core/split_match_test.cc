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
// 1. 40/60 split correctly divides aggressor quantity.
// ---------------------------------------------------------------------------

TEST(SplitFifoProRataTest, Split40_60) {
    // o1=60000 (oldest), o2=40000. Total=100000.
    // Aggressor wants 50000.
    // FIFO portion: 40% of 50000 = 20000.
    //   FIFO: o1 gets 20000 (oldest first).
    // ProRata portion: 60% of 50000 = 30000.
    //   Pro-rata over post-FIFO remaining quantities:
    //   o1 rem=40000, o2 rem=40000. level_remaining=80000.
    //   floor(40000*30000/80000)=15000 each. Sum=30000.
    //   o1 total: 20000+15000=35000. o2 total: 15000.

    using Algo = SplitFifoProRataMatch<40>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 60000);
    Order o2 = make_order(2, 40000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 50000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    Quantity total = o1.filled_quantity + o2.filled_quantity;
    EXPECT_EQ(total, 50000);

    // FIFO portion (20000): o1 gets all 20000.
    // ProRata portion (30000): each order has 40000/80000 = 50%, gets 15000.
    EXPECT_EQ(o1.filled_quantity, 35000);
    EXPECT_EQ(o2.filled_quantity, 15000);
}

// ---------------------------------------------------------------------------
// 2. FIFO portion fills in time order, ProRata proportional.
// ---------------------------------------------------------------------------

TEST(SplitFifoProRataTest, FifoPortionTimeOrder) {
    // Three orders: o1=20000, o2=30000, o3=50000. Total=100000.
    // Aggressor wants 50000. Split 50/50.
    // FIFO portion: 50% of 50000 = 25000.
    //   FIFO: o1 gets 20000 (fully filled), o2 gets 5000.
    // ProRata portion: 25000.
    //   floor(20000*25000/100000)=5000, floor(30000*25000/100000)=7500,
    //   floor(50000*25000/100000)=12500. Sum=25000.
    //   o1 total: 20000 + 5000 = 25000. But o1 only has 20000 remaining.
    //   The pro-rata allocation for o1 must be capped at what o1 has left
    //   after FIFO. o1 has 0 left after FIFO, so pro-rata for o1 = 0.
    //   Remaining pro-rata (5000) redistrib: handled by the implementation.
    //
    // Actually, let me think about this differently. The split algo should:
    //   1. Compute fifo_qty and prorata_qty from the aggressor amount.
    //   2. Run FIFO for fifo_qty on the level (modifying order state).
    //   3. Run ProRata for prorata_qty on the SAME level (with updated state).
    // This is the standard exchange interpretation.

    using Algo = SplitFifoProRataMatch<50>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 20000);
    Order o2 = make_order(2, 30000);
    Order o3 = make_order(3, 50000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 50000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    Quantity total = o1.filled_quantity + o2.filled_quantity + o3.filled_quantity;
    EXPECT_EQ(total, 50000);

    // FIFO: o1=20000 (full), o2=5000.
    // After FIFO: o1 rem=0, o2 rem=25000, o3 rem=50000. level total=75000.
    // ProRata on remaining 25000 over remaining orders:
    //   o1 has 0 left, skip.
    //   o2: floor(25000*25000/75000)=8333
    //   o3: floor(50000*25000/75000)=16666
    //   Sum=24999. Remainder=1 -> FIFO order -> o2 gets +1.
    // Final: o1=20000, o2=5000+8334=13334, o3=16666.
    EXPECT_EQ(o1.filled_quantity, 20000);
    EXPECT_EQ(o2.filled_quantity, 13334);
    EXPECT_EQ(o3.filled_quantity, 16666);
}

// ---------------------------------------------------------------------------
// 3. 0% FIFO = pure ProRata.
// ---------------------------------------------------------------------------

TEST(SplitFifoProRataTest, ZeroFifoIsPureProRata) {
    using Algo = SplitFifoProRataMatch<0>;

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
    // Pure pro-rata: floor(60000*50000/100000)=30000, floor(40000*50000/100000)=20000.
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o2.filled_quantity, 20000);
}

// ---------------------------------------------------------------------------
// 4. 100% FIFO = pure FIFO.
// ---------------------------------------------------------------------------

TEST(SplitFifoProRataTest, HundredPercentFifoIsPureFifo) {
    using Algo = SplitFifoProRataMatch<100>;

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 30000);
    Order o2 = make_order(2, 40000);
    Order o3 = make_order(3, 50000);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 50000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    // Pure FIFO: o1 gets 30000 (full), o2 gets 20000.
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.filled_quantity, 20000);
    EXPECT_EQ(o2.remaining_quantity, 20000);
    EXPECT_EQ(o3.filled_quantity, 0);
}

// ---------------------------------------------------------------------------
// 5. Aggressor exceeds level total.
// ---------------------------------------------------------------------------

TEST(SplitFifoProRataTest, AggressorExceedsLevel) {
    using Algo = SplitFifoProRataMatch<50>;

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

    // Fills entire level (50000), leaves 50000 remaining.
    EXPECT_EQ(remaining, 50000);
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.filled_quantity, 20000);
    EXPECT_EQ(o2.remaining_quantity, 0);
}

}  // namespace
}  // namespace exchange
