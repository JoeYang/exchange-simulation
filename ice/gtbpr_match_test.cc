#include "ice/gtbpr_match.h"

#include <gtest/gtest.h>

namespace exchange {
namespace ice {
namespace {

// ---------------------------------------------------------------------------
// Helpers — mirrors exchange-core/match_algo_test.cc conventions
// ---------------------------------------------------------------------------

static Order make_order(OrderId id, Quantity qty, Timestamp ts = 0) {
    Order o{};
    o.id                 = id;
    o.quantity           = qty;
    o.remaining_quantity = qty;
    o.filled_quantity    = 0;
    o.timestamp          = ts;
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

// Nanoseconds-per-second for readable timestamp construction.
constexpr Timestamp SEC = 1'000'000'000LL;

// Default config used by most tests.
static GtbprMatch::Config default_cfg() {
    return {.collar = 50000, .cap = 200000, .time_weight_factor = 0.1};
}

// ---------------------------------------------------------------------------
// Priority order gets filled first (up to cap)
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, PriorityOrderFilledFirst) {
    PriceLevel level{};
    level.price = 1000000;

    // o1: large order qualifying for priority (oldest).
    // o2: smaller order, not qualifying.
    Order o1 = make_order(1, 100000, 0);       // 10 lots, qualifies (>= collar 50000)
    Order o2 = make_order(2, 100000, 1 * SEC);  // 10 lots, also qualifies but not first
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    auto cfg = default_cfg();
    Quantity remaining = 200000;  // aggressor wants all
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 2 * SEC, cfg);

    // o1 should have received priority fill (up to cap=200000, but only 100000 avail)
    // then pro-rata distributes remaining 100000 to o2 (o1 has 0 left).
    ASSERT_GE(count, 1u);
    EXPECT_EQ(results[0].resting_order, &o1);
    // o1 gets priority fill of min(cap=200000, pool=200000, remaining_qty=100000) = 100000
    // Then pro-rata on remaining 100000: only o2 has effective qty.
    // o2 gets 100000.
    Quantity o1_total = 0, o2_total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].resting_order == &o1) o1_total += results[i].quantity;
        if (results[i].resting_order == &o2) o2_total += results[i].quantity;
    }
    EXPECT_EQ(o1_total, 100000);
    EXPECT_EQ(o2_total, 100000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Priority fill is capped at Config::cap
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, PriorityFillCappedAtCap) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 500000, 0);        // 50 lots, well above collar
    Order o2 = make_order(2, 500000, 1 * SEC);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    auto cfg = default_cfg();  // cap = 200000
    Quantity remaining = 1000000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 2 * SEC, cfg);

    // o1 priority fill = min(cap=200000, pool=1000000, remaining=500000) = 200000
    // Pro-rata pool = 1000000 - 200000 = 800000
    // o1 effective = 500000-200000 = 300000, o2 effective = 500000
    // Total fills should sum to 1000000 (level total).
    Quantity total = 0;
    Quantity o1_total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += results[i].quantity;
        if (results[i].resting_order == &o1) o1_total += results[i].quantity;
    }
    EXPECT_EQ(total, 1000000);
    EXPECT_EQ(remaining, 0);

    // o1 must have gotten at least the cap as priority.
    EXPECT_GE(o1_total, 200000);
}

// ---------------------------------------------------------------------------
// Order below collar size doesn't get priority
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, BelowCollarNoPriority) {
    PriceLevel level{};
    level.price = 1000000;

    // Both orders below collar (50000).
    Order o1 = make_order(1, 30000, 0);
    Order o2 = make_order(2, 30000, 1 * SEC);
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    auto cfg = default_cfg();
    Quantity remaining = 60000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 2 * SEC, cfg);

    // No priority order. Pure pro-rata with time weighting.
    // Both get filled (total = 60000).
    Quantity total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += results[i].quantity;
    }
    EXPECT_EQ(total, 60000);
    EXPECT_EQ(remaining, 0);

    // o1 is older (age=2s, tw=1.2) vs o2 (age=1s, tw=1.1).
    // Both same qty so weights are proportional to tw.
    // o1 share = 30000*1.2 / (30000*1.2 + 30000*1.1) = 1.2/2.3 ~= 0.5217
    // o1 alloc = floor(0.5217 * 60000) = floor(31304.3) = 31304? No — wait,
    // to_fill = min(60000, 60000) = 60000, and with equal sizes and the entire
    // level being consumed, each order just gets their full remaining.
    // Actually: to_fill = min(remaining=60000, total=60000) = 60000.
    // Each order has 30000. Pro-rata weights don't matter when we consume all.
    EXPECT_EQ(o1.remaining_quantity, 0);
    EXPECT_EQ(o2.remaining_quantity, 0);
}

