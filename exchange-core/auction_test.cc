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
                       Timestamp ts = 1000,
                       Quantity display_qty = 0) {
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
        order->remaining_quantity = (display_qty > 0) ? display_qty : qty;
        order->side = side;
        order->type = OrderType::Limit;
        order->tif = TimeInForce::GTC;
        order->timestamp = ts;
        order->gtd_expiry = 0;
        order->display_qty = display_qty;
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

    // Expose order lookup for iceberg field verification
    const Order* lookup(OrderId id) const {
        if (id == 0 || id >= 2000) return nullptr;
        return order_index_[id];
    }
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

    // Place an iceberg order in the book (only display_qty is visible).
    void add_iceberg(Side side, Price price, Quantity total_qty,
                     Quantity display_qty) {
        ASSERT_TRUE(engine_.place_resting(
            side, price, total_qty, /*cl_id=*/0, /*account=*/1,
            /*ts=*/1000, display_qty));
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

// ===========================================================================
// Test 7: Volume tie, imbalance tiebreak picks lower imbalance candidate
//
// Book:
//   Buy  103.00 (1030000) qty=10000
//   Buy  102.00 (1020000) qty=10000
//   Sell 101.00 (1010000) qty=10000
//   Sell 102.00 (1020000) qty=15000
//
// Candidates: 1010000, 1020000, 1030000
//   At 1010000:
//     buy_vol = 20000, sell_vol = 10000 → matched=10000, imbal=10000
//   At 1020000:
//     buy_vol = 20000, sell_vol = 25000 → matched=20000, imbal=5000
//   At 1030000:
//     buy_vol = 10000, sell_vol = 25000 → matched=10000, imbal=15000
//
// Winner: 1020000 (matched=20000, highest).
// ===========================================================================

TEST_F(AuctionTest, VolumeTie_ImbalanceTiebreak) {
    add_limit(Side::Buy,  1030000, 10000);
    add_limit(Side::Buy,  1020000, 10000);
    add_limit(Side::Sell, 1010000, 10000);
    add_limit(Side::Sell, 1020000, 15000);

    AuctionResult result = engine_.calculate_auction_price(1020000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1020000);
    EXPECT_EQ(result.matched_volume, 20000);
    EXPECT_EQ(result.buy_surplus, 0);
    EXPECT_EQ(result.sell_surplus, 5000);
}

// ===========================================================================
// Test 8: Bids only, no asks — no auction price
// ===========================================================================

TEST_F(AuctionTest, BidsOnly_NoPrice) {
    add_limit(Side::Buy, 1000000, 10000);
    add_limit(Side::Buy,  990000, 20000);

    AuctionResult result = engine_.calculate_auction_price(995000);
    EXPECT_FALSE(result.has_price);
    EXPECT_EQ(result.matched_volume, 0);
}

// ===========================================================================
// Test 9: Asks only, no bids — no auction price
// ===========================================================================

TEST_F(AuctionTest, AsksOnly_NoPrice) {
    add_limit(Side::Sell, 1000000, 10000);
    add_limit(Side::Sell, 1010000, 20000);

    AuctionResult result = engine_.calculate_auction_price(1005000);
    EXPECT_FALSE(result.has_price);
    EXPECT_EQ(result.matched_volume, 0);
}

// ===========================================================================
// Test 10: Single matching level — exact price match
//
// Book:
//   Buy  100.00 (1000000) qty=20000
//   Sell 100.00 (1000000) qty=20000
//
// Only candidate: 1000000
//   buy_vol=20000, sell_vol=20000 → matched=20000, imbal=0
// ===========================================================================

TEST_F(AuctionTest, ExactPriceMatch_SingleLevel) {
    add_limit(Side::Buy,  1000000, 20000);
    add_limit(Side::Sell, 1000000, 20000);

    AuctionResult result = engine_.calculate_auction_price(1000000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1000000);
    EXPECT_EQ(result.matched_volume, 20000);
    EXPECT_EQ(result.buy_surplus, 0);
    EXPECT_EQ(result.sell_surplus, 0);
}

// ===========================================================================
// Test 11: Reference price tiebreak — equal imbalance, pick closest to ref
//
// Book:
//   Buy  103.00 (1030000) qty=10000
//   Buy  102.00 (1020000) qty=10000
//   Sell 101.00 (1010000) qty=10000
//   Sell 103.00 (1030000) qty=10000
//
// Candidates: 1010000, 1020000, 1030000
//   At 1010000:
//     buy_vol=20000, sell_vol=10000 → matched=10000, imbal=10000
//   At 1020000:
//     buy_vol=20000, sell_vol=10000 → matched=10000, imbal=10000
//   At 1030000:
//     buy_vol=10000, sell_vol=20000 → matched=10000, imbal=10000
//
// All have matched=10000, imbal=10000. Tiebreak by ref distance.
// Reference at 1010000: dist(1010000)=0 < dist(1020000)=10000 → 1010000 wins.
// ===========================================================================

TEST_F(AuctionTest, EqualImbalance_RefPriceTiebreak) {
    add_limit(Side::Buy,  1030000, 10000);
    add_limit(Side::Buy,  1020000, 10000);
    add_limit(Side::Sell, 1010000, 10000);
    add_limit(Side::Sell, 1030000, 10000);

    AuctionResult result = engine_.calculate_auction_price(1010000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1010000);
    EXPECT_EQ(result.matched_volume, 10000);
}

// ===========================================================================
// Test 12: Higher price convention when all else equal
//
// Same book as Test 11, but reference at midpoint (1020000) — equidistant
// from 1010000 and 1030000; 1020000 is the closest.
// ===========================================================================

TEST_F(AuctionTest, AllTied_HigherPriceWins) {
    // Symmetric book: three candidates all yield matched=10000, imbal=10000.
    // Reference at 1020000: dist(1010000)=10000, dist(1020000)=0, dist(1030000)=10000.
    // 1020000 is closest → wins.
    add_limit(Side::Buy,  1030000, 10000);
    add_limit(Side::Buy,  1020000, 10000);
    add_limit(Side::Sell, 1010000, 10000);
    add_limit(Side::Sell, 1030000, 10000);

    AuctionResult result = engine_.calculate_auction_price(1020000);
    EXPECT_TRUE(result.has_price);
    EXPECT_EQ(result.price, 1020000);
    EXPECT_EQ(result.matched_volume, 10000);
}

// ===========================================================================
// Test 13: Idempotency — calling calculate_auction_price twice with same
// book state returns same result (read-only, no side effects).
// ===========================================================================

TEST_F(AuctionTest, Idempotent_NoSideEffects) {
    add_limit(Side::Buy,  1020000, 20000);
    add_limit(Side::Sell, 1000000, 20000);

    AuctionResult r1 = engine_.calculate_auction_price(1010000);
    AuctionResult r2 = engine_.calculate_auction_price(1010000);

    EXPECT_EQ(r1.has_price, r2.has_price);
    EXPECT_EQ(r1.price, r2.price);
    EXPECT_EQ(r1.matched_volume, r2.matched_volume);
    EXPECT_EQ(r1.buy_surplus, r2.buy_surplus);
    EXPECT_EQ(r1.sell_surplus, r2.sell_surplus);

    // Order book must be unchanged — both orders still resting.
    EXPECT_EQ(engine_.active_order_count(), 2u);
}

// ===========================================================================
// C2 Tests: Auction Execution (execute_auction)
// ===========================================================================

// ---------------------------------------------------------------------------
// C2-1: Simple auction — 1 bid + 1 ask crossing, fill at auction price
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_SimpleOneBidOneAsk) {
    add_limit(Side::Buy,  1000000, 10000);  // bid 100.00 qty=1.0
    add_limit(Side::Sell,  990000, 10000);  // ask  99.00 qty=1.0

    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(/*reference_price=*/995000, /*ts=*/2000);

    // Both orders should be fully filled and removed
    EXPECT_EQ(engine_.active_order_count(), 0u);

    // Verify OrderFilled callback fired (single fill, both sides done)
    ASSERT_GE(order_listener_.size(), 1u);
    const auto& fill_ev = std::get<OrderFilled>(order_listener_.events()[0]);
    EXPECT_EQ(fill_ev.quantity, 10000);
    // Price should be the auction price (1000000 — higher wins equidistant tie)
    EXPECT_EQ(fill_ev.price, 1000000);

    // Verify Trade callback
    bool found_trade = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            const auto& trade = std::get<Trade>(ev);
            EXPECT_EQ(trade.price, 1000000);
            EXPECT_EQ(trade.quantity, 10000);
            EXPECT_EQ(trade.aggressor_side, Side::Buy);
            found_trade = true;
        }
    }
    EXPECT_TRUE(found_trade);
}

