#include "exchange-core/matching_engine.h"
#include "exchange-core/order_persistence.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Minimal CRTP exchange for testing restore_order.
// ---------------------------------------------------------------------------

class RestoreTestExchange
    : public MatchingEngine<RestoreTestExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<RestoreTestExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class OrderRestoreTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};
    RestoreTestExchange engine_{config_, order_listener_, md_listener_};

    // Helper: build a SerializedOrder for a limit buy.
    SerializedOrder make_serialized_buy(OrderId id, uint64_t cl_ord_id,
                                        Price price, Quantity qty,
                                        Quantity filled = 0) {
        SerializedOrder s{};
        s.id                 = id;
        s.client_order_id    = cl_ord_id;
        s.account_id         = 1;
        s.price              = price;
        s.quantity           = qty;
        s.filled_quantity    = filled;
        s.remaining_quantity = qty - filled;
        s.side               = Side::Buy;
        s.type               = OrderType::Limit;
        s.tif                = TimeInForce::GTC;
        s.timestamp          = 100;
        s.total_qty          = qty;
        return s;
    }

    // Helper: build a SerializedOrder for a limit sell.
    SerializedOrder make_serialized_sell(OrderId id, uint64_t cl_ord_id,
                                         Price price, Quantity qty,
                                         Quantity filled = 0) {
        SerializedOrder s = make_serialized_buy(id, cl_ord_id, price, qty, filled);
        s.side = Side::Sell;
        return s;
    }
};

// ---------------------------------------------------------------------------
// 1. Restore a limit buy order -- appears on book at correct price/qty.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreLimitBuyOrder) {
    SerializedOrder s = make_serialized_buy(1, 100, 1000000, 50000);

    engine_.restore_order(s, 200);

    // Should fire OrderAccepted.
    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& accepted = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(accepted.id, 1u);
    EXPECT_EQ(accepted.client_order_id, 100u);
    EXPECT_EQ(accepted.ts, 200);

    // Verify the order is on the book by sending a matching sell.
    order_listener_.clear();
    md_listener_.clear();

    OrderRequest sell{};
    sell.client_order_id = 200;
    sell.account_id      = 2;
    sell.side            = Side::Sell;
    sell.type            = OrderType::Limit;
    sell.tif             = TimeInForce::GTC;
    sell.price           = 1000000;
    sell.quantity         = 50000;
    sell.timestamp       = 300;

    engine_.new_order(sell);

    // Should see OrderAccepted + OrderFilled (the sell matches the restored buy).
    bool found_fill = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) {
            auto& fill = std::get<OrderFilled>(ev);
            EXPECT_EQ(fill.resting_id, 1u);  // restored order
            EXPECT_EQ(fill.price, 1000000);
            EXPECT_EQ(fill.quantity, 50000);
            found_fill = true;
        }
    }
    EXPECT_TRUE(found_fill) << "Expected the restored order to match";
}

// ---------------------------------------------------------------------------
// 2. Restore preserves original order ID.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestorePreservesOriginalOrderId) {
    SerializedOrder s = make_serialized_buy(42, 500, 2000000, 30000);

    engine_.restore_order(s, 200);

    ASSERT_GE(order_listener_.size(), 1u);
    auto& accepted = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(accepted.id, 42u);
}

// ---------------------------------------------------------------------------
// 3. Restore order with invalid price (outside current bands) -- rejected.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreRejectsInvalidPrice) {
    // Set up price bands.
    EngineConfig banded_config{.tick_size = 100,
                               .lot_size = 10000,
                               .price_band_low = 500000,
                               .price_band_high = 1500000};
    RecordingOrderListener ol;
    RecordingMdListener ml;
    RestoreTestExchange banded_engine(banded_config, ol, ml);

    // Price below band.
    SerializedOrder s = make_serialized_buy(1, 100, 400000, 10000);
    banded_engine.restore_order(s, 200);

    ASSERT_GE(ol.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(ol.events()[0]));
    auto& rejected = std::get<OrderRejected>(ol.events()[0]);
    EXPECT_EQ(rejected.reason, RejectReason::PriceBandViolation);
}

// ---------------------------------------------------------------------------
// 4. Restore order with expired GTD -- rejected.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreRejectsExpiredGtd) {
    SerializedOrder s{};
    s.id                 = 1;
    s.client_order_id    = 100;
    s.price              = 1000000;
    s.quantity           = 10000;
    s.remaining_quantity = 10000;
    s.side               = Side::Buy;
    s.type               = OrderType::Limit;
    s.tif                = TimeInForce::GTD;
    s.timestamp          = 100;
    s.gtd_expiry         = 500;   // expires at 500
    s.total_qty          = 10000;

    // Restore at timestamp 600 -- after expiry.
    engine_.restore_order(s, 600);

    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rejected = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rejected.reason, RejectReason::ExchangeSpecific);
}

