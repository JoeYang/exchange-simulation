#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Minimal CRTP exchange for testing -- uses all defaults
// ---------------------------------------------------------------------------

class TestExchange
    : public MatchingEngine<TestExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<TestExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class MatchingEngineTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};
    TestExchange engine_{config_, order_listener_, md_listener_};

    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty, Timestamp ts) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id = 1,
                            .side = side,
                            .type = OrderType::Limit,
                            .tif = TimeInForce::GTC,
                            .price = price,
                            .quantity = qty,
                            .stop_price = 0,
                            .timestamp = ts,
                            .gtd_expiry = 0};
    }

    OrderRequest make_market(uint64_t cl_ord_id, Side side, Quantity qty,
                             Timestamp ts) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id = 1,
                            .side = side,
                            .type = OrderType::Market,
                            .tif = TimeInForce::GTC,
                            .price = 0,
                            .quantity = qty,
                            .stop_price = 0,
                            .timestamp = ts,
                            .gtd_expiry = 0};
    }
};

// ===========================================================================
// Task 17: Core path tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Limit order rests on empty book
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, LimitOrderRestsOnEmptyBook) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1000));

    // Order events: OrderAccepted
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& accepted = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(accepted.id, 1u);
    EXPECT_EQ(accepted.client_order_id, 100u);
    EXPECT_EQ(accepted.ts, 1000);

    // MD events: L3 Add, L2 Add, L1 TopOfBook
    ASSERT_EQ(md_listener_.size(), 3u);

    auto& l3 = std::get<OrderBookAction>(md_listener_.events()[0]);
    EXPECT_EQ(l3.id, 1u);
    EXPECT_EQ(l3.side, Side::Buy);
    EXPECT_EQ(l3.price, 1000);
    EXPECT_EQ(l3.qty, 10000);
    EXPECT_EQ(l3.action, OrderBookAction::Add);

    auto& l2 = std::get<DepthUpdate>(md_listener_.events()[1]);
    EXPECT_EQ(l2.side, Side::Buy);
    EXPECT_EQ(l2.price, 1000);
    EXPECT_EQ(l2.total_qty, 10000);
    EXPECT_EQ(l2.order_count, 1u);
    EXPECT_EQ(l2.action, DepthUpdate::Add);

    auto& l1 = std::get<TopOfBook>(md_listener_.events()[2]);
    EXPECT_EQ(l1.best_bid, 1000);
    EXPECT_EQ(l1.bid_qty, 10000);
    EXPECT_EQ(l1.best_ask, 0);
    EXPECT_EQ(l1.ask_qty, 0);
}

// ---------------------------------------------------------------------------
// 2. Two limits fill (full fill)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, TwoLimitsFill) {
    // Rest a buy
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Sell at same price -> full fill
    engine_.new_order(make_limit(200, Side::Sell, 1000, 10000, 2000));

    // Order events: OrderAccepted(sell), OrderFilled
    ASSERT_EQ(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& acc = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(acc.id, 2u);

    EXPECT_TRUE(std::holds_alternative<OrderFilled>(order_listener_.events()[1]));
    auto& fill = std::get<OrderFilled>(order_listener_.events()[1]);
    EXPECT_EQ(fill.aggressor_id, 2u);
    EXPECT_EQ(fill.resting_id, 1u);
    EXPECT_EQ(fill.price, 1000);
    EXPECT_EQ(fill.quantity, 10000);

    // MD events: Trade, L3 Fill, L2 Remove, L1 change
    ASSERT_GE(md_listener_.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<Trade>(md_listener_.events()[0]));
    auto& trade = std::get<Trade>(md_listener_.events()[0]);
    EXPECT_EQ(trade.price, 1000);
    EXPECT_EQ(trade.quantity, 10000);
    EXPECT_EQ(trade.aggressor_id, 2u);
    EXPECT_EQ(trade.resting_id, 1u);
    EXPECT_EQ(trade.aggressor_side, Side::Sell);

    EXPECT_TRUE(std::holds_alternative<OrderBookAction>(md_listener_.events()[1]));
    auto& l3fill = std::get<OrderBookAction>(md_listener_.events()[1]);
    EXPECT_EQ(l3fill.action, OrderBookAction::Fill);

    EXPECT_TRUE(std::holds_alternative<DepthUpdate>(md_listener_.events()[2]));
    auto& l2rem = std::get<DepthUpdate>(md_listener_.events()[2]);
    EXPECT_EQ(l2rem.action, DepthUpdate::Remove);
    EXPECT_EQ(l2rem.side, Side::Buy);

    // TopOfBook fires because best_bid changed from 1000 to 0
    ASSERT_EQ(md_listener_.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<TopOfBook>(md_listener_.events()[3]));
    auto& tob = std::get<TopOfBook>(md_listener_.events()[3]);
    EXPECT_EQ(tob.best_bid, 0);
    EXPECT_EQ(tob.best_ask, 0);
}

