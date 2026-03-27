#include "exchange-core/match_algo.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Order make_order(OrderId id, Quantity qty,
                        bool is_mm = false, bool is_top = false) {
    Order o{};
    o.id                 = id;
    o.quantity           = qty;
    o.remaining_quantity = qty;
    o.filled_quantity    = 0;
    o.is_market_maker    = is_mm;
    o.is_top_order       = is_top;
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

// Use 40% top-order priority, 30% MM priority for all tests.
using Algo = FifoTopLmmMatch<40, 30>;

// ---------------------------------------------------------------------------
// 1. Level with top order, MM, and regular:
//    top gets priority, then MM, then FIFO remainder.
// ---------------------------------------------------------------------------

TEST(FifoTopLmmMatchTest, TopThenMmThenFifo) {
    // o1 = top order (oldest), 50000 qty
    // o2 = MM order, 40000 qty
    // o3 = regular order, 30000 qty
    // Total = 120000. Aggressor = 100000.
    //
    // Phase 1 (top): 40% of 100000 = 40000, capped at o1's 50000 -> 40000.
    //   o1 gets 40000. remaining = 60000.
    //
    // Phase 2 (MM): 30% of original 100000 = 30000, capped at o2's 40000 -> 30000.
    //   o2 gets 30000. remaining = 30000.
    //
    // Phase 3 (FIFO): 30000 remaining.
    //   o1 has 10000 left -> gets 10000. remaining = 20000.
    //   o2 has 10000 left -> gets 10000. remaining = 10000.
    //   o3 has 30000 left -> gets 10000. remaining = 0.
    //
    // Final: o1=50000, o2=40000, o3=10000. Total=100000.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 50000, false, true);   // top order
    Order o2 = make_order(2, 40000, true, false);    // MM
    Order o3 = make_order(3, 30000, false, false);   // regular
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 100000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 50000);  // 40000 top + 10000 FIFO
    EXPECT_EQ(o2.filled_quantity, 40000);  // 30000 MM + 10000 FIFO
    EXPECT_EQ(o3.filled_quantity, 10000);  // 10000 FIFO

    Quantity total = o1.filled_quantity + o2.filled_quantity + o3.filled_quantity;
    EXPECT_EQ(total, 100000);
}

// ---------------------------------------------------------------------------
// 2. No top order at level -- LMM + FIFO only.
// ---------------------------------------------------------------------------

TEST(FifoTopLmmMatchTest, NoTopOrderLmmPlusFifo) {
    // o1 = regular, 50000
    // o2 = MM, 30000
    // o3 = regular, 20000
    // Total = 100000. Aggressor = 60000.
    //
    // Phase 1 (top): no top order -> 0.
    // Phase 2 (MM): 30% of 60000 = 18000, capped at o2's 30000 -> 18000.
    //   o2 gets 18000. remaining = 42000.
    // Phase 3 (FIFO): 42000 remaining.
    //   o1 gets 42000 (has 50000). remaining = 0.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 50000, false, false);
    Order o2 = make_order(2, 30000, true, false);
    Order o3 = make_order(3, 20000, false, false);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 60000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 42000);
    EXPECT_EQ(o2.filled_quantity, 18000);
    EXPECT_EQ(o3.filled_quantity, 0);
}

// ---------------------------------------------------------------------------
// 3. Top order fully filled by its allocation --
//    remainder goes to LMM + FIFO.
// ---------------------------------------------------------------------------

TEST(FifoTopLmmMatchTest, TopFullyFilledRemainderToLmmFifo) {
    // o1 = top order, 10000 (small)
    // o2 = MM, 40000
    // o3 = regular, 50000
    // Total = 100000. Aggressor = 80000.
    //
    // Phase 1 (top): 40% of 80000 = 32000, capped at o1's 10000 -> 10000.
    //   o1 gets 10000 (fully filled). remaining = 70000.
    //
    // Phase 2 (MM): 30% of original 80000 = 24000, capped at o2's 40000 -> 24000.
    //   o2 gets 24000. remaining = 46000.
    //
    // Phase 3 (FIFO): 46000 remaining.
    //   o1 has 0 left -> skip.
    //   o2 has 16000 left -> gets 16000. remaining = 30000.
    //   o3 has 50000 left -> gets 30000. remaining = 0.
    //
    // Final: o1=10000, o2=40000, o3=30000.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 10000, false, true);
    Order o2 = make_order(2, 40000, true, false);
    Order o3 = make_order(3, 50000, false, false);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    Quantity remaining = 80000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 10000);
    EXPECT_EQ(o2.filled_quantity, 40000);
    EXPECT_EQ(o3.filled_quantity, 30000);
}

// ---------------------------------------------------------------------------
// 4. Top order AND MM are the same order -- gets both allocations.
// ---------------------------------------------------------------------------

TEST(FifoTopLmmMatchTest, TopAndMmSameOrder) {
    // o1 = both top order AND MM, 80000
    // o2 = regular, 40000
    // Total = 120000. Aggressor = 100000.
    //
    // Phase 1 (top): 40% of 100000 = 40000, capped at o1's 80000 -> 40000.
    //   o1 gets 40000. remaining = 60000.
    //
    // Phase 2 (MM): 30% of original 100000 = 30000, capped at o1's remaining 40000 -> 30000.
    //   o1 gets +30000 (total 70000). remaining = 30000.
    //
    // Phase 3 (FIFO): 30000 remaining.
    //   o1 has 10000 left -> gets 10000. remaining = 20000.
    //   o2 has 40000 left -> gets 20000. remaining = 0.
    //
    // Final: o1=80000, o2=20000.

    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 80000, true, true);   // both top + MM
    Order o2 = make_order(2, 40000, false, false);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 100000;
    FillResult results[16]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 80000);
    EXPECT_EQ(o2.filled_quantity, 20000);
}

// ---------------------------------------------------------------------------
// 5. No top, no MM -- pure FIFO.
// ---------------------------------------------------------------------------

TEST(FifoTopLmmMatchTest, NoTopNoMmPureFifo) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 30000);
    Order o2 = make_order(2, 40000);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    Quantity remaining = 50000;
    FillResult results[8]{};
    size_t count = 0;

    Algo::match(level, remaining, results, count);

    EXPECT_EQ(remaining, 0);
    EXPECT_EQ(o1.filled_quantity, 30000);
    EXPECT_EQ(o2.filled_quantity, 20000);
}

}  // namespace
}  // namespace exchange
