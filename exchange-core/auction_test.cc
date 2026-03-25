#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Minimal CRTP exchange for testing -- uses all defaults
// ---------------------------------------------------------------------------

class AuctionTestExchange
    : public MatchingEngine<AuctionTestExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 200, 100, 2000> {
public:
    using Base = MatchingEngine<AuctionTestExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 200, 100, 2000>;
    using Base::Base;

    // Place an order directly into the book without going through continuous
    // matching.  Used to simulate auction-collection phase where orders are
    // accepted but not matched.  Returns true on success.
    bool place_resting(Side side, Price price, Quantity qty,
                       uint64_t cl_id = 0, uint64_t account = 1,
                       Timestamp ts = 1000) {
        Order* order = order_pool_.allocate();
        if (!order) return false;
        PriceLevel* lv = level_pool_.allocate();
        if (!lv) { order_pool_.deallocate(order); return false; }

        order->id = next_order_id_++;
        order->client_order_id = cl_id;
        order->account_id = account;
        order->price = price;
        order->quantity = qty;
        order->filled_quantity = 0;
        order->remaining_quantity = qty;
        order->side = side;
        order->type = OrderType::Limit;
        order->tif = TimeInForce::GTC;
        order->timestamp = ts;
        order->gtd_expiry = 0;
        order->display_qty = 0;
        order->total_qty = qty;
        order->prev = nullptr;
        order->next = nullptr;
        order->level = nullptr;
        order_index_[order->id] = order;

        PriceLevel* used = book_.insert_order(order, lv);
        if (used != lv) level_pool_.deallocate(lv);
        return true;
    }

    size_t resting_order_count() const { return active_order_count(); }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class AuctionTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    // tick_size=100 (0.01), lot_size=10000 (1.0), no price bands
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};
    AuctionTestExchange engine_{config_, order_listener_, md_listener_};

    // Place an order directly in the book (simulates auction collection phase
    // where orders rest without being matched).
    void add_limit(Side side, Price price, Quantity qty) {
        ASSERT_TRUE(engine_.place_resting(side, price, qty));
    }
};

// ===========================================================================
// Test 1: Empty book — no auction price
// ===========================================================================

TEST_F(AuctionTest, EmptyBook_NoPrice) {
    AuctionResult result = engine_.calculate_auction_price(1000000);
    EXPECT_FALSE(result.has_price);
    EXPECT_EQ(result.price, 0);
    EXPECT_EQ(result.matched_volume, 0);
    EXPECT_EQ(result.buy_surplus, 0);
    EXPECT_EQ(result.sell_surplus, 0);
}

// ===========================================================================
// Test 2: One bid, one ask, crossing — price in the overlap
//
// Book:
//   Buy  100.00 (1000000)  qty=10000
//   Sell  99.00 (990000)   qty=10000
//
// Both candidate prices: 990000 and 1000000.
//   At 990000: buy_vol = 10000 (bid >= 990000), sell_vol = 10000 (ask <= 990000)
//              matched = 10000, imbalance = 0
//   At 1000000: buy_vol = 10000 (bid >= 1000000), sell_vol = 10000 (ask <= 1000000)
//               matched = 10000, imbalance = 0
// Both have same matched_volume and imbalance=0. Tiebreak: closest to
// reference_price=995000. 990000 is closer (dist=5000) vs 1000000 (dist=5000).
// Equal distance -> higher price wins: 1000000.
// ===========================================================================

TEST_F(AuctionTest, OneBidOneAsk_Crossing) {
    // Prices: 100.00 = 1000000, 99.00 = 990000 (PRICE_SCALE=10000)
    add_limit(Side::Buy,  1000000, 10000);
    add_limit(Side::Sell,  990000, 10000);

    // Reference at 99.50 = 995000 — equidistant; higher price wins.
    AuctionResult result = engine_.calculate_auction_price(995000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.matched_volume, 10000);
    EXPECT_EQ(result.buy_surplus, 0);
    EXPECT_EQ(result.sell_surplus, 0);
    // Both candidates match equally; higher price wins tiebreak.
    EXPECT_EQ(result.price, 1000000);
}

TEST_F(AuctionTest, OneBidOneAsk_Crossing_RefPriceFavorsLower) {
    // Same book, but reference price is at 99.10 = 991000 — closer to 990000.
    add_limit(Side::Buy,  1000000, 10000);
    add_limit(Side::Sell,  990000, 10000);

    AuctionResult result = engine_.calculate_auction_price(991000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.matched_volume, 10000);
    EXPECT_EQ(result.price, 990000);  // dist=1000 vs 9000
}

// ===========================================================================
// Test 3: No crossing — best bid < best ask
//
// Book:
//   Buy  98.00 (980000)  qty=10000
//   Sell 99.00 (990000)  qty=10000
//
// At 980000: sell_vol at price <= 980000 = 0 → matched=0
// At 990000: buy_vol at price >= 990000 = 0 → matched=0
// No candidate yields matched > 0 → no price.
// ===========================================================================

TEST_F(AuctionTest, NoCrossing_BestBidBelowBestAsk) {
    add_limit(Side::Buy,   980000, 10000);
    add_limit(Side::Sell,  990000, 10000);

    AuctionResult result = engine_.calculate_auction_price(985000);
    EXPECT_FALSE(result.has_price);
    EXPECT_EQ(result.matched_volume, 0);
}