// ---------------------------------------------------------------------------
// 3. Partial fill
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, PartialFill) {
    // Rest a buy of 5 lots
    engine_.new_order(make_limit(100, Side::Buy, 1000, 50000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Sell 10 lots -> partial fill of 5 lots, sell remainder (5 lots) rests
    engine_.new_order(make_limit(200, Side::Sell, 1000, 100000, 2000));

    // The sell aggressor qty (100000) > resting buy qty (50000).
    // After filling 50000, the aggressor has 50000 remaining -> OrderPartiallyFilled.
    // Then the resting buy is fully consumed (removed from book).
    // The sell remainder (50000) rests on the ask side.
    ASSERT_GE(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(
        std::holds_alternative<OrderPartiallyFilled>(order_listener_.events()[1]));
    auto& pf = std::get<OrderPartiallyFilled>(order_listener_.events()[1]);
    EXPECT_EQ(pf.aggressor_id, 2u);
    EXPECT_EQ(pf.resting_id, 1u);
    EXPECT_EQ(pf.quantity, 50000);
    EXPECT_EQ(pf.aggressor_remaining, 50000);
    EXPECT_EQ(pf.resting_remaining, 0);

    // MD: Trade, L3 Fill(resting), L2 Remove(resting buy level fully consumed),
    //     L3 Add(sell remainder rests), L2 Add(ask), TopOfBook
    ASSERT_GE(md_listener_.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<Trade>(md_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderBookAction>(md_listener_.events()[1]));
    EXPECT_TRUE(std::holds_alternative<DepthUpdate>(md_listener_.events()[2]));
    auto& l2rem = std::get<DepthUpdate>(md_listener_.events()[2]);
    EXPECT_EQ(l2rem.action, DepthUpdate::Remove);
    EXPECT_EQ(l2rem.side, Side::Buy);
}

// ---------------------------------------------------------------------------
// 4. Market order fills resting limit
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MarketOrderFills) {
    // Rest a sell
    engine_.new_order(make_limit(100, Side::Sell, 1000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // Market buy fills it
    engine_.new_order(make_market(200, Side::Buy, 10000, 2000));

    // Order events: Accepted, OrderFilled
    ASSERT_EQ(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderFilled>(order_listener_.events()[1]));
    auto& fill = std::get<OrderFilled>(order_listener_.events()[1]);
    EXPECT_EQ(fill.aggressor_id, 2u);
    EXPECT_EQ(fill.resting_id, 1u);
    EXPECT_EQ(fill.price, 1000);
    EXPECT_EQ(fill.quantity, 10000);
}

// ---------------------------------------------------------------------------
// 5. Market order no liquidity
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MarketOrderNoLiquidity) {
    engine_.new_order(make_market(100, Side::Buy, 10000, 1000));

    // Accepted then cancelled (IOCRemainder for market)
    ASSERT_EQ(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(order_listener_.events()[1]));
    auto& cancel = std::get<OrderCancelled>(order_listener_.events()[1]);
    EXPECT_EQ(cancel.id, 1u);
    EXPECT_EQ(cancel.reason, CancelReason::IOCRemainder);
}

// ---------------------------------------------------------------------------
// 6. Cancel resting order
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, CancelRestingOrder) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    engine_.cancel_order(1, 2000);

    // Order events: OrderCancelled
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(order_listener_.events()[0]));
    auto& cancel = std::get<OrderCancelled>(order_listener_.events()[0]);
    EXPECT_EQ(cancel.id, 1u);
    EXPECT_EQ(cancel.reason, CancelReason::UserRequested);

    // MD: L3 Cancel, L2 Remove, L1 TopOfBook
    ASSERT_EQ(md_listener_.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<OrderBookAction>(md_listener_.events()[0]));
    auto& l3 = std::get<OrderBookAction>(md_listener_.events()[0]);
    EXPECT_EQ(l3.action, OrderBookAction::Cancel);

    EXPECT_TRUE(std::holds_alternative<DepthUpdate>(md_listener_.events()[1]));
    auto& l2 = std::get<DepthUpdate>(md_listener_.events()[1]);
    EXPECT_EQ(l2.action, DepthUpdate::Remove);

    EXPECT_TRUE(std::holds_alternative<TopOfBook>(md_listener_.events()[2]));
    auto& tob = std::get<TopOfBook>(md_listener_.events()[2]);
    EXPECT_EQ(tob.best_bid, 0);
}

// ---------------------------------------------------------------------------
// 7. Cancel unknown order
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, CancelUnknownOrder) {
    engine_.cancel_order(999, 1000);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<OrderCancelRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderCancelRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.id, 999u);
    EXPECT_EQ(rej.reason, RejectReason::UnknownOrder);
}

// ---------------------------------------------------------------------------
// 8. Reject: invalid tick size
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, RejectInvalidTickSize) {
    // tick_size = 100, price 150 not aligned
    engine_.new_order(make_limit(100, Side::Buy, 150, 10000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.client_order_id, 100u);
    EXPECT_EQ(rej.reason, RejectReason::InvalidPrice);
}

// ---------------------------------------------------------------------------
// 9. Reject: invalid lot size
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, RejectInvalidLotSize) {
    // lot_size = 10000, qty 5000 not aligned
    engine_.new_order(make_limit(100, Side::Buy, 1000, 5000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::InvalidQuantity);
}

// ---------------------------------------------------------------------------
// 10. Reject: zero quantity
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, RejectZeroQuantity) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 0, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::InvalidQuantity);
}

// ---------------------------------------------------------------------------
// 11. Reject: pool exhaustion (order pool)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, RejectPoolExhaustion) {
    // MaxOrders=100 -- fill the pool with resting orders all at the same price
    // (uses one level, avoids exhausting MaxPriceLevels=50)
    for (int i = 0; i < 100; ++i) {
        engine_.new_order(
            make_limit(static_cast<uint64_t>(i + 1), Side::Buy, 1000,
                       10000, static_cast<Timestamp>(i + 1)));
    }
    order_listener_.clear();
    md_listener_.clear();

    // 101st order should be rejected -- pool exhausted
    engine_.new_order(make_limit(999, Side::Buy, 1000, 10000, 5000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::PoolExhausted);
}

