#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>
#include <span>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// CRTP exchange with order lookup for implied fill testing
// ---------------------------------------------------------------------------

class ImpliedFillExchange
    : public MatchingEngine<ImpliedFillExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<ImpliedFillExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    const Order* lookup(OrderId id) const {
        if (id == 0 || id >= 1000) return nullptr;
        return order_index_[id];
    }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ImpliedFillsTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener    md_listener_;
    EngineConfig config_{.tick_size       = 100,
                         .lot_size        = 10000,
                         .price_band_low  = 0,
                         .price_band_high = 0};
    ImpliedFillExchange engine_{config_, order_listener_, md_listener_};

    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty, Timestamp ts,
                            uint64_t account_id = 1) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id      = account_id,
                            .side            = side,
                            .type            = OrderType::Limit,
                            .tif             = TimeInForce::GTC,
                            .price           = price,
                            .quantity        = qty,
                            .stop_price      = 0,
                            .timestamp       = ts,
                            .gtd_expiry      = 0,
                            .display_qty     = 0};
    }

    OrderRequest make_iceberg(uint64_t cl_ord_id, Side side, Price price,
                              Quantity total_qty, Quantity display_qty,
                              Timestamp ts) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id      = 1,
                            .side            = side,
                            .type            = OrderType::Limit,
                            .tif             = TimeInForce::GTC,
                            .price           = price,
                            .quantity        = total_qty,
                            .stop_price      = 0,
                            .timestamp       = ts,
                            .gtd_expiry      = 0,
                            .display_qty     = display_qty};
    }
};

// ===========================================================================
// apply_implied_fills: 2-leg batch, both succeed
// ===========================================================================

TEST_F(ImpliedFillsTest, TwoLegBatchSuccess) {
    // Place a resting bid and a resting ask (different prices, no match)
    engine_.new_order(make_limit(100, Side::Buy,  1000, 10000, 1));  // id=1
    engine_.new_order(make_limit(200, Side::Sell, 1100, 10000, 2));  // id=2
    order_listener_.clear();
    md_listener_.clear();

    LegFill fills[] = {
        {.order_id = 1, .fill_price = 1000, .fill_qty = 10000},
        {.order_id = 2, .fill_price = 1100, .fill_qty = 10000},
    };
    bool ok = engine_.apply_implied_fills(fills, 100);
    ASSERT_TRUE(ok);

    // Both orders should be fully filled and deallocated
    EXPECT_EQ(engine_.lookup(1), nullptr);
    EXPECT_EQ(engine_.lookup(2), nullptr);

    // Order events: 2 OrderFilled (one per leg)
    size_t filled_count = 0;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) ++filled_count;
    }
    EXPECT_EQ(filled_count, 2u);

    // MD events should include 2 Trades, 2 L3 Fills, L2 updates, 1 TopOfBook
    size_t trade_count = 0;
    size_t tob_count = 0;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) ++trade_count;
        if (std::holds_alternative<TopOfBook>(ev)) ++tob_count;
    }
    EXPECT_EQ(trade_count, 2u);
    EXPECT_EQ(tob_count, 1u);  // fired once at end
}

// ===========================================================================
// apply_implied_fills: 2 legs, second fails (order gone) -> zero fills
// ===========================================================================

TEST_F(ImpliedFillsTest, TwoLegSecondOrderGone) {
    engine_.new_order(make_limit(100, Side::Buy,  1000, 10000, 1));  // id=1
    engine_.new_order(make_limit(200, Side::Sell, 1100, 10000, 2));  // id=2

    // Cancel the second order so it's gone
    engine_.cancel_order(2, 50);
    order_listener_.clear();
    md_listener_.clear();

    LegFill fills[] = {
        {.order_id = 1, .fill_price = 1000, .fill_qty = 10000},
        {.order_id = 2, .fill_price = 1100, .fill_qty = 10000},
    };
    bool ok = engine_.apply_implied_fills(fills, 100);
    ASSERT_FALSE(ok);

    // No events should have been emitted
    EXPECT_EQ(order_listener_.size(), 0u);
    EXPECT_EQ(md_listener_.size(), 0u);

    // First order should still be intact
    const Order* o1 = engine_.lookup(1);
    ASSERT_NE(o1, nullptr);
    EXPECT_EQ(o1->remaining_quantity, 10000);
    EXPECT_EQ(o1->filled_quantity, 0);
}

// ===========================================================================
// apply_implied_fills: 2 legs, second fails (insufficient qty) -> zero fills
// ===========================================================================

TEST_F(ImpliedFillsTest, TwoLegInsufficientQty) {
    engine_.new_order(make_limit(100, Side::Buy,  1000, 20000, 1));  // id=1
    engine_.new_order(make_limit(200, Side::Sell, 1100, 10000, 2));  // id=2
    order_listener_.clear();
    md_listener_.clear();

    LegFill fills[] = {
        {.order_id = 1, .fill_price = 1000, .fill_qty = 20000},
        {.order_id = 2, .fill_price = 1100, .fill_qty = 20000},  // only 10000 available
    };
    bool ok = engine_.apply_implied_fills(fills, 100);
    ASSERT_FALSE(ok);

    // No events emitted, both orders unchanged
    EXPECT_EQ(order_listener_.size(), 0u);
    EXPECT_EQ(md_listener_.size(), 0u);
    EXPECT_EQ(engine_.lookup(1)->remaining_quantity, 20000);
    EXPECT_EQ(engine_.lookup(2)->remaining_quantity, 10000);
}