// ---------------------------------------------------------------------------
// Pro-rata allocation proportional to quantity (equal age)
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, ProRataProportionalToQuantity) {
    PriceLevel level{};
    level.price = 1000000;

    // Same timestamp, different sizes. No priority (below collar).
    Order o1 = make_order(1, 30000, 0);  // 30k
    Order o2 = make_order(2, 10000, 0);  // 10k
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    auto cfg = default_cfg();
    Quantity remaining = 20000;  // partial fill
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 0, cfg);

    // Equal age → time_weight = 1.0 for both. Weights purely by quantity.
    // o1 weight = 30000, o2 weight = 10000, total = 40000.
    // o1 alloc = floor(30000/40000 * 20000) = floor(15000) = 15000
    // o2 alloc = floor(10000/40000 * 20000) = floor(5000) = 5000
    // Total = 20000, no remainder.
    Quantity o1_total = 0, o2_total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].resting_order == &o1) o1_total += results[i].quantity;
        if (results[i].resting_order == &o2) o2_total += results[i].quantity;
    }
    EXPECT_EQ(o1_total, 15000);
    EXPECT_EQ(o2_total, 5000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Time-weighted: older orders get slightly more
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, TimeWeightedOlderGetsMore) {
    PriceLevel level{};
    level.price = 1000000;

    // Same quantity, different ages. No priority (below collar).
    Order o1 = make_order(1, 40000, 0);        // age = 10s, tw = 1.0 + 0.1*10 = 2.0
    Order o2 = make_order(2, 40000, 5 * SEC);  // age = 5s,  tw = 1.0 + 0.1*5  = 1.5
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    auto cfg = default_cfg();
    Quantity remaining = 40000;  // half the level
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 10 * SEC, cfg);

    // w1 = 40000 * 2.0 = 80000, w2 = 40000 * 1.5 = 60000, total = 140000
    // o1 alloc = floor(80000/140000 * 40000) = floor(22857.14) = 22857
    // o2 alloc = floor(60000/140000 * 40000) = floor(17142.85) = 17142
    // Total = 39999, remainder = 1 → goes to o1 (FIFO, oldest)
    Quantity o1_total = 0, o2_total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].resting_order == &o1) o1_total += results[i].quantity;
        if (results[i].resting_order == &o2) o2_total += results[i].quantity;
    }
    EXPECT_EQ(o1_total, 22858);  // 22857 + 1 remainder
    EXPECT_EQ(o2_total, 17142);
    EXPECT_EQ(o1_total + o2_total, 40000);
    EXPECT_EQ(remaining, 0);

    // Confirm older order got more despite equal quantity.
    EXPECT_GT(o1_total, o2_total);
}

// ---------------------------------------------------------------------------
// Remainder goes to oldest order (FIFO tiebreak)
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, RemainderGoesToOldest) {
    PriceLevel level{};
    level.price = 1000000;

    // Three equal-sized orders, same age → equal weights.
    // Below collar so no priority.
    Order o1 = make_order(1, 10000, 0);
    Order o2 = make_order(2, 10000, 0);
    Order o3 = make_order(3, 10000, 0);
    Order* orders[] = {&o1, &o2, &o3};
    build_level(level, orders, 3);

    auto cfg = default_cfg();
    Quantity remaining = 10000;  // 10000 across 3 orders
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 0, cfg);

    // Equal weights. Each gets floor(1/3 * 10000) = 3333. Total = 9999.
    // Remainder = 1 → goes to o1 (oldest, FIFO).
    Quantity o1_total = 0, o2_total = 0, o3_total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].resting_order == &o1) o1_total += results[i].quantity;
        if (results[i].resting_order == &o2) o2_total += results[i].quantity;
        if (results[i].resting_order == &o3) o3_total += results[i].quantity;
    }
    EXPECT_EQ(o1_total, 3334);  // 3333 + 1 remainder
    EXPECT_EQ(o2_total, 3333);
    EXPECT_EQ(o3_total, 3333);
    EXPECT_EQ(o1_total + o2_total + o3_total, 10000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Edge: single order at level
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, SingleOrderAtLevel) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100000, 0);  // qualifies for priority
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    auto cfg = default_cfg();
    Quantity remaining = 50000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 1 * SEC, cfg);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].resting_order, &o1);
    EXPECT_EQ(results[0].quantity, 50000);
    EXPECT_EQ(results[0].resting_remaining, 50000);
    EXPECT_EQ(o1.filled_quantity, 50000);
    EXPECT_EQ(o1.remaining_quantity, 50000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Edge: all orders same size and age — equal distribution + FIFO remainder
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, AllOrdersSameSizeAndAge) {
    PriceLevel level{};
    level.price = 1000000;

    // 4 identical orders, all below collar.
    Order o1 = make_order(1, 10000, 0);
    Order o2 = make_order(2, 10000, 0);
    Order o3 = make_order(3, 10000, 0);
    Order o4 = make_order(4, 10000, 0);
    Order* orders[] = {&o1, &o2, &o3, &o4};
    build_level(level, orders, 4);

    auto cfg = default_cfg();
    Quantity remaining = 10000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 0, cfg);

    // Equal weights → each gets floor(10000/4) = 2500. Total = 10000, no remainder.
    Quantity o1_t = 0, o2_t = 0, o3_t = 0, o4_t = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].resting_order == &o1) o1_t += results[i].quantity;
        if (results[i].resting_order == &o2) o2_t += results[i].quantity;
        if (results[i].resting_order == &o3) o3_t += results[i].quantity;
        if (results[i].resting_order == &o4) o4_t += results[i].quantity;
    }
    EXPECT_EQ(o1_t, 2500);
    EXPECT_EQ(o2_t, 2500);
    EXPECT_EQ(o3_t, 2500);
    EXPECT_EQ(o4_t, 2500);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Empty level — no-op
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, EmptyLevelNoFills) {
    PriceLevel level{};
    level.price = 1000000;
    level.head = nullptr;
    level.tail = nullptr;
    level.order_count = 0;
    level.total_quantity = 0;

    auto cfg = default_cfg();
    Quantity remaining = 10000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 0, cfg);

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(remaining, 10000);
}