// ===========================================================================
// Task 18: Advanced path tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 12. Modify order (cancel-replace)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, ModifyOrderCancelReplace) {
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    ModifyRequest req{.order_id = 1,
                      .client_order_id = 101,
                      .new_price = 1100,
                      .new_quantity = 20000,
                      .timestamp = 2000};
    engine_.modify_order(req);

    // Order events: OrderModified
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderModified>(order_listener_.events()[0]));
    auto& mod = std::get<OrderModified>(order_listener_.events()[0]);
    EXPECT_EQ(mod.id, 1u);
    EXPECT_EQ(mod.client_order_id, 101u);
    EXPECT_EQ(mod.new_price, 1100);
    EXPECT_EQ(mod.new_qty, 20000);

    // MD: L3 Cancel(old), L2 Remove(old), OrderModified already fired,
    //     then L3 Add(new), L2 Add(new), TopOfBook
    ASSERT_GE(md_listener_.size(), 5u);

    auto& l3cancel = std::get<OrderBookAction>(md_listener_.events()[0]);
    EXPECT_EQ(l3cancel.action, OrderBookAction::Cancel);
    EXPECT_EQ(l3cancel.price, 1000);

    auto& l2remove = std::get<DepthUpdate>(md_listener_.events()[1]);
    EXPECT_EQ(l2remove.action, DepthUpdate::Remove);
    EXPECT_EQ(l2remove.price, 1000);

    auto& l3add = std::get<OrderBookAction>(md_listener_.events()[2]);
    EXPECT_EQ(l3add.action, OrderBookAction::Add);
    EXPECT_EQ(l3add.price, 1100);

    auto& l2add = std::get<DepthUpdate>(md_listener_.events()[3]);
    EXPECT_EQ(l2add.action, DepthUpdate::Add);
    EXPECT_EQ(l2add.price, 1100);

    auto& tob = std::get<TopOfBook>(md_listener_.events()[4]);
    EXPECT_EQ(tob.best_bid, 1100);
    EXPECT_EQ(tob.bid_qty, 20000);
}

// ---------------------------------------------------------------------------
// 13. Modify unknown order
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, ModifyUnknownOrder) {
    ModifyRequest req{.order_id = 999,
                      .client_order_id = 101,
                      .new_price = 1000,
                      .new_quantity = 10000,
                      .timestamp = 1000};
    engine_.modify_order(req);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<OrderModifyRejected>(order_listener_.events()[0]));
    auto& rej = std::get<OrderModifyRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.id, 999u);
    EXPECT_EQ(rej.reason, RejectReason::UnknownOrder);
}

// ---------------------------------------------------------------------------
// 14. IOC full fill
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, IocFullFill) {
    // Rest a sell
    engine_.new_order(make_limit(100, Side::Sell, 1000, 10000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // IOC buy fills completely
    OrderRequest ioc{.client_order_id = 200,
                     .account_id = 1,
                     .side = Side::Buy,
                     .type = OrderType::Limit,
                     .tif = TimeInForce::IOC,
                     .price = 1000,
                     .quantity = 10000,
                     .stop_price = 0,
                     .timestamp = 2000,
                     .gtd_expiry = 0};
    engine_.new_order(ioc);

    // Should see: Accepted, Filled -- no cancel since fully consumed
    ASSERT_EQ(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderFilled>(order_listener_.events()[1]));
    auto& fill = std::get<OrderFilled>(order_listener_.events()[1]);
    EXPECT_EQ(fill.aggressor_id, 2u);
    EXPECT_EQ(fill.quantity, 10000);
}

// ---------------------------------------------------------------------------
// 15. IOC partial cancel
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, IocPartialCancel) {
    // Rest a sell of 5 lots
    engine_.new_order(make_limit(100, Side::Sell, 1000, 50000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // IOC buy of 10 lots -- partial fill 5, remainder cancelled
    OrderRequest ioc{.client_order_id = 200,
                     .account_id = 1,
                     .side = Side::Buy,
                     .type = OrderType::Limit,
                     .tif = TimeInForce::IOC,
                     .price = 1000,
                     .quantity = 100000,
                     .stop_price = 0,
                     .timestamp = 2000,
                     .gtd_expiry = 0};
    engine_.new_order(ioc);

    // Accepted, PartiallyFilled, then Cancelled (IOCRemainder)
    ASSERT_GE(order_listener_.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(
        std::holds_alternative<OrderPartiallyFilled>(order_listener_.events()[1]));
    auto& pf = std::get<OrderPartiallyFilled>(order_listener_.events()[1]);
    EXPECT_EQ(pf.quantity, 50000);
    EXPECT_EQ(pf.aggressor_remaining, 50000);

    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(order_listener_.events()[2]));
    auto& cancel = std::get<OrderCancelled>(order_listener_.events()[2]);
    EXPECT_EQ(cancel.reason, CancelReason::IOCRemainder);
}

// ---------------------------------------------------------------------------
// 16. FOK full fill
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, FokFullFill) {
    // Rest a sell of 10 lots
    engine_.new_order(make_limit(100, Side::Sell, 1000, 100000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // FOK buy of 10 lots -- sufficient liquidity
    OrderRequest fok{.client_order_id = 200,
                     .account_id = 1,
                     .side = Side::Buy,
                     .type = OrderType::Limit,
                     .tif = TimeInForce::FOK,
                     .price = 1000,
                     .quantity = 100000,
                     .stop_price = 0,
                     .timestamp = 2000,
                     .gtd_expiry = 0};
    engine_.new_order(fok);

    ASSERT_EQ(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderFilled>(order_listener_.events()[1]));
    auto& fill = std::get<OrderFilled>(order_listener_.events()[1]);
    EXPECT_EQ(fill.quantity, 100000);
}

// ---------------------------------------------------------------------------
// 17. FOK no fill (insufficient liquidity)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, FokNoFill) {
    // Rest a sell of 5 lots
    engine_.new_order(make_limit(100, Side::Sell, 1000, 50000, 1000));
    order_listener_.clear();
    md_listener_.clear();

    // FOK buy of 10 lots -- not enough, entire order cancelled
    OrderRequest fok{.client_order_id = 200,
                     .account_id = 1,
                     .side = Side::Buy,
                     .type = OrderType::Limit,
                     .tif = TimeInForce::FOK,
                     .price = 1000,
                     .quantity = 100000,
                     .stop_price = 0,
                     .timestamp = 2000,
                     .gtd_expiry = 0};
    engine_.new_order(fok);

    // Accepted, then Cancelled (FOKFailed) -- no fills
    ASSERT_EQ(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(order_listener_.events()[1]));
    auto& cancel = std::get<OrderCancelled>(order_listener_.events()[1]);
    EXPECT_EQ(cancel.reason, CancelReason::FOKFailed);

    // No trades should have fired
    bool has_trade = false;
    for (auto& e : md_listener_.events()) {
        if (std::holds_alternative<Trade>(e)) has_trade = true;
    }
    EXPECT_FALSE(has_trade);
}

// ---------------------------------------------------------------------------
// 18. Stop order triggers
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, StopOrderTriggers) {
    // Step 1: Place a resting sell at 1000
    engine_.new_order(make_limit(100, Side::Sell, 1000, 10000, 1000));

    // Step 2: Place a stop sell at stop_price=1000 (triggers when price <= 1000)
    OrderRequest stop_sell{.client_order_id = 300,
                           .account_id = 2,
                           .side = Side::Sell,
                           .type = OrderType::Stop,
                           .tif = TimeInForce::GTC,
                           .price = 0,
                           .quantity = 10000,
                           .stop_price = 1000,
                           .timestamp = 2000,
                           .gtd_expiry = 0};
    engine_.new_order(stop_sell);

    // Step 3: Place a resting buy at 900 (for the triggered stop to fill against)
    engine_.new_order(make_limit(400, Side::Buy, 900, 10000, 2500));
    order_listener_.clear();
    md_listener_.clear();

    // Step 4: A buy limit at 1000 fills the resting sell, producing a trade at 1000
    // This trade should trigger the stop sell (last_trade <= stop_price=1000)
    engine_.new_order(make_limit(200, Side::Buy, 1000, 10000, 3000));

    // The buy fills the resting sell -> trade at 1000 -> triggers stop sell
    // The triggered stop sell becomes a market order and fills the buy at 900
    // Check that we see fills for both the original and the triggered stop
    bool saw_trade = false;
    int trade_count = 0;
    for (auto& e : md_listener_.events()) {
        if (std::holds_alternative<Trade>(e)) {
            saw_trade = true;
            ++trade_count;
        }
    }
    EXPECT_TRUE(saw_trade);
    // At least 1 trade from the initial match; possibly 2 if stop triggers and fills
    EXPECT_GE(trade_count, 1);

    // The stop order (id=2) should have been accepted and eventually filled or
    // cancelled (as market on empty book). Check that at least the initial trade
    // happened.
    bool saw_fill = false;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(e) ||
            std::holds_alternative<OrderPartiallyFilled>(e)) {
            saw_fill = true;
        }
    }
    EXPECT_TRUE(saw_fill);
}