// ---------------------------------------------------------------------------
// C2-2: Multiple bids and asks — all matchable orders fill at single price
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_MultipleBidsAndAsks) {
    // Bids: 102.00 qty=10000, 101.00 qty=20000
    // Asks: 100.00 qty=10000, 101.00 qty=15000
    add_limit(Side::Buy,  1020000, 10000);
    add_limit(Side::Buy,  1010000, 20000);
    add_limit(Side::Sell, 1000000, 10000);
    add_limit(Side::Sell, 1010000, 15000);

    // Auction at ref=1010000:
    //   At 1000000: buy=30000, sell=10000 -> matched=10000
    //   At 1010000: buy=30000, sell=25000 -> matched=25000 (winner)
    //   At 1020000: buy=10000, sell=25000 -> matched=10000

    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(1010000, 3000);

    // matched=25000, so 25000 units should trade
    Quantity total_traded = 0;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            const auto& trade = std::get<Trade>(ev);
            EXPECT_EQ(trade.price, 1010000);
            total_traded += trade.quantity;
        }
    }
    EXPECT_EQ(total_traded, 25000);

    // Surplus: buy_surplus = 30000-25000 = 5000 remains on bid side
    // The bid at 102 (10000) is fully consumed, bid at 101 has 5000 remaining
    // Ask side: 10000+15000=25000 all filled
    EXPECT_EQ(engine_.active_order_count(), 1u);  // 1 bid remaining
}

