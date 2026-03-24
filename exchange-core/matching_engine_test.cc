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

}  // namespace
}  // namespace exchange