// ===========================================================================
// Test 4: Multiple bids, one ask — price maximizes volume
//
// Book (bids sorted descending, ask single):
//   Buy 102.00 (1020000) qty=10000
//   Buy 101.00 (1010000) qty=20000
//   Buy 100.00 (1000000) qty=30000
//   Sell 100.00 (1000000) qty=50000
//
// Candidates: 1000000, 1010000, 1020000
//   At 1000000: buy_vol=60000 (all bids), sell_vol=50000 → matched=50000, imbal=10000
//   At 1010000: buy_vol=30000, sell_vol=0 → matched=0 (no asks <= 1010000 except none)
//     Wait — the ask is at 1000000, so sell_vol at p=1010000: asks<=1010000 = 50000
//     buy_vol at p=1010000: bids>=1010000 = 10000+20000 = 30000
//     matched=30000, imbal=20000
//   At 1020000: buy_vol=10000, sell_vol=50000 (ask 1000000 <= 1020000)
//     matched=10000, imbal=40000
//
// Winner: 1000000 with matched=50000 (highest).
// ===========================================================================

TEST_F(AuctionTest, MultipleBids_OneAsk_MaximizesVolume) {
    add_limit(Side::Buy,  1020000, 10000);
    add_limit(Side::Buy,  1010000, 20000);
    add_limit(Side::Buy,  1000000, 30000);
    add_limit(Side::Sell, 1000000, 50000);

    AuctionResult result = engine_.calculate_auction_price(1010000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1000000);
    EXPECT_EQ(result.matched_volume, 50000);
    EXPECT_EQ(result.buy_surplus, 10000);   // 60000 - 50000
    EXPECT_EQ(result.sell_surplus, 0);
}

// ===========================================================================
// Test 5: Symmetric book — price closest to reference when volume is tied
//
// Book:
//   Buy  102.00 (1020000) qty=10000
//   Buy  101.00 (1010000) qty=10000
//   Sell 100.00 (1000000) qty=10000
//   Sell 101.00 (1010000) qty=10000
//
// Candidates: 1000000, 1010000, 1020000
//   At 1000000: buy_vol=20000, sell_vol=10000 → matched=10000, imbal=10000
//   At 1010000: buy_vol=20000, sell_vol=20000 → matched=20000, imbal=0
//   At 1020000: buy_vol=10000, sell_vol=20000 → matched=10000, imbal=10000
//
// Winner: 1010000 (matched=20000 is highest).
// ===========================================================================

TEST_F(AuctionTest, SymmetricBook_MaxVolumeAtMiddle) {
    add_limit(Side::Buy,  1020000, 10000);
    add_limit(Side::Buy,  1010000, 10000);
    add_limit(Side::Sell, 1000000, 10000);
    add_limit(Side::Sell, 1010000, 10000);

    // Reference at 1010000 exactly.
    AuctionResult result = engine_.calculate_auction_price(1010000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1010000);
    EXPECT_EQ(result.matched_volume, 20000);
    EXPECT_EQ(result.buy_surplus, 0);
    EXPECT_EQ(result.sell_surplus, 0);
}

// ===========================================================================
// Test 6: Complex scenario — clear winner among multiple candidates
//
// Book:
//   Buy  105.00 (1050000) qty=10000
//   Buy  104.00 (1040000) qty=15000
//   Buy  103.00 (1030000) qty=25000
//   Buy  102.00 (1020000) qty=30000
//   Sell 101.00 (1010000) qty=20000
//   Sell 103.00 (1030000) qty=40000
//   Sell 104.00 (1040000) qty=20000
//
// Candidates (sorted): 1010000, 1020000, 1030000, 1040000, 1050000
//
//   At 1010000:
//     buy_vol = 10000+15000+25000+30000 = 80000
//     sell_vol = 20000  (only asks <= 1010000 → 1010000)
//     matched = 20000, imbal = 60000
//
//   At 1020000:
//     buy_vol = 10000+15000+25000+30000 = 80000
//     sell_vol = 20000  (asks <= 1020000 → 1010000)
//     matched = 20000, imbal = 60000
//
//   At 1030000:
//     buy_vol = 10000+15000+25000 = 50000
//     sell_vol = 20000+40000 = 60000
//     matched = 50000, imbal = 10000
//
//   At 1040000:
//     buy_vol = 10000+15000 = 25000
//     sell_vol = 20000+40000+20000 = 80000
//     matched = 25000, imbal = 55000
//
//   At 1050000:
//     buy_vol = 10000
//     sell_vol = 80000
//     matched = 10000, imbal = 70000
//
// Winner: 1030000 with matched=50000 (highest volume).
// ===========================================================================

TEST_F(AuctionTest, ComplexScenario_ClearWinner) {
    add_limit(Side::Buy,  1050000, 10000);
    add_limit(Side::Buy,  1040000, 15000);
    add_limit(Side::Buy,  1030000, 25000);
    add_limit(Side::Buy,  1020000, 30000);
    add_limit(Side::Sell, 1010000, 20000);
    add_limit(Side::Sell, 1030000, 40000);
    add_limit(Side::Sell, 1040000, 20000);

    AuctionResult result = engine_.calculate_auction_price(1030000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1030000);
    EXPECT_EQ(result.matched_volume, 50000);
    EXPECT_EQ(result.buy_surplus, 0);    // buy_vol=50000 - matched=50000
    EXPECT_EQ(result.sell_surplus, 10000); // sell_vol=60000 - matched=50000
}

}  // namespace
}  // namespace exchange
