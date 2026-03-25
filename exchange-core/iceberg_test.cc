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

// ===========================================================================
// 8. Iceberg fills one tranche, next tranche revealed, order at back of queue
// ===========================================================================

TEST_F(IcebergTest, IcebergTrancheRevealAfterFill) {
    // Resting iceberg sell: total=30000, display=10000
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 30000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy consumes exactly the first tranche (10000)
    engine_.new_order(make_limit(200, Side::Buy, 1000, 10000, 2000));

    // The iceberg should have revealed a new tranche
    const Order* ice = engine_.lookup(1);
    ASSERT_NE(ice, nullptr);
    EXPECT_EQ(ice->remaining_quantity, 10000);  // second tranche
    EXPECT_EQ(ice->filled_quantity, 10000);      // first tranche filled
    EXPECT_EQ(ice->total_qty, 30000);
}

// ===========================================================================
// 9. Iceberg fully fills (all tranches consumed), removed from book
// ===========================================================================

TEST_F(IcebergTest, IcebergFullyConsumedRemovedFromBook) {
    // Resting iceberg sell: total=20000, display=10000 (2 tranches)
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 20000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy for 20000 — should consume both tranches
    engine_.new_order(make_limit(200, Side::Buy, 1000, 20000, 2000));

    // Iceberg order should be gone from the book
    const Order* ice = engine_.lookup(1);
    EXPECT_EQ(ice, nullptr);

    // Aggressor should be fully filled too (no remainder on book)
    const Order* agg = engine_.lookup(2);
    EXPECT_EQ(agg, nullptr);
}

// ===========================================================================
// 10. Iceberg tranche reveal fires L3 Add and L2 Update callbacks
// ===========================================================================

TEST_F(IcebergTest, IcebergTrancheRevealFiresCallbacks) {
    // Resting iceberg sell: total=30000, display=10000
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 30000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor consumes first tranche
    engine_.new_order(make_limit(200, Side::Buy, 1000, 10000, 2000));

    // Walk MD events to find the L3 Add for the revealed tranche and
    // the L2 Update reflecting it.
    // Expected MD sequence for the fill + reveal:
    //   Trade, L3 Fill (resting), L3 Add (tranche reveal),
    //   L2 Update (level), L1 TopOfBook
    bool found_l3_add = false;
    bool found_l2_update = false;
    for (size_t i = 0; i < md_listener_.size(); ++i) {
        auto& ev = md_listener_.events()[i];
        if (std::holds_alternative<OrderBookAction>(ev)) {
            auto& oba = std::get<OrderBookAction>(ev);
            if (oba.action == OrderBookAction::Add &&
                oba.id == 1 && oba.qty == 10000) {
                found_l3_add = true;
            }
        }
        if (std::holds_alternative<DepthUpdate>(ev)) {
            auto& du = std::get<DepthUpdate>(ev);
            if (du.action == DepthUpdate::Update &&
                du.side == Side::Sell && du.price == 1000 &&
                du.total_qty == 10000) {
                found_l2_update = true;
            }
        }
    }
    EXPECT_TRUE(found_l3_add) << "Expected L3 OrderBookAction(Add) for revealed tranche";
    EXPECT_TRUE(found_l2_update) << "Expected L2 DepthUpdate(Update) after tranche reveal";
}

// ===========================================================================
// 11. Iceberg loses priority on reveal: other orders at same level fill first
// ===========================================================================

TEST_F(IcebergTest, IcebergLosesPriorityOnReveal) {
    // Order A: iceberg sell, total=20000, display=10000 (arrives first)
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 20000, 10000, 1000));
    // Order B: plain sell 10000 (arrives second)
    engine_.new_order(make_limit(101, Side::Sell, 1000, 10000, 2000));
    order_listener_.clear();
    md_listener_.clear();

    // Buy 10000 — should fill iceberg's first tranche (FIFO, it arrived first)
    engine_.new_order(make_limit(200, Side::Buy, 1000, 10000, 3000));

    // Iceberg reveals second tranche and goes to back of queue.
    // Now order B should be ahead of the iceberg's second tranche.
    const Order* ice = engine_.lookup(1);
    const Order* plain = engine_.lookup(2);
    ASSERT_NE(ice, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(ice->remaining_quantity, 10000);  // revealed second tranche
    EXPECT_EQ(plain->remaining_quantity, 10000); // untouched

    order_listener_.clear();
    md_listener_.clear();

    // Buy another 10000 — should fill order B (now at front), not the iceberg
    engine_.new_order(make_limit(300, Side::Buy, 1000, 10000, 4000));

    // Order B should be gone (filled)
    const Order* plain_after = engine_.lookup(2);
    EXPECT_EQ(plain_after, nullptr);

    // Iceberg should still be resting with second tranche
    const Order* ice_after = engine_.lookup(1);
    ASSERT_NE(ice_after, nullptr);
    EXPECT_EQ(ice_after->remaining_quantity, 10000);
    EXPECT_EQ(ice_after->filled_quantity, 10000);  // only first tranche filled
}

// ===========================================================================
// 12. Multiple iceberg orders at same price level
// ===========================================================================