// ---------------------------------------------------------------------------
// C2-3: Partial fill — unmatched surplus remains in book
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_PartialFill_SurplusRemains) {
    add_limit(Side::Buy,  1000000, 30000);  // bid 100.00 qty=3.0
    add_limit(Side::Sell, 1000000, 10000);  // ask 100.00 qty=1.0

    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(1000000, 4000);

    // matched=10000, buy surplus=20000 remains
    EXPECT_EQ(engine_.active_order_count(), 1u);

    // Verify the remaining order has correct qty
    const Order* remaining = engine_.lookup(1);  // first order placed
    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->remaining_quantity, 20000);
    EXPECT_EQ(remaining->filled_quantity, 10000);
}

// ---------------------------------------------------------------------------
// C2-4: Auction with iceberg orders
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_IcebergOrder) {
    // Iceberg bid: total=30000, display=10000 at 100.00
    add_iceberg(Side::Buy, 1000000, 30000, 10000);
    // Normal ask: qty=20000 at 100.00
    add_limit(Side::Sell, 1000000, 20000);

    order_listener_.clear();
    md_listener_.clear();

    // Auction price calc only sees display qty (10000) from bid side.
    // But calculate_auction_price uses total_quantity on the level which
    // for iceberg is only the display qty (10000).
    // So matched = min(10000, 20000) = 10000.
    engine_.execute_auction(1000000, 5000);

    // First tranche of 10000 fills, then iceberg reveals next 10000.
    // The auction algorithm computed matched_volume=10000, so only 10000
    // is matched. After iceberg reveal, the remaining 10000 display +
    // 10000 hidden stays in book.
    Quantity total_traded = 0;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            total_traded += std::get<Trade>(ev).quantity;
        }
    }
    EXPECT_EQ(total_traded, 10000);

    // Iceberg order should have revealed next tranche and remain in book
    const Order* iceberg = engine_.lookup(1);
    ASSERT_NE(iceberg, nullptr);
    EXPECT_EQ(iceberg->filled_quantity, 10000);
    EXPECT_EQ(iceberg->remaining_quantity, 10000);  // revealed tranche

    // Ask partially filled: 20000 - 10000 = 10000 remaining
    const Order* ask_order = engine_.lookup(2);
    ASSERT_NE(ask_order, nullptr);
    EXPECT_EQ(ask_order->remaining_quantity, 10000);
}