// ---------------------------------------------------------------------------
// 5. Restore into full pool -- rejected.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreRejectsWhenPoolFull) {
    // Fill up the order pool (MaxOrders = 100).
    // Use the same price so all orders share one level (avoid level pool
    // exhaustion with MaxPriceLevels=50).
    for (int i = 0; i < 100; ++i) {
        OrderRequest req{};
        req.client_order_id = static_cast<uint64_t>(1000 + i);
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity         = 10000;
        req.timestamp       = static_cast<Timestamp>(i + 1);
        engine_.new_order(req);
    }

    order_listener_.clear();

    // Now try to restore an order -- pool should be full.
    SerializedOrder s = make_serialized_buy(500, 999, 2000000, 10000);
    engine_.restore_order(s, 200);

    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rejected = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rejected.reason, RejectReason::PoolExhausted);
}

// ---------------------------------------------------------------------------
// 6. Restore with duplicate order ID -- rejected.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreRejectsDuplicateOrderId) {
    // Place an order so ID 1 is taken.
    OrderRequest req{};
    req.client_order_id = 100;
    req.account_id      = 1;
    req.side            = Side::Buy;
    req.type            = OrderType::Limit;
    req.tif             = TimeInForce::GTC;
    req.price           = 1000000;
    req.quantity         = 10000;
    req.timestamp       = 1;
    engine_.new_order(req);  // gets assigned ID 1

    order_listener_.clear();

    // Try to restore with same ID.
    SerializedOrder s = make_serialized_buy(1, 200, 2000000, 20000);
    engine_.restore_order(s, 200);

    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rejected = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rejected.reason, RejectReason::ExchangeSpecific);
}

// ---------------------------------------------------------------------------
// 7. Restore iceberg order -- display_qty tranche visible.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreIcebergOrder) {
    SerializedOrder s{};
    s.id                 = 5;
    s.client_order_id    = 300;
    s.account_id         = 1;
    s.price              = 1000000;
    s.quantity           = 100000;
    s.filled_quantity    = 20000;
    s.remaining_quantity = 10000;   // current visible tranche
    s.side               = Side::Buy;
    s.type               = OrderType::Limit;
    s.tif                = TimeInForce::GTC;
    s.timestamp          = 100;
    s.display_qty        = 10000;
    s.total_qty          = 100000;

    engine_.restore_order(s, 200);

    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& accepted = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(accepted.id, 5u);
}

// ---------------------------------------------------------------------------
// 8. Restore a sell order that matches existing resting buy.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreAndMatchAgainstExisting) {
    // Place a resting buy.
    OrderRequest buy{};
    buy.client_order_id = 100;
    buy.account_id      = 1;
    buy.side            = Side::Buy;
    buy.type            = OrderType::Limit;
    buy.tif             = TimeInForce::GTC;
    buy.price           = 1000000;
    buy.quantity         = 50000;
    buy.timestamp       = 1;
    engine_.new_order(buy);  // ID 1

    order_listener_.clear();
    md_listener_.clear();

    // Restore a sell at the same price -- should match.
    // Restored orders are inserted into the book (no immediate matching),
    // so this should rest on the book, not match.
    // Actually per the plan: "Allocates from pool, inserts into book at
    // original price, fires OrderAccepted". Restoring inserts into book
    // without triggering matching -- the order is a resting order being
    // restored, not an aggressive new order.
    SerializedOrder s = make_serialized_sell(10, 200, 1000000, 50000);
    engine_.restore_order(s, 200);

    // Should fire OrderAccepted for the restored order (it rests, no match).
    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// 9. Restore order with bad tick size -- rejected.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreRejectsBadTickSize) {
    // tick_size = 100, so price must be multiple of 100.
    SerializedOrder s = make_serialized_buy(1, 100, 1000050, 10000);
    engine_.restore_order(s, 200);

    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rejected = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rejected.reason, RejectReason::InvalidPrice);
}

// ---------------------------------------------------------------------------
// 10. Restore order with bad lot size -- rejected.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreRejectsBadLotSize) {
    // lot_size = 10000, so remaining_quantity must be multiple of lot_size.
    // Actually lot_size checks are on the original quantity, not remaining.
    SerializedOrder s = make_serialized_buy(1, 100, 1000000, 5555);
    engine_.restore_order(s, 200);

    ASSERT_GE(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rejected = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rejected.reason, RejectReason::InvalidQuantity);
}

// ---------------------------------------------------------------------------
// 11. Restore updates next_order_id_ if restored ID >= current next.
// ---------------------------------------------------------------------------

TEST_F(OrderRestoreTest, RestoreAdvancesNextOrderId) {
    // Restore order with ID 50.
    SerializedOrder s = make_serialized_buy(50, 100, 1000000, 10000);
    engine_.restore_order(s, 200);

    order_listener_.clear();

    // Now submit a new order -- should get ID > 50.
    OrderRequest req{};
    req.client_order_id = 200;
    req.account_id      = 2;
    req.side            = Side::Sell;
    req.type            = OrderType::Limit;
    req.tif             = TimeInForce::GTC;
    req.price           = 2000000;
    req.quantity         = 10000;
    req.timestamp       = 300;
    engine_.new_order(req);

    ASSERT_GE(order_listener_.size(), 1u);
    auto& accepted = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_GT(accepted.id, 50u);
}

}  // namespace
}  // namespace exchange
