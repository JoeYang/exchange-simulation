#include "exchange-core/stop_book.h"

#include <gtest/gtest.h>

#include "exchange-core/object_pool.h"
#include "exchange-core/types.h"

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static constexpr size_t kMaxLevels = 64;

Order make_stop(OrderId id, Side side, Price stop_price, Quantity qty) {
    Order o{};
    o.id                = id;
    o.side              = side;
    o.price             = stop_price;
    o.quantity          = qty;
    o.remaining_quantity = qty;
    o.type              = OrderType::Stop;
    return o;
}

// Collect PriceLevel prices from head towards tail.
std::vector<Price> level_prices(const PriceLevel* head) {
    std::vector<Price> out;
    for (const PriceLevel* lv = head; lv; lv = lv->next) {
        out.push_back(lv->price);
    }
    return out;
}

// Collect order ids from head to tail within a price level.
std::vector<OrderId> order_ids(const PriceLevel* lv) {
    std::vector<OrderId> out;
    for (const Order* o = lv->head; o; o = o->next) {
        out.push_back(o->id);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Empty book
// ---------------------------------------------------------------------------

TEST(StopBookTest, EmptyBookInitialState) {
    StopBook book;
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.buy_stops(), nullptr);
    EXPECT_EQ(book.sell_stops(), nullptr);
    EXPECT_FALSE(book.has_triggered_stops(100'0000));
    EXPECT_EQ(book.next_triggered_stop(100'0000), nullptr);
}

// ---------------------------------------------------------------------------
// Insert buy stop — levels sorted ascending (lowest stop price first)
// ---------------------------------------------------------------------------

TEST(StopBookTest, InsertBuyStopCreatesLevel) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};

    PriceLevel* result = book.insert_stop(&o1, lv);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->price, 105'0000);
    EXPECT_EQ(result->order_count, 1u);
    EXPECT_EQ(result->total_quantity, 10'0000);
    EXPECT_EQ(result->head, &o1);
    EXPECT_EQ(result->tail, &o1);
    EXPECT_EQ(o1.level, result);
    EXPECT_EQ(book.buy_stops(), result);
    EXPECT_FALSE(book.empty());
}

TEST(StopBookTest, InsertBuyStopsAscendingSort) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Insert in non-sorted order: 107, 105, 106
    Price prices[] = {107'0000, 105'0000, 106'0000};
    Order orders[3];
    for (int i = 0; i < 3; ++i) {
        orders[i] = make_stop(i + 1, Side::Buy, prices[i], 1'0000);
        PriceLevel* lv = pool.allocate();
        *lv = PriceLevel{};
        book.insert_stop(&orders[i], lv);
    }

    // Buy stops ascending: 105, 106, 107 — lowest triggers first
    EXPECT_EQ((std::vector<Price>{105'0000, 106'0000, 107'0000}),
              level_prices(book.buy_stops()));
    EXPECT_EQ(book.buy_stops()->price, 105'0000);
}

// ---------------------------------------------------------------------------
// Insert sell stop — levels sorted descending (highest stop price first)
// ---------------------------------------------------------------------------

TEST(StopBookTest, InsertSellStopCreatesLevel) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Sell, 95'0000, 5'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};

    PriceLevel* result = book.insert_stop(&o1, lv);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->price, 95'0000);
    EXPECT_EQ(result->order_count, 1u);
    EXPECT_EQ(result->total_quantity, 5'0000);
    EXPECT_EQ(book.sell_stops(), result);
    EXPECT_FALSE(book.empty());
}

TEST(StopBookTest, InsertSellStopsDescendingSort) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Insert in non-sorted order: 93, 95, 94
    Price prices[] = {93'0000, 95'0000, 94'0000};
    Order orders[3];
    for (int i = 0; i < 3; ++i) {
        orders[i] = make_stop(i + 1, Side::Sell, prices[i], 1'0000);
        PriceLevel* lv = pool.allocate();
        *lv = PriceLevel{};
        book.insert_stop(&orders[i], lv);
    }

    // Sell stops descending: 95, 94, 93 — highest triggers first
    EXPECT_EQ((std::vector<Price>{95'0000, 94'0000, 93'0000}),
              level_prices(book.sell_stops()));
    EXPECT_EQ(book.sell_stops()->price, 95'0000);
}

// ---------------------------------------------------------------------------
// Insert at existing level — new_level_if_needed not consumed
// ---------------------------------------------------------------------------

TEST(StopBookTest, InsertBuyStopAtExistingLevelReturnsExistingLevel) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    Order o2 = make_stop(2, Side::Buy, 105'0000, 20'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_stop(&o1, lv1);

    PriceLevel* spare = pool.allocate();
    *spare = PriceLevel{};
    PriceLevel* lv2 = book.insert_stop(&o2, spare);

    // Must return the original level, not spare
    EXPECT_EQ(lv2, lv1);
    EXPECT_EQ(lv1->order_count, 2u);
    EXPECT_EQ(lv1->total_quantity, 30'0000);
    EXPECT_EQ(lv1->head, &o1);
    EXPECT_EQ(lv1->tail, &o2);
    EXPECT_EQ(o2.level, lv1);
}

// ---------------------------------------------------------------------------
// Multiple stops at same level — FIFO order preserved
// ---------------------------------------------------------------------------

TEST(StopBookTest, MultipleBuyStopsAtSameLevelFifoOrder) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    Order o2 = make_stop(2, Side::Buy, 105'0000, 20'0000);
    Order o3 = make_stop(3, Side::Buy, 105'0000, 5'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    PriceLevel* s2 = pool.allocate();
    *s2 = PriceLevel{};
    book.insert_stop(&o2, s2);

    PriceLevel* s3 = pool.allocate();
    *s3 = PriceLevel{};
    book.insert_stop(&o3, s3);

    EXPECT_EQ((std::vector<OrderId>{1, 2, 3}), order_ids(book.buy_stops()));
}

// ---------------------------------------------------------------------------
// Trigger logic: buy stops trigger at or above stop price
// ---------------------------------------------------------------------------

TEST(StopBookTest, BuyStopTriggersAtExactStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade exactly at stop price triggers it
    EXPECT_TRUE(book.has_triggered_stops(105'0000));
    EXPECT_EQ(book.next_triggered_stop(105'0000), &o1);
}

TEST(StopBookTest, BuyStopTriggersAboveStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade above stop price triggers it
    EXPECT_TRUE(book.has_triggered_stops(106'0000));
    EXPECT_EQ(book.next_triggered_stop(106'0000), &o1);
}

TEST(StopBookTest, BuyStopDoesNotTriggerBelowStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade below stop price does NOT trigger
    EXPECT_FALSE(book.has_triggered_stops(104'0000));
    EXPECT_EQ(book.next_triggered_stop(104'0000), nullptr);
}

// ---------------------------------------------------------------------------
// Trigger logic: sell stops trigger at or below stop price
// ---------------------------------------------------------------------------

TEST(StopBookTest, SellStopTriggersAtExactStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Sell, 95'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade exactly at stop price triggers it
    EXPECT_TRUE(book.has_triggered_stops(95'0000));
    EXPECT_EQ(book.next_triggered_stop(95'0000), &o1);
}

TEST(StopBookTest, SellStopTriggersBelowStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Sell, 95'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade below stop price triggers it
    EXPECT_TRUE(book.has_triggered_stops(94'0000));
    EXPECT_EQ(book.next_triggered_stop(94'0000), &o1);
}

TEST(StopBookTest, SellStopDoesNotTriggerAboveStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Sell, 95'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade above stop price does NOT trigger
    EXPECT_FALSE(book.has_triggered_stops(96'0000));
    EXPECT_EQ(book.next_triggered_stop(96'0000), nullptr);
}

// ---------------------------------------------------------------------------
// No trigger when stop book has only the other side
// ---------------------------------------------------------------------------

TEST(StopBookTest, NoBuyTriggerWhenOnlySellStopsPresent) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Only sell stop at 95 — a high trade price should not trigger it
    Order o1 = make_stop(1, Side::Sell, 95'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade at 110 — above sell stop price, so NO trigger
    EXPECT_FALSE(book.has_triggered_stops(110'0000));
    EXPECT_EQ(book.next_triggered_stop(110'0000), nullptr);
}

TEST(StopBookTest, NoSellTriggerWhenOnlyBuyStopsPresent) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Only buy stop at 105
    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Trade at 90 — below buy stop price, so NO trigger
    EXPECT_FALSE(book.has_triggered_stops(90'0000));
    EXPECT_EQ(book.next_triggered_stop(90'0000), nullptr);
}

// ---------------------------------------------------------------------------
// next_triggered_stop returns the head order of the best-priority triggered level
// ---------------------------------------------------------------------------

TEST(StopBookTest, NextTriggeredBuyStopIsLowestStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Two buy stop levels: 104 and 106. Trade at 107 triggers both.
    Order o_low  = make_stop(1, Side::Buy, 104'0000, 10'0000);
    Order o_high = make_stop(2, Side::Buy, 106'0000, 10'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_stop(&o_low, lv1);

    PriceLevel* lv2 = pool.allocate();
    *lv2 = PriceLevel{};
    book.insert_stop(&o_high, lv2);

    // next_triggered_stop should return the order at the lowest stop level (104)
    EXPECT_EQ(book.next_triggered_stop(107'0000), &o_low);
}

TEST(StopBookTest, NextTriggeredSellStopIsHighestStopPrice) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Two sell stop levels: 94 and 96. Trade at 93 triggers both.
    Order o_high = make_stop(1, Side::Sell, 96'0000, 10'0000);
    Order o_low  = make_stop(2, Side::Sell, 94'0000, 10'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_stop(&o_high, lv1);

    PriceLevel* lv2 = pool.allocate();
    *lv2 = PriceLevel{};
    book.insert_stop(&o_low, lv2);

    // next_triggered_stop should return order at highest stop level (96)
    EXPECT_EQ(book.next_triggered_stop(93'0000), &o_high);
}

// ---------------------------------------------------------------------------
// Remove stop order
// ---------------------------------------------------------------------------

TEST(StopBookTest, RemoveBuyStopLevelRemainsWhenOtherOrdersExist) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    Order o2 = make_stop(2, Side::Buy, 105'0000, 20'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    PriceLevel* spare = pool.allocate();
    *spare = PriceLevel{};
    book.insert_stop(&o2, spare);

    PriceLevel* emptied = book.remove_stop(&o1);

    EXPECT_EQ(emptied, nullptr);  // level NOT emptied
    EXPECT_EQ(lv->order_count, 1u);
    EXPECT_EQ(lv->total_quantity, 20'0000);
    EXPECT_EQ(lv->head, &o2);
    EXPECT_EQ(lv->tail, &o2);
}

TEST(StopBookTest, RemoveLastBuyStopReturnsLevel) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    PriceLevel* emptied = book.remove_stop(&o1);

    EXPECT_EQ(emptied, lv);          // caller must deallocate
    EXPECT_EQ(book.buy_stops(), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST(StopBookTest, RemoveLastSellStopReturnsLevel) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Sell, 95'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    PriceLevel* emptied = book.remove_stop(&o1);

    EXPECT_EQ(emptied, lv);
    EXPECT_EQ(book.sell_stops(), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST(StopBookTest, RemoveStopBeforeTriggerPreventsIt) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    // Cancel the stop before price reaches it
    PriceLevel* emptied = book.remove_stop(&o1);
    pool.deallocate(emptied);

    // Now price rises past the original stop price — nothing triggers
    EXPECT_FALSE(book.has_triggered_stops(110'0000));
    EXPECT_EQ(book.next_triggered_stop(110'0000), nullptr);
}

// ---------------------------------------------------------------------------
// Both sides present — correct side triggers
// ---------------------------------------------------------------------------

TEST(StopBookTest, OnlyBuySideTriggesWhenPriceRises) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order buy_stop  = make_stop(1, Side::Buy,  105'0000, 10'0000);
    Order sell_stop = make_stop(2, Side::Sell,  95'0000, 10'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_stop(&buy_stop, lv1);

    PriceLevel* lv2 = pool.allocate();
    *lv2 = PriceLevel{};
    book.insert_stop(&sell_stop, lv2);

    // Trade at 110: buy stop (105) triggered, sell stop (95) is NOT
    EXPECT_TRUE(book.has_triggered_stops(110'0000));
    EXPECT_EQ(book.next_triggered_stop(110'0000), &buy_stop);
}

TEST(StopBookTest, OnlySellSideTriggersWhenPriceFalls) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order buy_stop  = make_stop(1, Side::Buy,  105'0000, 10'0000);
    Order sell_stop = make_stop(2, Side::Sell,  95'0000, 10'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_stop(&buy_stop, lv1);

    PriceLevel* lv2 = pool.allocate();
    *lv2 = PriceLevel{};
    book.insert_stop(&sell_stop, lv2);

    // Trade at 90: sell stop (95) triggered, buy stop (105) is NOT
    EXPECT_TRUE(book.has_triggered_stops(90'0000));
    EXPECT_EQ(book.next_triggered_stop(90'0000), &sell_stop);
}

// ---------------------------------------------------------------------------
// Remove order — intrusive pointer cleared
// ---------------------------------------------------------------------------

TEST(StopBookTest, RemoveStopClearsOrderPointers) {
    StopBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_stop(1, Side::Buy, 105'0000, 5'0000);
    Order o2 = make_stop(2, Side::Buy, 105'0000, 5'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_stop(&o1, lv);

    PriceLevel* spare = pool.allocate();
    *spare = PriceLevel{};
    book.insert_stop(&o2, spare);

    book.remove_stop(&o1);

    EXPECT_EQ(o1.prev, nullptr);
    EXPECT_EQ(o1.next, nullptr);
}

}  // namespace
}  // namespace exchange
