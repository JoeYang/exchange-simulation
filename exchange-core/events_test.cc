#include "exchange-core/events.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// --- Default construction (zero/default fields) ---

TEST(EventsTest, OrderAcceptedDefaultConstruct) {
    OrderAccepted e{};
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.client_order_id, 0u);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderRejectedDefaultConstruct) {
    OrderRejected e{};
    EXPECT_EQ(e.client_order_id, 0u);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderFilledDefaultConstruct) {
    OrderFilled e{};
    EXPECT_EQ(e.aggressor_id, 0u);
    EXPECT_EQ(e.resting_id, 0u);
    EXPECT_EQ(e.price, 0);
    EXPECT_EQ(e.quantity, 0);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderPartiallyFilledDefaultConstruct) {
    OrderPartiallyFilled e{};
    EXPECT_EQ(e.aggressor_remaining, 0);
    EXPECT_EQ(e.resting_remaining, 0);
}

TEST(EventsTest, OrderCancelledDefaultConstruct) {
    OrderCancelled e{};
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderCancelRejectedDefaultConstruct) {
    OrderCancelRejected e{};
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.client_order_id, 0u);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderModifiedDefaultConstruct) {
    OrderModified e{};
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.new_price, 0);
    EXPECT_EQ(e.new_qty, 0);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderModifyRejectedDefaultConstruct) {
    OrderModifyRejected e{};
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, TopOfBookDefaultConstruct) {
    TopOfBook e{};
    EXPECT_EQ(e.best_bid, 0);
    EXPECT_EQ(e.bid_qty, 0);
    EXPECT_EQ(e.best_ask, 0);
    EXPECT_EQ(e.ask_qty, 0);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, DepthUpdateDefaultConstruct) {
    DepthUpdate e{};
    EXPECT_EQ(e.price, 0);
    EXPECT_EQ(e.total_qty, 0);
    EXPECT_EQ(e.order_count, 0u);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, OrderBookActionDefaultConstruct) {
    OrderBookAction e{};
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.price, 0);
    EXPECT_EQ(e.qty, 0);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, TradeDefaultConstruct) {
    Trade e{};
    EXPECT_EQ(e.price, 0);
    EXPECT_EQ(e.quantity, 0);
    EXPECT_EQ(e.aggressor_id, 0u);
    EXPECT_EQ(e.resting_id, 0u);
    EXPECT_EQ(e.ts, 0);
}

// --- Assigned non-zero values ---

TEST(EventsTest, OrderAcceptedConstruction) {
    OrderAccepted e{.id = 1, .client_order_id = 100, .ts = 5000};
    EXPECT_EQ(e.id, 1u);
    EXPECT_EQ(e.client_order_id, 100u);
    EXPECT_EQ(e.ts, 5000);
}

TEST(EventsTest, OrderRejectedConstruction) {
    OrderRejected e{.client_order_id = 1, .ts = 1000, .reason = RejectReason::InvalidPrice};
    EXPECT_EQ(e.reason, RejectReason::InvalidPrice);
}