// ---------------------------------------------------------------------------
// C2-5: Empty book auction — no fills
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_EmptyBook_NoFills) {
    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(1000000, 6000);

    // No callbacks should fire
    EXPECT_EQ(order_listener_.size(), 0u);
    // Only TopOfBook might fire, but with empty book it should not
    // since execute_auction returns early
    size_t trade_count = 0;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) ++trade_count;
    }
    EXPECT_EQ(trade_count, 0u);
}

// ---------------------------------------------------------------------------
// C2-6: Auction callbacks: verify OrderFilled, Trade, L3, L2, L1 all fire
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_AllCallbacksFire) {
    add_limit(Side::Buy,  1010000, 10000);
    add_limit(Side::Sell, 1000000, 10000);

    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(1005000, 7000);

    // Check order listener has an OrderFilled
    bool has_fill = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) has_fill = true;
    }
    EXPECT_TRUE(has_fill);

    // Check md listener has Trade, OrderBookAction (Fill for both sides),
    // DepthUpdate (Remove for both), TopOfBook
    bool has_trade = false;
    int fill_actions = 0;
    int depth_removes = 0;
    bool has_tob = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) has_trade = true;
        if (std::holds_alternative<OrderBookAction>(ev)) {
            if (std::get<OrderBookAction>(ev).action ==
                OrderBookAction::Fill)
                ++fill_actions;
        }
        if (std::holds_alternative<DepthUpdate>(ev)) {
            if (std::get<DepthUpdate>(ev).action == DepthUpdate::Remove)
                ++depth_removes;
        }
        if (std::holds_alternative<TopOfBook>(ev)) has_tob = true;
    }

    EXPECT_TRUE(has_trade);
    EXPECT_EQ(fill_actions, 2);      // one for bid, one for ask
    EXPECT_EQ(depth_removes, 2);     // both levels removed
    EXPECT_TRUE(has_tob);
}

// ---------------------------------------------------------------------------
// C2-7: Fill price = auction price, NOT individual order prices
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_FillPriceIsAuctionPrice) {
    // Bid at 105.00, ask at 95.00 — wide spread
    // Auction price should be closest to reference
    add_limit(Side::Buy,  1050000, 10000);
    add_limit(Side::Sell,  950000, 10000);

    // Reference at 100.00 = 1000000
    // At 950000: buy=10000 (bid>=950000), sell=10000 -> matched=10000
    // At 1050000: buy=10000, sell=10000 -> matched=10000
    // Both tied on vol/imbalance. Ref dist: 950000->50000, 1050000->50000
    // Equal dist -> higher price wins: 1050000
    // But let's use ref=960000 to favor lower:
    // dist(950000)=10000, dist(1050000)=90000 -> 950000 wins

    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(960000, 8000);

    // All fills must be at 950000 (the auction price), not at 1050000 or 950000
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            EXPECT_EQ(std::get<Trade>(ev).price, 950000);
        }
    }
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) {
            EXPECT_EQ(std::get<OrderFilled>(ev).price, 950000);
        }
    }
}

// ---------------------------------------------------------------------------
// C2-8: No crossing — execute_auction is a no-op
// ---------------------------------------------------------------------------

TEST_F(AuctionTest, ExecuteAuction_NoCrossing_NoOp) {
    add_limit(Side::Buy,   980000, 10000);
    add_limit(Side::Sell, 1000000, 10000);

    order_listener_.clear();
    md_listener_.clear();

    engine_.execute_auction(990000, 9000);

    // No fills
    EXPECT_EQ(order_listener_.size(), 0u);
    EXPECT_EQ(engine_.active_order_count(), 2u);
}

}  // namespace
}  // namespace exchange