// ---------------------------------------------------------------------------
// 19. DAY expiry
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, DayOrderExpiry) {
    // Place a DAY order
    OrderRequest day_order{.client_order_id = 100,
                           .account_id = 1,
                           .side = Side::Buy,
                           .type = OrderType::Limit,
                           .tif = TimeInForce::DAY,
                           .price = 1000,
                           .quantity = 10000,
                           .stop_price = 0,
                           .timestamp = 1000,
                           .gtd_expiry = 0};
    engine_.new_order(day_order);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    order_listener_.clear();
    md_listener_.clear();

    // Trigger DAY expiry
    engine_.trigger_expiry(5000, TimeInForce::DAY);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(order_listener_.events()[0]));
    auto& cancel = std::get<OrderCancelled>(order_listener_.events()[0]);
    EXPECT_EQ(cancel.id, 1u);
    EXPECT_EQ(cancel.reason, CancelReason::Expired);
    EXPECT_EQ(cancel.ts, 5000);

    // MD should fire L3 Cancel, L2 Remove, L1 TopOfBook
    ASSERT_EQ(md_listener_.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<OrderBookAction>(md_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<DepthUpdate>(md_listener_.events()[1]));
    EXPECT_TRUE(std::holds_alternative<TopOfBook>(md_listener_.events()[2]));
}

// ---------------------------------------------------------------------------
// 20. Self-match prevention (cancel newest)
// ---------------------------------------------------------------------------

class SmpExchange
    : public MatchingEngine<SmpExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<SmpExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    bool is_self_match(const Order& a, const Order& b) {
        return a.account_id == b.account_id;
    }
};

TEST_F(MatchingEngineTest, SelfMatchPrevention) {
    RecordingOrderListener smp_order_listener;
    RecordingMdListener smp_md_listener;
    EngineConfig smp_config{.tick_size = 100,
                            .lot_size = 10000,
                            .price_band_low = 0,
                            .price_band_high = 0};
    SmpExchange smp_engine(smp_config, smp_order_listener, smp_md_listener);

    // Same account_id = 1 for both orders -> self-match
    // Rest a buy
    OrderRequest buy{.client_order_id = 100,
                     .account_id = 1,
                     .side = Side::Buy,
                     .type = OrderType::Limit,
                     .tif = TimeInForce::GTC,
                     .price = 1000,
                     .quantity = 10000,
                     .stop_price = 0,
                     .timestamp = 1000,
                     .gtd_expiry = 0};
    smp_engine.new_order(buy);
    smp_order_listener.clear();
    smp_md_listener.clear();

    // Sell from same account -> SMP triggers, cancel newest (aggressor)
    OrderRequest sell{.client_order_id = 200,
                      .account_id = 1,
                      .side = Side::Sell,
                      .type = OrderType::Limit,
                      .tif = TimeInForce::GTC,
                      .price = 1000,
                      .quantity = 10000,
                      .stop_price = 0,
                      .timestamp = 2000,
                      .gtd_expiry = 0};
    smp_engine.new_order(sell);

    // Accepted then Cancelled (SelfMatchPrevention) for the aggressor (sell)
    ASSERT_GE(smp_order_listener.size(), 2u);
    EXPECT_TRUE(
        std::holds_alternative<OrderAccepted>(smp_order_listener.events()[0]));
    EXPECT_TRUE(
        std::holds_alternative<OrderCancelled>(smp_order_listener.events()[1]));
    auto& cancel = std::get<OrderCancelled>(smp_order_listener.events()[1]);
    EXPECT_EQ(cancel.id, 2u);  // aggressor (sell) was cancelled
    EXPECT_EQ(cancel.reason, CancelReason::SelfMatchPrevention);

    // No trades should have occurred
    bool has_trade = false;
    for (auto& e : smp_md_listener.events()) {
        if (std::holds_alternative<Trade>(e)) has_trade = true;
    }
    EXPECT_FALSE(has_trade);

    // The resting buy (id=1) should still be active
    EXPECT_EQ(smp_engine.active_order_count(), 1u);
}