// ---------------------------------------------------------------------------
// Zero remaining aggressor — no-op
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, ZeroRemainingNoFills) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 100000, 0);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    auto cfg = default_cfg();
    Quantity remaining = 0;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 0, cfg);

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(o1.filled_quantity, 0);
    EXPECT_EQ(o1.remaining_quantity, 100000);
}

// ---------------------------------------------------------------------------
// Priority + pro-rata combined: priority order also participates in pro-rata
// on its remaining (post-priority) quantity.
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, PriorityOrderParticipatesInProRata) {
    PriceLevel level{};
    level.price = 1000000;

    // o1 qualifies for priority and has leftover for pro-rata.
    Order o1 = make_order(1, 500000, 0);        // 50 lots
    Order o2 = make_order(2, 300000, 1 * SEC);  // 30 lots, also qualifies but not first
    Order* orders[] = {&o1, &o2};
    build_level(level, orders, 2);

    auto cfg = default_cfg();  // cap = 200000
    Quantity remaining = 600000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 2 * SEC, cfg);

    // Priority: o1 gets 200000.
    // Pro-rata pool = 600000 - 200000 = 400000.
    // o1 effective = 500000 - 200000 = 300000, age=2s, tw=1.2, w=360000
    // o2 effective = 300000, age=1s, tw=1.1, w=330000
    // total_weight = 690000
    // o1 pro-rata = floor(360000/690000 * 400000) = floor(208695.65) = 208695
    // o2 pro-rata = floor(330000/690000 * 400000) = floor(191304.34) = 191304
    // allocated = 399999, remainder = 1 → o1 (FIFO)
    Quantity o1_total = 0, o2_total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i].resting_order == &o1) o1_total += results[i].quantity;
        if (results[i].resting_order == &o2) o2_total += results[i].quantity;
    }

    // o1 = 200000 (priority) + 208695 (pro-rata) + 1 (remainder) = 408696
    // o2 = 191304
    EXPECT_EQ(o1_total, 408696);
    EXPECT_EQ(o2_total, 191304);
    EXPECT_EQ(o1_total + o2_total, 600000);
    EXPECT_EQ(remaining, 0);
}

// ---------------------------------------------------------------------------
// Aggressor exceeds level — capped at level total
// ---------------------------------------------------------------------------
TEST(GtbprMatchTest, AggressorExceedsLevel) {
    PriceLevel level{};
    level.price = 1000000;

    Order o1 = make_order(1, 50000, 0);
    Order* orders[] = {&o1};
    build_level(level, orders, 1);

    auto cfg = default_cfg();
    Quantity remaining = 200000;
    FillResult results[16]{};
    size_t count = 0;

    GtbprMatch::match(level, remaining, results, count, 1 * SEC, cfg);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(results[0].quantity, 50000);
    EXPECT_EQ(results[0].resting_remaining, 0);
    EXPECT_EQ(remaining, 150000);  // leftover for next level
}

}  // namespace
}  // namespace ice
}  // namespace exchange
