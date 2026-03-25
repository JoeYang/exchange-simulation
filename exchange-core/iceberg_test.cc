#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Test exchange that exposes order lookup for field verification
// ---------------------------------------------------------------------------

class IcebergTestExchange
    : public MatchingEngine<IcebergTestExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<IcebergTestExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    // Expose order lookup so tests can inspect iceberg fields directly
    const Order* lookup(OrderId id) const {
        if (id == 0 || id >= 1000) return nullptr;
        return order_index_[id];
    }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class IcebergTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener    md_listener_;
    // tick_size=100, lot_size=10000 — same as matching_engine_test
    EngineConfig config_{.tick_size       = 100,
                         .lot_size        = 10000,
                         .price_band_low  = 0,
                         .price_band_high = 0};
    IcebergTestExchange engine_{config_, order_listener_, md_listener_};

    // Build a plain limit request (no iceberg)
    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty, Timestamp ts) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id      = 1,
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

    // Build an iceberg limit request
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
// 1. Non-iceberg order: display_qty=0, behaves exactly as before
// ===========================================================================

TEST_F(IcebergTest, NonIcebergOrderBehavesNormally) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 50000, 1000));

    // Accepted
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& acc = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(acc.id, 1u);

    // Iceberg fields should be zero / equal to quantity
    const Order* o = engine_.lookup(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->display_qty, 0);
    EXPECT_EQ(o->total_qty, 50000);
    EXPECT_EQ(o->remaining_quantity, 50000);  // full qty visible

    // L2 depth should report the full quantity
    ASSERT_GE(md_listener_.size(), 2u);
    auto& l2 = std::get<DepthUpdate>(md_listener_.events()[1]);
    EXPECT_EQ(l2.total_qty, 50000);
}

// ===========================================================================
// 2. Iceberg validation: display_qty > quantity must be rejected
// ===========================================================================

TEST_F(IcebergTest, RejectIcebergDisplayQtyExceedsTotal) {
    // display_qty (30000) > quantity (20000) — invalid
    engine_.new_order(make_iceberg(100, Side::Buy, 1000, 20000, 30000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.client_order_id, 100u);
    EXPECT_EQ(rej.reason, RejectReason::InvalidQuantity);
}

// ===========================================================================
// 3. Iceberg validation: display_qty not aligned to lot_size must be rejected
// ===========================================================================

TEST_F(IcebergTest, RejectIcebergDisplayQtyNotAlignedToLotSize) {
    // lot_size=10000, display_qty=5000 — not aligned
    engine_.new_order(make_iceberg(100, Side::Buy, 1000, 50000, 5000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.client_order_id, 100u);
    EXPECT_EQ(rej.reason, RejectReason::InvalidQuantity);
}

// ===========================================================================
// 4. Iceberg validation: display_qty == quantity is valid (edge case)
// ===========================================================================

TEST_F(IcebergTest, IcebergDisplayQtyEqualToTotalIsValid) {
    // display_qty == quantity: degenerate iceberg, but valid
    engine_.new_order(make_iceberg(100, Side::Buy, 1000, 30000, 30000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));

    const Order* o = engine_.lookup(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->display_qty, 30000);
    EXPECT_EQ(o->total_qty, 30000);
    EXPECT_EQ(o->remaining_quantity, 30000);
}

// ===========================================================================
// 5. Iceberg order accepted: fields correctly initialized
// ===========================================================================

TEST_F(IcebergTest, IcebergOrderFieldsCorrectlyInitialized) {
    // total=50000 (5 lots), display=10000 (1 lot)
    engine_.new_order(make_iceberg(200, Side::Sell, 2000, 50000, 10000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& acc = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(acc.id, 1u);

    const Order* o = engine_.lookup(1);
    ASSERT_NE(o, nullptr);

    // display_qty and total_qty set from request
    EXPECT_EQ(o->display_qty, 10000);
    EXPECT_EQ(o->total_qty, 50000);

    // Only the first tranche is visible in remaining_quantity
    EXPECT_EQ(o->remaining_quantity, 10000);

    // The full original quantity is preserved in quantity
    EXPECT_EQ(o->quantity, 50000);

    // L3 OrderBookAction (Add) should show the display quantity, not total
    ASSERT_GE(md_listener_.size(), 1u);
    auto& l3 = std::get<OrderBookAction>(md_listener_.events()[0]);
    EXPECT_EQ(l3.action, OrderBookAction::Add);
    EXPECT_EQ(l3.qty, 10000);  // display tranche, not 50000

    // L2 depth should also show only display quantity
    ASSERT_GE(md_listener_.size(), 2u);
    auto& l2 = std::get<DepthUpdate>(md_listener_.events()[1]);
    EXPECT_EQ(l2.total_qty, 10000);
}

// ===========================================================================
// 6. Iceberg with no lot_size constraint: any positive display_qty valid
// ===========================================================================

TEST_F(IcebergTest, IcebergAcceptedWithNoLotSizeConstraint) {
    // Engine with no lot_size
    EngineConfig no_lot_config{.tick_size       = 0,
                               .lot_size        = 0,
                               .price_band_low  = 0,
                               .price_band_high = 0};
    IcebergTestExchange eng{no_lot_config, order_listener_, md_listener_};

    OrderRequest req{.client_order_id = 300,
                     .account_id      = 1,
                     .side            = Side::Buy,
                     .type            = OrderType::Limit,
                     .tif             = TimeInForce::GTC,
                     .price           = 1000,
                     .quantity        = 7777,
                     .stop_price      = 0,
                     .timestamp       = 1000,
                     .gtd_expiry      = 0,
                     .display_qty     = 111};
    eng.new_order(req);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));

    const Order* o = eng.lookup(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->display_qty, 111);
    EXPECT_EQ(o->total_qty, 7777);
    EXPECT_EQ(o->remaining_quantity, 111);
}

// ===========================================================================
// 7. Non-iceberg: total_qty equals quantity (invariant check)
// ===========================================================================

TEST_F(IcebergTest, NonIcebergTotalQtyEqualsQuantity) {
    engine_.new_order(make_limit(400, Side::Sell, 1500, 40000, 2000));

    const Order* o = engine_.lookup(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->total_qty, o->quantity);
    EXPECT_EQ(o->display_qty, 0);
}

}  // namespace
}  // namespace exchange