// ===========================================================================
// apply_implied_fills: single leg fill (degenerate case)
// ===========================================================================

TEST_F(ImpliedFillsTest, SingleLegFill) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 20000, 1));  // id=1
    order_listener_.clear();
    md_listener_.clear();

    LegFill fills[] = {
        {.order_id = 1, .fill_price = 1000, .fill_qty = 10000},
    };
    bool ok = engine_.apply_implied_fills(fills, 100);
    ASSERT_TRUE(ok);

    // Order should be partially filled
    const Order* o1 = engine_.lookup(1);
    ASSERT_NE(o1, nullptr);
    EXPECT_EQ(o1->filled_quantity, 10000);
    EXPECT_EQ(o1->remaining_quantity, 10000);

    // Should have partial fill event
    size_t partial_count = 0;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderPartiallyFilled>(ev)) {
            auto& pf = std::get<OrderPartiallyFilled>(ev);
            EXPECT_EQ(pf.aggressor_id, 1u);
            EXPECT_EQ(pf.resting_id, 1u);  // implied: same id for both
            EXPECT_EQ(pf.quantity, 10000);
            EXPECT_EQ(pf.aggressor_remaining, 10000);
            ++partial_count;
        }
    }
    EXPECT_EQ(partial_count, 1u);
}

// ===========================================================================
// apply_implied_fills: iceberg tranche reveal after visible portion consumed
// ===========================================================================

TEST_F(ImpliedFillsTest, IcebergTrancheReveal) {
    // Iceberg: total_qty=30000, display_qty=10000
    engine_.new_order(make_iceberg(100, Side::Buy, 1000, 30000, 10000, 1));
    // id=1, remaining_quantity initially = display_qty = 10000
    order_listener_.clear();
    md_listener_.clear();

    // Fill the visible tranche
    LegFill fills[] = {
        {.order_id = 1, .fill_price = 1000, .fill_qty = 10000},
    };
    bool ok = engine_.apply_implied_fills(fills, 100);
    ASSERT_TRUE(ok);

    // Iceberg should have revealed next tranche
    const Order* o1 = engine_.lookup(1);
    ASSERT_NE(o1, nullptr);
    EXPECT_EQ(o1->filled_quantity, 10000);
    // Next tranche: min(display_qty=10000, total_qty-filled=20000) = 10000
    EXPECT_EQ(o1->remaining_quantity, 10000);

    // Should have a PartiallyFilled event (not Filled, since hidden qty remains)
    bool has_partial = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderPartiallyFilled>(ev)) {
            has_partial = true;
        }
        // Should NOT have OrderFilled since hidden quantity remains
        EXPECT_FALSE(std::holds_alternative<OrderFilled>(ev));
    }
    EXPECT_TRUE(has_partial);
}

// ===========================================================================
// best_order_id: populated book returns correct ID
// ===========================================================================

TEST_F(ImpliedFillsTest, BestOrderIdPopulated) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1));  // id=1
    engine_.new_order(make_limit(200, Side::Buy,  900, 10000, 2));  // id=2
    engine_.new_order(make_limit(300, Side::Sell, 1100, 10000, 3));  // id=3

    auto bid_id = engine_.best_order_id(Side::Buy);
    ASSERT_TRUE(bid_id.has_value());
    EXPECT_EQ(*bid_id, 1u);  // best bid is at 1000, order id=1

    auto ask_id = engine_.best_order_id(Side::Sell);
    ASSERT_TRUE(ask_id.has_value());
    EXPECT_EQ(*ask_id, 3u);  // best ask is at 1100, order id=3
}

// ===========================================================================
// best_order_id: empty side returns nullopt
// ===========================================================================

TEST_F(ImpliedFillsTest, BestOrderIdEmptySide) {
    auto bid_id = engine_.best_order_id(Side::Buy);
    EXPECT_FALSE(bid_id.has_value());

    auto ask_id = engine_.best_order_id(Side::Sell);
    EXPECT_FALSE(ask_id.has_value());

    // Add only a bid — ask should still be nullopt
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1));

    bid_id = engine_.best_order_id(Side::Buy);
    EXPECT_TRUE(bid_id.has_value());

    ask_id = engine_.best_order_id(Side::Sell);
    EXPECT_FALSE(ask_id.has_value());
}

// ===========================================================================
// best_order_id: after fill changes BBO, returns new head
// ===========================================================================

TEST_F(ImpliedFillsTest, BestOrderIdAfterFillChangesBBO) {
    // Two bids at different prices
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1));  // id=1
    engine_.new_order(make_limit(200, Side::Buy,  900, 10000, 2));  // id=2

    auto bid_id = engine_.best_order_id(Side::Buy);
    ASSERT_TRUE(bid_id.has_value());
    EXPECT_EQ(*bid_id, 1u);  // best is id=1 at 1000

    // Fill order 1 completely via implied fill
    order_listener_.clear();
    md_listener_.clear();

    LegFill fills[] = {
        {.order_id = 1, .fill_price = 1000, .fill_qty = 10000},
    };
    ASSERT_TRUE(engine_.apply_implied_fills(fills, 100));

    // Now best bid should be id=2 at 900
    bid_id = engine_.best_order_id(Side::Buy);
    ASSERT_TRUE(bid_id.has_value());
    EXPECT_EQ(*bid_id, 2u);
}

}  // namespace
}  // namespace exchange