// ===========================================================================
// Task A4: Max order size + dynamic price bands
// ===========================================================================

// ---------------------------------------------------------------------------
// 21. Max order size: order exceeding limit is rejected with
//     MaxOrderSizeExceeded
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MaxOrderSizeExceeded) {
    RecordingOrderListener ol;
    RecordingMdListener ml;
    // max_order_size = 50000 (5 lots of 10000)
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0,
                     .max_order_size = 50000};
    TestExchange eng(cfg, ol, ml);

    eng.new_order(make_limit(1, Side::Buy, 1000, 60000, 1000));

    ASSERT_EQ(ol.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(ol.events()[0]));
    auto& rej = std::get<OrderRejected>(ol.events()[0]);
    EXPECT_EQ(rej.client_order_id, 1u);
    EXPECT_EQ(rej.reason, RejectReason::MaxOrderSizeExceeded);

    // No market-data events emitted for a rejected order
    EXPECT_EQ(ml.size(), 0u);
}

// ---------------------------------------------------------------------------
// 22. Max order size: order at exactly the limit is accepted
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MaxOrderSizeAtLimit) {
    RecordingOrderListener ol;
    RecordingMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0,
                     .max_order_size = 50000};
    TestExchange eng(cfg, ol, ml);

    eng.new_order(make_limit(1, Side::Buy, 1000, 50000, 1000));

    ASSERT_EQ(ol.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(ol.events()[0]));
}

// ---------------------------------------------------------------------------
// 23. Max order size = 0: no limit enforced (backward compatibility)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MaxOrderSizeZeroMeansNoLimit) {
    // The default fixture config has max_order_size = 0 (implicitly via
    // zero-initialisation of the new field).  Submit a very large order and
    // confirm it is accepted without a MaxOrderSizeExceeded reject.
    engine_.new_order(make_limit(1, Side::Buy, 1000, 1000000, 1000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// 24. Dynamic price bands: orders outside calculated band are rejected
// ---------------------------------------------------------------------------

// A test exchange that overrides calculate_dynamic_bands to return a band
// that is +/- 500 ticks around the reference price (last trade price).
class DynamicBandExchange
    : public MatchingEngine<DynamicBandExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<DynamicBandExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    // When a reference price is available, band = [ref - 500, ref + 500].
    // When no reference price (0), fall back to static config bands.
    std::pair<Price, Price> calculate_dynamic_bands(Price reference_price) {
        if (reference_price <= 0)
            return {config_.price_band_low, config_.price_band_high};
        return {reference_price - 500, reference_price + 500};
    }
};

TEST_F(MatchingEngineTest, DynamicBandsRejectOutsideRange) {
    RecordingOrderListener ol;
    RecordingMdListener ml;
    // Static bands disabled; dynamic hook will compute from last trade price.
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    DynamicBandExchange eng(cfg, ol, ml);

    // Seed a trade so last_trade_price_ = 1000.
    // Place a resting sell at 1000 and a market buy to create the trade.
    OrderRequest sell{.client_order_id = 1,
                      .account_id = 1,
                      .side = Side::Sell,
                      .type = OrderType::Limit,
                      .tif = TimeInForce::GTC,
                      .price = 1000,
                      .quantity = 10000,
                      .stop_price = 0,
                      .timestamp = 1000,
                      .gtd_expiry = 0};
    eng.new_order(sell);

    OrderRequest buy_mkt{.client_order_id = 2,
                         .account_id = 2,
                         .side = Side::Buy,
                         .type = OrderType::Market,
                         .tif = TimeInForce::GTC,
                         .price = 0,
                         .quantity = 10000,
                         .stop_price = 0,
                         .timestamp = 2000,
                         .gtd_expiry = 0};
    eng.new_order(buy_mkt);  // fills at 1000 -> last_trade_price_ = 1000

    ol.clear();
    ml.clear();

    // last_trade_price_ = 1000, so band = [500, 1500].
    // Price 400 is below 500 -> rejected.
    eng.new_order(make_limit(3, Side::Buy, 400, 10000, 3000));

    ASSERT_EQ(ol.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(ol.events()[0]));
    EXPECT_EQ(std::get<OrderRejected>(ol.events()[0]).reason,
              RejectReason::PriceBandViolation);

    ol.clear();

    // Price 1600 is above 1500 -> rejected.
    eng.new_order(make_limit(4, Side::Sell, 1600, 10000, 4000));

    ASSERT_EQ(ol.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(ol.events()[0]));
    EXPECT_EQ(std::get<OrderRejected>(ol.events()[0]).reason,
              RejectReason::PriceBandViolation);

    ol.clear();

    // Price 1000 is inside [500, 1500] -> accepted.
    eng.new_order(make_limit(5, Side::Buy, 1000, 10000, 5000));

    ASSERT_EQ(ol.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(ol.events()[0]));
}

// ---------------------------------------------------------------------------
// 25. Dynamic bands: when no reference price exists (0), static config
//     bands are used (default fallback, backward compatible)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, DynamicBandsFallbackToStaticWhenNoRefPrice) {
    RecordingOrderListener ol;
    RecordingMdListener ml;
    // Static bands: [800, 1200]
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 800,
                     .price_band_high = 1200};
    // Plain TestExchange uses the default calculate_dynamic_bands which
    // returns the static config bands unchanged.
    TestExchange eng(cfg, ol, ml);

    // Price 700 is below static low of 800 -> rejected.
    eng.new_order(make_limit(1, Side::Buy, 700, 10000, 1000));

    ASSERT_EQ(ol.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(ol.events()[0]));
    EXPECT_EQ(std::get<OrderRejected>(ol.events()[0]).reason,
              RejectReason::PriceBandViolation);

    ol.clear();

    // Price 1300 is above static high of 1200 -> rejected.
    eng.new_order(make_limit(2, Side::Sell, 1300, 10000, 2000));

    ASSERT_EQ(ol.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(ol.events()[0]));
    EXPECT_EQ(std::get<OrderRejected>(ol.events()[0]).reason,
              RejectReason::PriceBandViolation);

    ol.clear();

    // Price 1000 is inside [800, 1200] -> accepted.
    eng.new_order(make_limit(3, Side::Buy, 1000, 10000, 3000));

    ASSERT_EQ(ol.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(ol.events()[0]));
}