TEST_F(IcebergTest, MultipleIcebergsAtSameLevel) {
    // Iceberg A: total=20000, display=10000
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 20000, 10000, 1000));
    // Iceberg B: total=20000, display=10000
    engine_.new_order(make_iceberg(101, Side::Sell, 1000, 20000, 10000, 2000));
    order_listener_.clear();
    md_listener_.clear();

    // Buy 10000 — fills iceberg A's first tranche (FIFO)
    engine_.new_order(make_limit(200, Side::Buy, 1000, 10000, 3000));

    // A reveals second tranche at back; B is now first in queue
    const Order* a = engine_.lookup(1);
    const Order* b = engine_.lookup(2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->filled_quantity, 10000);
    EXPECT_EQ(a->remaining_quantity, 10000);
    EXPECT_EQ(b->filled_quantity, 0);
    EXPECT_EQ(b->remaining_quantity, 10000);

    order_listener_.clear();
    md_listener_.clear();

    // Buy 10000 — fills iceberg B's first tranche (now first in queue)
    engine_.new_order(make_limit(300, Side::Buy, 1000, 10000, 4000));

    a = engine_.lookup(1);
    b = engine_.lookup(2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // B reveals second tranche at back; A's second tranche is now first
    EXPECT_EQ(a->filled_quantity, 10000);
    EXPECT_EQ(a->remaining_quantity, 10000);
    EXPECT_EQ(b->filled_quantity, 10000);
    EXPECT_EQ(b->remaining_quantity, 10000);

    order_listener_.clear();
    md_listener_.clear();

    // Buy 10000 — fills A's second tranche (now first in queue)
    engine_.new_order(make_limit(400, Side::Buy, 1000, 10000, 5000));

    // A should be fully consumed (20000 total filled)
    a = engine_.lookup(1);
    EXPECT_EQ(a, nullptr);

    // B still has its second tranche
    b = engine_.lookup(2);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->filled_quantity, 10000);
    EXPECT_EQ(b->remaining_quantity, 10000);
}

// ===========================================================================
// 13. Non-iceberg behavior unchanged (regression)
// ===========================================================================

TEST_F(IcebergTest, NonIcebergRegressionFullFill) {
    // Resting plain sell
    engine_.new_order(make_limit(100, Side::Sell, 1000, 20000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy consumes it entirely
    engine_.new_order(make_limit(200, Side::Buy, 1000, 20000, 2000));

    // Resting order gone
    const Order* rest = engine_.lookup(1);
    EXPECT_EQ(rest, nullptr);

    // Aggressor also fully filled
    const Order* agg = engine_.lookup(2);
    EXPECT_EQ(agg, nullptr);

    // Should see OrderFilled for aggressor
    bool found_fill = false;
    for (auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) {
            found_fill = true;
            break;
        }
    }
    EXPECT_TRUE(found_fill);
}

TEST_F(IcebergTest, NonIcebergRegressionPartialFill) {
    // Resting plain sell
    engine_.new_order(make_limit(100, Side::Sell, 1000, 30000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy takes part of it
    engine_.new_order(make_limit(200, Side::Buy, 1000, 10000, 2000));

    // Resting order still present with remainder
    const Order* rest = engine_.lookup(1);
    ASSERT_NE(rest, nullptr);
    EXPECT_EQ(rest->remaining_quantity, 20000);
    EXPECT_EQ(rest->filled_quantity, 10000);
    EXPECT_EQ(rest->display_qty, 0);  // not an iceberg
}

// ===========================================================================
// 14. Iceberg last tranche is smaller than display_qty
// ===========================================================================

TEST_F(IcebergTest, IcebergLastTrancheSmaller) {
    // total=30000, display=20000 — first tranche=20000, second=10000
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 30000, 20000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Buy 20000 — fills first tranche
    engine_.new_order(make_limit(200, Side::Buy, 1000, 20000, 2000));

    const Order* ice = engine_.lookup(1);
    ASSERT_NE(ice, nullptr);
    // Last tranche should be min(20000, 30000-20000) = 10000
    EXPECT_EQ(ice->remaining_quantity, 10000);
    EXPECT_EQ(ice->filled_quantity, 20000);

    // Verify the L3 Add callback shows the smaller tranche size
    bool found_add = false;
    for (size_t i = 0; i < md_listener_.size(); ++i) {
        auto& ev = md_listener_.events()[i];
        if (std::holds_alternative<OrderBookAction>(ev)) {
            auto& oba = std::get<OrderBookAction>(ev);
            if (oba.action == OrderBookAction::Add && oba.id == 1) {
                EXPECT_EQ(oba.qty, 10000);
                found_add = true;
            }
        }
    }
    EXPECT_TRUE(found_add);
}

// ===========================================================================
// 15. Large aggressor fills multiple iceberg tranches in one match
// ===========================================================================

TEST_F(IcebergTest, AggressorFillsMultipleIcebergTranches) {
    // Resting iceberg: total=30000, display=10000 (3 tranches)
    engine_.new_order(make_iceberg(100, Side::Sell, 1000, 30000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Buy 30000 — should consume all 3 tranches
    engine_.new_order(make_limit(200, Side::Buy, 1000, 30000, 2000));

    // Iceberg fully consumed
    const Order* ice = engine_.lookup(1);
    EXPECT_EQ(ice, nullptr);

    // Aggressor also fully filled
    const Order* agg = engine_.lookup(2);
    EXPECT_EQ(agg, nullptr);
}

}  // namespace
}  // namespace exchange
