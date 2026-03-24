#include "exchange-core/orderbook.h"

#include <gtest/gtest.h>

#include "exchange-core/object_pool.h"
#include "exchange-core/types.h"

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Test fixtures and helpers
// ---------------------------------------------------------------------------

static constexpr size_t kMaxLevels = 64;

// Allocate an Order with minimal fields set.
Order make_order(OrderId id, Side side, Price price, Quantity qty) {
    Order o{};
    o.id = id;
    o.side = side;
    o.price = price;
    o.quantity = qty;
    o.remaining_quantity = qty;
    return o;
}

// Collect PriceLevel prices from best towards worst into a vector.
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
// Single bid insertion
// ---------------------------------------------------------------------------

TEST(OrderbookTest, InsertBidCreatesLevel) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 10'0000);
    PriceLevel* new_lv = pool.allocate();
    *new_lv = PriceLevel{};

    PriceLevel* lv = book.insert_order(&o1, new_lv);

    ASSERT_NE(lv, nullptr);
    EXPECT_EQ(lv->price, 100'0000);
    EXPECT_EQ(lv->order_count, 1u);
    EXPECT_EQ(lv->total_quantity, 10'0000);
    EXPECT_EQ(lv->head, &o1);
    EXPECT_EQ(lv->tail, &o1);
    EXPECT_EQ(o1.level, lv);

    EXPECT_EQ(book.best_bid(), lv);
    EXPECT_EQ(book.best_ask(), nullptr);
    EXPECT_FALSE(book.empty());
}

// ---------------------------------------------------------------------------
// Single ask insertion
// ---------------------------------------------------------------------------

TEST(OrderbookTest, InsertAskCreatesLevel) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Sell, 101'0000, 5'0000);
    PriceLevel* new_lv = pool.allocate();
    *new_lv = PriceLevel{};

    PriceLevel* lv = book.insert_order(&o1, new_lv);

    ASSERT_NE(lv, nullptr);
    EXPECT_EQ(lv->price, 101'0000);
    EXPECT_EQ(lv->order_count, 1u);
    EXPECT_EQ(lv->total_quantity, 5'0000);
    EXPECT_EQ(book.best_ask(), lv);
    EXPECT_EQ(book.best_bid(), nullptr);
}

// ---------------------------------------------------------------------------
// Insert at existing level — new_level_if_needed not consumed
// ---------------------------------------------------------------------------

TEST(OrderbookTest, InsertOrderAtExistingLevelReturnsExistingLevel) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 10'0000);
    Order o2 = make_order(2, Side::Buy, 100'0000, 20'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_order(&o1, lv1);

    // Provide a fresh level — should NOT be used since 100.0000 already exists.
    PriceLevel* spare = pool.allocate();
    *spare = PriceLevel{};
    PriceLevel* lv2 = book.insert_order(&o2, spare);

    // Must return the original level, not spare.
    EXPECT_EQ(lv2, lv1);
    EXPECT_EQ(lv1->order_count, 2u);
    EXPECT_EQ(lv1->total_quantity, 30'0000);
    EXPECT_EQ(lv1->head, &o1);
    EXPECT_EQ(lv1->tail, &o2);
    EXPECT_EQ(o2.level, lv1);

    // Pool: spare was not consumed — caller still holds it.
    EXPECT_EQ(book.best_bid(), lv1);
}

// ---------------------------------------------------------------------------
// FIFO order within a price level
// ---------------------------------------------------------------------------

TEST(OrderbookTest, FifoOrderWithinLevel) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 5'0000);
    Order o2 = make_order(2, Side::Buy, 100'0000, 8'0000);
    Order o3 = make_order(3, Side::Buy, 100'0000, 3'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_order(&o1, lv);

    PriceLevel* spare2 = pool.allocate();
    *spare2 = PriceLevel{};
    book.insert_order(&o2, spare2);

    PriceLevel* spare3 = pool.allocate();
    *spare3 = PriceLevel{};
    book.insert_order(&o3, spare3);

    const PriceLevel* best = book.best_bid();
    ASSERT_NE(best, nullptr);
    EXPECT_EQ((std::vector<OrderId>{1, 2, 3}), order_ids(best));
}

// ---------------------------------------------------------------------------
// Multiple bids: sorted descending
// ---------------------------------------------------------------------------

TEST(OrderbookTest, InsertMultipleBidsDescendingSort) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Insert in non-sorted order: 100, 102, 101
    Price prices[] = {100'0000, 102'0000, 101'0000};
    Order orders[3];
    for (int i = 0; i < 3; ++i) {
        orders[i] = make_order(i + 1, Side::Buy, prices[i], 1'0000);
        PriceLevel* lv = pool.allocate();
        *lv = PriceLevel{};
        book.insert_order(&orders[i], lv);
    }

    // Expected descending: 102, 101, 100
    EXPECT_EQ((std::vector<Price>{102'0000, 101'0000, 100'0000}),
              level_prices(book.best_bid()));
    EXPECT_EQ(book.best_bid()->price, 102'0000);
}

// ---------------------------------------------------------------------------
// Multiple asks: sorted ascending
// ---------------------------------------------------------------------------

TEST(OrderbookTest, InsertMultipleAsksAscendingSort) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    // Insert in non-sorted order: 103, 101, 102
    Price prices[] = {103'0000, 101'0000, 102'0000};
    Order orders[3];
    for (int i = 0; i < 3; ++i) {
        orders[i] = make_order(i + 1, Side::Sell, prices[i], 1'0000);
        PriceLevel* lv = pool.allocate();
        *lv = PriceLevel{};
        book.insert_order(&orders[i], lv);
    }

    // Expected ascending: 101, 102, 103
    EXPECT_EQ((std::vector<Price>{101'0000, 102'0000, 103'0000}),
              level_prices(book.best_ask()));
    EXPECT_EQ(book.best_ask()->price, 101'0000);
}

// ---------------------------------------------------------------------------
// Remove order — level stays when other orders remain
// ---------------------------------------------------------------------------

TEST(OrderbookTest, RemoveOrderUpdatesLevelQuantity) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 10'0000);
    Order o2 = make_order(2, Side::Buy, 100'0000, 20'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_order(&o1, lv);

    PriceLevel* spare = pool.allocate();
    *spare = PriceLevel{};
    book.insert_order(&o2, spare);

    // Remove first order; level should still exist.
    PriceLevel* emptied = book.remove_order(&o1);

    EXPECT_EQ(emptied, nullptr);          // level NOT emptied
    EXPECT_EQ(lv->order_count, 1u);
    EXPECT_EQ(lv->total_quantity, 20'0000);
    EXPECT_EQ(lv->head, &o2);
    EXPECT_EQ(lv->tail, &o2);
    EXPECT_EQ(book.best_bid(), lv);
}

// ---------------------------------------------------------------------------
// Remove last order from level — level is returned for deallocation
// ---------------------------------------------------------------------------

TEST(OrderbookTest, RemoveLastOrderFromLevelReturnsLevel) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 10'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_order(&o1, lv);

    PriceLevel* emptied = book.remove_order(&o1);

    EXPECT_EQ(emptied, lv);    // caller must deallocate this
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(o1.level, nullptr);
}

// ---------------------------------------------------------------------------
// Best bid/ask update after level removal
// ---------------------------------------------------------------------------

TEST(OrderbookTest, BestBidUpdatesAfterLevelRemoval) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o_high = make_order(1, Side::Buy, 102'0000, 1'0000);
    Order o_low  = make_order(2, Side::Buy, 100'0000, 1'0000);

    PriceLevel* lv_high = pool.allocate();
    *lv_high = PriceLevel{};
    book.insert_order(&o_high, lv_high);

    PriceLevel* lv_low = pool.allocate();
    *lv_low = PriceLevel{};
    book.insert_order(&o_low, lv_low);

    EXPECT_EQ(book.best_bid()->price, 102'0000);

    // Remove the best bid's only order.
    PriceLevel* emptied = book.remove_order(&o_high);
    ASSERT_NE(emptied, nullptr);
    pool.deallocate(emptied);

    EXPECT_EQ(book.best_bid()->price, 100'0000);
}

TEST(OrderbookTest, BestAskUpdatesAfterLevelRemoval) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o_low  = make_order(1, Side::Sell, 101'0000, 1'0000);
    Order o_high = make_order(2, Side::Sell, 103'0000, 1'0000);

    PriceLevel* lv_low = pool.allocate();
    *lv_low = PriceLevel{};
    book.insert_order(&o_low, lv_low);

    PriceLevel* lv_high = pool.allocate();
    *lv_high = PriceLevel{};
    book.insert_order(&o_high, lv_high);

    EXPECT_EQ(book.best_ask()->price, 101'0000);

    PriceLevel* emptied = book.remove_order(&o_low);
    ASSERT_NE(emptied, nullptr);
    pool.deallocate(emptied);

    EXPECT_EQ(book.best_ask()->price, 103'0000);
}

// ---------------------------------------------------------------------------
// Empty book after all removals
// ---------------------------------------------------------------------------

TEST(OrderbookTest, EmptyAfterAllRemovals) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy,  100'0000, 1'0000);
    Order o2 = make_order(2, Side::Sell, 101'0000, 1'0000);

    PriceLevel* lv1 = pool.allocate();
    *lv1 = PriceLevel{};
    book.insert_order(&o1, lv1);

    PriceLevel* lv2 = pool.allocate();
    *lv2 = PriceLevel{};
    book.insert_order(&o2, lv2);

    pool.deallocate(book.remove_order(&o1));
    pool.deallocate(book.remove_order(&o2));

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
}

// ---------------------------------------------------------------------------
// Remove middle order from three-order level
// ---------------------------------------------------------------------------

TEST(OrderbookTest, RemoveMiddleOrderPreservesHeadAndTail) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 1'0000);
    Order o2 = make_order(2, Side::Buy, 100'0000, 2'0000);
    Order o3 = make_order(3, Side::Buy, 100'0000, 3'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_order(&o1, lv);

    PriceLevel* s2 = pool.allocate();
    *s2 = PriceLevel{};
    book.insert_order(&o2, s2);

    PriceLevel* s3 = pool.allocate();
    *s3 = PriceLevel{};
    book.insert_order(&o3, s3);

    PriceLevel* emptied = book.remove_order(&o2);

    EXPECT_EQ(emptied, nullptr);
    EXPECT_EQ(lv->order_count, 2u);
    EXPECT_EQ(lv->total_quantity, 4'0000);
    EXPECT_EQ(lv->head, &o1);
    EXPECT_EQ(lv->tail, &o3);
    EXPECT_EQ((std::vector<OrderId>{1, 3}), order_ids(lv));
}

// ---------------------------------------------------------------------------
// Best bid/ask tracking across mixed insertions
// ---------------------------------------------------------------------------

TEST(OrderbookTest, BestBidBestAskTracking) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    EXPECT_TRUE(book.empty());

    Order b1 = make_order(1, Side::Buy,  99'0000, 1'0000);
    Order b2 = make_order(2, Side::Buy, 100'0000, 1'0000);
    Order a1 = make_order(3, Side::Sell, 101'0000, 1'0000);
    Order a2 = make_order(4, Side::Sell, 102'0000, 1'0000);

    auto alloc_lv = [&]() -> PriceLevel* {
        PriceLevel* lv = pool.allocate();
        *lv = PriceLevel{};
        return lv;
    };

    book.insert_order(&b1, alloc_lv());
    EXPECT_EQ(book.best_bid()->price, 99'0000);

    book.insert_order(&b2, alloc_lv());
    EXPECT_EQ(book.best_bid()->price, 100'0000);

    book.insert_order(&a1, alloc_lv());
    EXPECT_EQ(book.best_ask()->price, 101'0000);

    book.insert_order(&a2, alloc_lv());
    EXPECT_EQ(book.best_ask()->price, 101'0000);  // unchanged

    // Remove best bid.
    pool.deallocate(book.remove_order(&b2));
    EXPECT_EQ(book.best_bid()->price, 99'0000);

    // Remove best ask.
    pool.deallocate(book.remove_order(&a1));
    EXPECT_EQ(book.best_ask()->price, 102'0000);
}

// ---------------------------------------------------------------------------
// Insert ascending asks at three distinct prices — verify full list order
// ---------------------------------------------------------------------------

TEST(OrderbookTest, ThreeLevelAskListOrder) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Sell, 105'0000, 1'0000);
    Order o2 = make_order(2, Side::Sell, 103'0000, 1'0000);
    Order o3 = make_order(3, Side::Sell, 104'0000, 1'0000);

    auto alloc_lv = [&]() -> PriceLevel* {
        PriceLevel* lv = pool.allocate();
        *lv = PriceLevel{};
        return lv;
    };
    book.insert_order(&o1, alloc_lv());
    book.insert_order(&o2, alloc_lv());
    book.insert_order(&o3, alloc_lv());

    EXPECT_EQ((std::vector<Price>{103'0000, 104'0000, 105'0000}),
              level_prices(book.best_ask()));
}

// ---------------------------------------------------------------------------
// Level back-pointer set on insert
// ---------------------------------------------------------------------------

TEST(OrderbookTest, OrderLevelBackPointerSet) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Sell, 101'0000, 1'0000);
    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_order(&o1, lv);

    EXPECT_EQ(o1.level, lv);
}

// ---------------------------------------------------------------------------
// Remove order — order's prev/next cleared, level pointer cleared
// ---------------------------------------------------------------------------

TEST(OrderbookTest, RemoveOrderClearsOrderPointers) {
    OrderBook book;
    ObjectPool<PriceLevel, kMaxLevels> pool;

    Order o1 = make_order(1, Side::Buy, 100'0000, 5'0000);
    Order o2 = make_order(2, Side::Buy, 100'0000, 5'0000);

    PriceLevel* lv = pool.allocate();
    *lv = PriceLevel{};
    book.insert_order(&o1, lv);

    PriceLevel* spare = pool.allocate();
    *spare = PriceLevel{};
    book.insert_order(&o2, spare);

    book.remove_order(&o1);

    EXPECT_EQ(o1.prev, nullptr);
    EXPECT_EQ(o1.next, nullptr);
    EXPECT_EQ(o1.level, nullptr);
}

}  // namespace
}  // namespace exchange