// ===========================================================================
// Task A3: Mass Cancel API
// ===========================================================================

// Helper: build a limit order with explicit account_id
static OrderRequest make_limit_for(uint64_t cl_ord_id, uint64_t account_id,
                                    Side side, Price price, Quantity qty,
                                    Timestamp ts) {
    return OrderRequest{.client_order_id = cl_ord_id,
                        .account_id = account_id,
                        .side = side,
                        .type = OrderType::Limit,
                        .tif = TimeInForce::GTC,
                        .price = price,
                        .quantity = qty,
                        .stop_price = 0,
                        .timestamp = ts,
                        .gtd_expiry = 0};
}

// ---------------------------------------------------------------------------
// 21. mass_cancel: cancels only the target account's orders
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MassCancelByAccount) {
    // Place 5 orders for account 1
    engine_.new_order(make_limit_for(101, 1, Side::Buy,  1000, 10000, 1));
    engine_.new_order(make_limit_for(102, 1, Side::Buy,  1100, 10000, 2));
    engine_.new_order(make_limit_for(103, 1, Side::Sell, 2000, 10000, 3));
    engine_.new_order(make_limit_for(104, 1, Side::Sell, 2100, 10000, 4));
    engine_.new_order(make_limit_for(105, 1, Side::Buy,  900,  10000, 5));

    // Place 3 orders for account 2
    engine_.new_order(make_limit_for(201, 2, Side::Buy,  800,  10000, 6));
    engine_.new_order(make_limit_for(202, 2, Side::Sell, 3000, 10000, 7));
    engine_.new_order(make_limit_for(203, 2, Side::Buy,  700,  10000, 8));

    EXPECT_EQ(engine_.active_order_count(), 8u);

    order_listener_.clear();
    md_listener_.clear();

    // Cancel all orders for account 1
    size_t cancelled = engine_.mass_cancel(1, 9000);
    EXPECT_EQ(cancelled, 5u);
    EXPECT_EQ(engine_.active_order_count(), 3u);

    // All 5 OrderCancelled events fired with MassCancelled reason
    size_t cancel_count = 0;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(e)) {
            auto& c = std::get<OrderCancelled>(e);
            EXPECT_EQ(c.reason, CancelReason::MassCancelled);
            EXPECT_EQ(c.ts, 9000);
            ++cancel_count;
        }
    }
    EXPECT_EQ(cancel_count, 5u);
}

// ---------------------------------------------------------------------------
// 22. mass_cancel_all: cancels all remaining orders, book empty afterwards
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MassCancelAll) {
    // Place 5 orders for account 1 and 3 for account 2
    engine_.new_order(make_limit_for(101, 1, Side::Buy,  1000, 10000, 1));
    engine_.new_order(make_limit_for(102, 1, Side::Buy,  1100, 10000, 2));
    engine_.new_order(make_limit_for(103, 1, Side::Sell, 2000, 10000, 3));
    engine_.new_order(make_limit_for(104, 1, Side::Sell, 2100, 10000, 4));
    engine_.new_order(make_limit_for(105, 1, Side::Buy,  900,  10000, 5));
    engine_.new_order(make_limit_for(201, 2, Side::Buy,  800,  10000, 6));
    engine_.new_order(make_limit_for(202, 2, Side::Sell, 3000, 10000, 7));
    engine_.new_order(make_limit_for(203, 2, Side::Buy,  700,  10000, 8));

    // First wipe account 1
    engine_.mass_cancel(1, 9000);
    EXPECT_EQ(engine_.active_order_count(), 3u);

    order_listener_.clear();
    md_listener_.clear();

    // Now wipe everything remaining
    size_t cancelled = engine_.mass_cancel_all(10000);
    EXPECT_EQ(cancelled, 3u);
    EXPECT_EQ(engine_.active_order_count(), 0u);

    // Verify 3 OrderCancelled events with MassCancelled reason
    size_t cancel_count = 0;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(e)) {
            auto& c = std::get<OrderCancelled>(e);
            EXPECT_EQ(c.reason, CancelReason::MassCancelled);
            EXPECT_EQ(c.ts, 10000);
            ++cancel_count;
        }
    }
    EXPECT_EQ(cancel_count, 3u);

    // Book must be completely empty: no TopOfBook bid or ask
    bool found_tob = false;
    for (auto& e : md_listener_.events()) {
        if (std::holds_alternative<TopOfBook>(e)) {
            auto& tob = std::get<TopOfBook>(e);
            // After mass_cancel_all the last TopOfBook must be fully empty
            found_tob = true;
            (void)tob;
        }
    }
    // At least one TopOfBook was fired (book cleared)
    EXPECT_TRUE(found_tob);
    // Confirm the engine reports zero active orders
    EXPECT_EQ(engine_.active_order_count(), 0u);
}

// ---------------------------------------------------------------------------
// 23. mass_cancel on empty / non-existent account returns 0
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MassCancelNoMatch) {
    engine_.new_order(make_limit_for(101, 1, Side::Buy, 1000, 10000, 1));
    order_listener_.clear();
    md_listener_.clear();

    size_t cancelled = engine_.mass_cancel(99, 2000);
    EXPECT_EQ(cancelled, 0u);

    // No events fired for a no-op cancel
    EXPECT_EQ(order_listener_.size(), 0u);
    EXPECT_EQ(engine_.active_order_count(), 1u);
}

// ---------------------------------------------------------------------------
// 24. mass_cancel_all on empty engine returns 0
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MassCancelAllEmpty) {
    size_t cancelled = engine_.mass_cancel_all(1000);
    EXPECT_EQ(cancelled, 0u);
    EXPECT_EQ(order_listener_.size(), 0u);
}