TEST(EventsTest, OrderFilledConstruction) {
    OrderFilled e{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    EXPECT_EQ(e.price, 1005000);
    EXPECT_EQ(e.quantity, 10000);
}

TEST(EventsTest, OrderPartiallyFilledConstruction) {
    OrderPartiallyFilled e{
        .aggressor_id = 3, .resting_id = 2,
        .price = 500000, .quantity = 5000,
        .aggressor_remaining = 3000, .resting_remaining = 2000,
        .ts = 7000
    };
    EXPECT_EQ(e.aggressor_remaining, 3000);
    EXPECT_EQ(e.resting_remaining, 2000);
}

TEST(EventsTest, OrderCancelledConstruction) {
    OrderCancelled e{.id = 10, .ts = 6000, .reason = CancelReason::UserRequested};
    EXPECT_EQ(e.id, 10u);
    EXPECT_EQ(e.reason, CancelReason::UserRequested);
}

TEST(EventsTest, OrderCancelRejectedConstruction) {
    OrderCancelRejected e{.id = 7, .client_order_id = 77, .ts = 8000, .reason = RejectReason::UnknownOrder};
    EXPECT_EQ(e.id, 7u);
    EXPECT_EQ(e.reason, RejectReason::UnknownOrder);
}

TEST(EventsTest, OrderModifiedConstruction) {
    OrderModified e{.id = 4, .client_order_id = 40, .new_price = 990000, .new_qty = 20000, .ts = 4000};
    EXPECT_EQ(e.new_price, 990000);
    EXPECT_EQ(e.new_qty, 20000);
}

TEST(EventsTest, OrderModifyRejectedConstruction) {
    OrderModifyRejected e{.id = 5, .client_order_id = 50, .ts = 9000,
                          .reason = RejectReason::UnknownOrder};
    EXPECT_EQ(e.reason, RejectReason::UnknownOrder);
}

TEST(EventsTest, TopOfBookConstruction) {
    TopOfBook e{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    EXPECT_EQ(e.best_bid, 999000);
    EXPECT_EQ(e.bid_qty, 10000);
    EXPECT_EQ(e.best_ask, 1001000);
    EXPECT_EQ(e.ask_qty, 5000);
    EXPECT_EQ(e.ts, 500);
}

TEST(EventsTest, DepthUpdateActions) {
    DepthUpdate e{.side = Side::Buy, .price = 1000, .total_qty = 500, .order_count = 3,
                  .action = DepthUpdate::Add, .ts = 100};
    EXPECT_EQ(e.action, DepthUpdate::Add);
    EXPECT_EQ(e.side, Side::Buy);
    EXPECT_EQ(e.order_count, 3u);
}

TEST(EventsTest, DepthUpdateRemoveAction) {
    DepthUpdate e{.side = Side::Sell, .price = 2000, .total_qty = 0, .order_count = 0,
                  .action = DepthUpdate::Remove, .ts = 200};
    EXPECT_EQ(e.action, DepthUpdate::Remove);
}

TEST(EventsTest, OrderBookActionValues) {
    OrderBookAction e{.id = 1, .side = Side::Sell, .price = 2000, .qty = 100,
                      .action = OrderBookAction::Fill, .ts = 200};
    EXPECT_EQ(e.action, OrderBookAction::Fill);
    EXPECT_EQ(e.side, Side::Sell);
}

TEST(EventsTest, OrderBookActionCancelModify) {
    OrderBookAction cancel{.id = 2, .side = Side::Buy, .price = 1000, .qty = 50,
                           .action = OrderBookAction::Cancel, .ts = 300};
    EXPECT_EQ(cancel.action, OrderBookAction::Cancel);

    OrderBookAction modify{.id = 3, .side = Side::Buy, .price = 1000, .qty = 75,
                           .action = OrderBookAction::Modify, .ts = 400};
    EXPECT_EQ(modify.action, OrderBookAction::Modify);
}

TEST(EventsTest, TradeConstruction) {
    Trade t{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 3000};
    EXPECT_EQ(t.price, 1005000);
    EXPECT_EQ(t.quantity, 10000);
    EXPECT_EQ(t.aggressor_id, 2u);
    EXPECT_EQ(t.resting_id, 1u);
    EXPECT_EQ(t.aggressor_side, Side::Sell);
    EXPECT_EQ(t.ts, 3000);
}

TEST(EventsTest, MarketStatusDefaultConstruct) {
    MarketStatus e{};
    EXPECT_EQ(e.state, SessionState::Closed);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, MarketStatusConstruction) {
    MarketStatus e{.state = SessionState::Continuous, .ts = 12345};
    EXPECT_EQ(e.state, SessionState::Continuous);
    EXPECT_EQ(e.ts, 12345);
}

TEST(EventsTest, MarketStatusAllStates) {
    // Verify each SessionState can be stored in a MarketStatus event.
    for (auto s : {
            SessionState::Closed, SessionState::PreOpen,
            SessionState::OpeningAuction, SessionState::Continuous,
            SessionState::PreClose, SessionState::ClosingAuction,
            SessionState::Halt, SessionState::VolatilityAuction}) {
        MarketStatus e{.state = s, .ts = 1000};
        EXPECT_EQ(e.state, s);
    }
}

TEST(EventsTest, IndicativePriceDefaultConstruct) {
    IndicativePrice e{};
    EXPECT_EQ(e.price, 0);
    EXPECT_EQ(e.matched_volume, 0);
    EXPECT_EQ(e.buy_surplus, 0);
    EXPECT_EQ(e.sell_surplus, 0);
    EXPECT_EQ(e.ts, 0);
}

TEST(EventsTest, IndicativePriceConstruction) {
    IndicativePrice e{
        .price = 1005000,
        .matched_volume = 50000,
        .buy_surplus = 10000,
        .sell_surplus = 0,
        .ts = 9999
    };
    EXPECT_EQ(e.price, 1005000);
    EXPECT_EQ(e.matched_volume, 50000);
    EXPECT_EQ(e.buy_surplus, 10000);
    EXPECT_EQ(e.sell_surplus, 0);
    EXPECT_EQ(e.ts, 9999);
}

}  // namespace
}  // namespace exchange