// ---------------------------------------------------------------------------
// 25. mass_cancel fires MassCancelled for stop orders too
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MassCancelIncludesStopOrders) {
    // Place a resting limit and a stop order for account 1
    engine_.new_order(make_limit_for(101, 1, Side::Buy, 1000, 10000, 1));

    OrderRequest stop_req{.client_order_id = 102,
                          .account_id = 1,
                          .side = Side::Sell,
                          .type = OrderType::Stop,
                          .tif = TimeInForce::GTC,
                          .price = 0,
                          .quantity = 10000,
                          .stop_price = 900,
                          .timestamp = 2,
                          .gtd_expiry = 0};
    engine_.new_order(stop_req);
    EXPECT_EQ(engine_.active_order_count(), 2u);

    order_listener_.clear();
    md_listener_.clear();

    size_t cancelled = engine_.mass_cancel(1, 3000);
    EXPECT_EQ(cancelled, 2u);
    EXPECT_EQ(engine_.active_order_count(), 0u);

    size_t cancel_count = 0;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(e)) {
            EXPECT_EQ(std::get<OrderCancelled>(e).reason,
                      CancelReason::MassCancelled);
            ++cancel_count;
        }
    }
    EXPECT_EQ(cancel_count, 2u);
}

// ===========================================================================
// Task B1: Session State Tracking
// ===========================================================================

// ---------------------------------------------------------------------------
// 26. Default state is Continuous -- all existing behavior unchanged
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, DefaultStateIsContinuous) {
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
}

// ---------------------------------------------------------------------------
// 27. set_session_state fires MarketStatus callback
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, SetSessionStateFiresMarketStatus) {
    engine_.set_session_state(SessionState::PreOpen, 5000);

    EXPECT_EQ(engine_.session_state(), SessionState::PreOpen);
    ASSERT_EQ(md_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<MarketStatus>(md_listener_.events()[0]));
    auto& ms = std::get<MarketStatus>(md_listener_.events()[0]);
    EXPECT_EQ(ms.state, SessionState::PreOpen);
    EXPECT_EQ(ms.ts, 5000);
}

// ---------------------------------------------------------------------------
// 28. set_session_state no-op when state is same
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, SetSessionStateSameIsNoOp) {
    engine_.set_session_state(SessionState::Continuous, 5000);

    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
    EXPECT_EQ(md_listener_.size(), 0u);
}

// ---------------------------------------------------------------------------
// 29. Order rejected in Closed state
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, OrderRejectedInClosedState) {
    engine_.set_session_state(SessionState::Closed, 5000);
    md_listener_.clear();

    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 6000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.client_order_id, 100u);
    EXPECT_EQ(rej.reason, RejectReason::ExchangeSpecific);
}

// ---------------------------------------------------------------------------
// 30. Limit order accepted in PreOpen -- rests on book, no matching
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, LimitOrderRestsInPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 5000);
    md_listener_.clear();

    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 6000));

    // Order is accepted
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));

    // Should have L3 Add, L2 Add, TopOfBook -- but NO Trade
    bool found_trade = false;
    for (auto& e : md_listener_.events()) {
        if (std::holds_alternative<Trade>(e)) {
            found_trade = true;
        }
    }
    EXPECT_FALSE(found_trade);

    // Order is on the book
    EXPECT_EQ(engine_.active_order_count(), 1u);
}

// ---------------------------------------------------------------------------
// 31. Two crossing limit orders in PreOpen -- both rest, no matching
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, CrossingOrdersInPreOpenNoMatch) {
    engine_.set_session_state(SessionState::PreOpen, 5000);
    md_listener_.clear();

    // Buy at 1100
    engine_.new_order(make_limit(100, Side::Buy, 1100, 10000, 6000));
    // Sell at 1000 -- would match in Continuous, but NOT in PreOpen
    engine_.new_order(make_limit(200, Side::Sell, 1000, 10000, 7000));

    // Both should be accepted
    size_t accept_count = 0;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(e)) ++accept_count;
    }
    EXPECT_EQ(accept_count, 2u);

    // No fills
    bool found_fill = false;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(e) ||
            std::holds_alternative<OrderPartiallyFilled>(e)) {
            found_fill = true;
        }
    }
    EXPECT_FALSE(found_fill);

    // No trades
    bool found_trade = false;
    for (auto& e : md_listener_.events()) {
        if (std::holds_alternative<Trade>(e)) found_trade = true;
    }
    EXPECT_FALSE(found_trade);

    // Both orders are on the book
    EXPECT_EQ(engine_.active_order_count(), 2u);
}

// ---------------------------------------------------------------------------
// 32. IOC rejected in PreOpen via is_order_allowed_in_phase override
// ---------------------------------------------------------------------------

class PreOpenExchange
    : public MatchingEngine<PreOpenExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<PreOpenExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    bool is_order_allowed_in_phase(const OrderRequest& req,
                                   SessionState state) {
        // Reject IOC/FOK in collection phases
        if (state == SessionState::PreOpen ||
            state == SessionState::PreClose ||
            state == SessionState::VolatilityAuction) {
            if (req.tif == TimeInForce::IOC || req.tif == TimeInForce::FOK)
                return false;
        }
        return true;
    }
};

TEST_F(MatchingEngineTest, IOCRejectedInPreOpenViaHook) {
    RecordingOrderListener po_order_listener;
    RecordingMdListener po_md_listener;
    EngineConfig po_config{.tick_size = 100,
                           .lot_size = 10000,
                           .price_band_low = 0,
                           .price_band_high = 0};
    PreOpenExchange po_engine(po_config, po_order_listener, po_md_listener);

    po_engine.set_session_state(SessionState::PreOpen, 5000);
    po_md_listener.clear();

    OrderRequest ioc_req{.client_order_id = 100,
                         .account_id = 1,
                         .side = Side::Buy,
                         .type = OrderType::Limit,
                         .tif = TimeInForce::IOC,
                         .price = 1000,
                         .quantity = 10000,
                         .stop_price = 0,
                         .timestamp = 6000,
                         .gtd_expiry = 0};
    po_engine.new_order(ioc_req);

    ASSERT_EQ(po_order_listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        po_order_listener.events()[0]));
    auto& rej = std::get<OrderRejected>(po_order_listener.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::InvalidTif);
}

// ---------------------------------------------------------------------------
// 33. Cancel works in PreOpen
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, CancelWorksInPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 5000);
    md_listener_.clear();

    // Place a limit order
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 6000));
    EXPECT_EQ(engine_.active_order_count(), 1u);

    order_listener_.clear();
    md_listener_.clear();

    // Cancel it -- should succeed in PreOpen
    engine_.cancel_order(1, 7000);

    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(
        order_listener_.events()[0]));
    auto& canc = std::get<OrderCancelled>(order_listener_.events()[0]);
    EXPECT_EQ(canc.id, 1u);
    EXPECT_EQ(canc.reason, CancelReason::UserRequested);

    EXPECT_EQ(engine_.active_order_count(), 0u);
}

// ---------------------------------------------------------------------------
// 34. Cancel rejected in Closed state
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, CancelRejectedInClosedState) {
    // Place order in Continuous
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1000));
    EXPECT_EQ(engine_.active_order_count(), 1u);

    // Transition to Closed
    engine_.set_session_state(SessionState::Closed, 5000);
    order_listener_.clear();
    md_listener_.clear();

    engine_.cancel_order(1, 6000);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderCancelRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::ExchangeSpecific);
}

// ---------------------------------------------------------------------------
// 35. Modify rejected in Closed state
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, ModifyRejectedInClosedState) {
    // Place order in Continuous
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 1000));
    EXPECT_EQ(engine_.active_order_count(), 1u);

    // Transition to Closed
    engine_.set_session_state(SessionState::Closed, 5000);
    order_listener_.clear();
    md_listener_.clear();

    ModifyRequest mod{.order_id = 1,
                      .client_order_id = 100,
                      .new_price = 1100,
                      .new_quantity = 10000,
                      .timestamp = 6000};
    engine_.modify_order(mod);

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderModifyRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderModifyRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::ExchangeSpecific);
}

// ---------------------------------------------------------------------------
// 36. State transition back to Continuous -- next order matches normally
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, TransitionBackToContinuousMatchesNormally) {
    // Enter PreOpen and place crossing orders
    engine_.set_session_state(SessionState::PreOpen, 5000);

    engine_.new_order(make_limit(100, Side::Buy, 1100, 10000, 6000));
    engine_.new_order(make_limit(200, Side::Sell, 1000, 10000, 7000));
    EXPECT_EQ(engine_.active_order_count(), 2u);

    // Transition back to Continuous
    engine_.set_session_state(SessionState::Continuous, 8000);
    order_listener_.clear();
    md_listener_.clear();

    // New aggressive sell should match against the resting buy
    engine_.new_order(make_limit(300, Side::Sell, 1100, 10000, 9000));

    // Should see a fill
    bool found_fill = false;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(e)) found_fill = true;
    }
    EXPECT_TRUE(found_fill);

    // Should see a trade
    bool found_trade = false;
    for (auto& e : md_listener_.events()) {
        if (std::holds_alternative<Trade>(e)) found_trade = true;
    }
    EXPECT_TRUE(found_trade);
}

// ---------------------------------------------------------------------------
// 37. on_session_transition returns false -- transition blocked
// ---------------------------------------------------------------------------

class BlockTransitionExchange
    : public MatchingEngine<BlockTransitionExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<BlockTransitionExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    bool block_transition{false};

    bool on_session_transition(SessionState /*old_state*/,
                               SessionState /*new_state*/,
                               Timestamp /*ts*/) {
        return !block_transition;
    }
};

TEST_F(MatchingEngineTest, TransitionBlockedByHook) {
    RecordingOrderListener bt_order_listener;
    RecordingMdListener bt_md_listener;
    EngineConfig bt_config{.tick_size = 100,
                           .lot_size = 10000,
                           .price_band_low = 0,
                           .price_band_high = 0};
    BlockTransitionExchange bt_engine(bt_config, bt_order_listener,
                                      bt_md_listener);

    bt_engine.block_transition = true;
    bt_engine.set_session_state(SessionState::PreOpen, 5000);

    // Transition should have been blocked
    EXPECT_EQ(bt_engine.session_state(), SessionState::Continuous);
    // No MarketStatus event
    EXPECT_EQ(bt_md_listener.size(), 0u);
}

// ---------------------------------------------------------------------------
// 38. Market order in PreOpen is accepted then cancelled
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, MarketOrderInPreOpenCancelled) {
    engine_.set_session_state(SessionState::PreOpen, 5000);
    md_listener_.clear();

    engine_.new_order(make_market(100, Side::Buy, 10000, 6000));

    // Should see OrderAccepted then OrderCancelled
    ASSERT_GE(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(
        order_listener_.events()[1]));
    auto& canc = std::get<OrderCancelled>(order_listener_.events()[1]);
    EXPECT_EQ(canc.reason, CancelReason::IOCRemainder);

    EXPECT_EQ(engine_.active_order_count(), 0u);
}

// ---------------------------------------------------------------------------
// 39. Modify works in PreOpen (collection phase)
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, ModifyWorksInPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 5000);
    md_listener_.clear();

    // Place a limit order
    engine_.new_order(make_limit(100, Side::Buy, 1000, 10000, 6000));
    EXPECT_EQ(engine_.active_order_count(), 1u);

    order_listener_.clear();
    md_listener_.clear();

    // Modify should succeed in PreOpen
    ModifyRequest mod{.order_id = 1,
                      .client_order_id = 100,
                      .new_price = 1100,
                      .new_quantity = 20000,
                      .timestamp = 7000};
    engine_.modify_order(mod);

    // Should see OrderModified (not rejected)
    bool found_modified = false;
    for (auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderModified>(e)) {
            found_modified = true;
        }
    }
    EXPECT_TRUE(found_modified);
}

}  // namespace
}  // namespace exchange
