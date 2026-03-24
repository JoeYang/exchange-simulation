#include "test-harness/recorded_event.h"

#include <gtest/gtest.h>

#include <string>

namespace exchange {
namespace {

// --- Construction from each event type ---

TEST(RecordedEventTest, ConstructFromOrderAccepted) {
    OrderAccepted e{.id = 1, .client_order_id = 100, .ts = 1000};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 0u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderRejected) {
    OrderRejected e{.client_order_id = 2, .ts = 1001, .reason = RejectReason::InvalidPrice};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderFilled) {
    OrderFilled e{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderFilled>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderPartiallyFilled) {
    OrderPartiallyFilled e{
        .aggressor_id = 3, .resting_id = 2,
        .price = 500000, .quantity = 5000,
        .aggressor_remaining = 3000, .resting_remaining = 2000,
        .ts = 7000
    };
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 3u);
    EXPECT_TRUE(std::holds_alternative<OrderPartiallyFilled>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderCancelled) {
    OrderCancelled e{.id = 10, .ts = 6000, .reason = CancelReason::UserRequested};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 4u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderCancelRejected) {
    OrderCancelRejected e{.id = 7, .client_order_id = 77, .ts = 8000, .reason = RejectReason::UnknownOrder};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 5u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelRejected>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderModified) {
    OrderModified e{.id = 4, .client_order_id = 40, .new_price = 990000, .new_qty = 20000, .ts = 4000};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 6u);
    EXPECT_TRUE(std::holds_alternative<OrderModified>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderModifyRejected) {
    OrderModifyRejected e{.id = 5, .client_order_id = 50, .ts = 9000, .reason = RejectReason::UnknownOrder};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 7u);
    EXPECT_TRUE(std::holds_alternative<OrderModifyRejected>(ev));
}

TEST(RecordedEventTest, ConstructFromTopOfBook) {
    TopOfBook e{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 8u);
    EXPECT_TRUE(std::holds_alternative<TopOfBook>(ev));
}

TEST(RecordedEventTest, ConstructFromDepthUpdate) {
    DepthUpdate e{.side = Side::Buy, .price = 1000, .total_qty = 500, .order_count = 3,
                  .action = DepthUpdate::Add, .ts = 100};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 9u);
    EXPECT_TRUE(std::holds_alternative<DepthUpdate>(ev));
}

TEST(RecordedEventTest, ConstructFromOrderBookAction) {
    OrderBookAction e{.id = 1, .side = Side::Buy, .price = 1005000, .qty = 10000,
                      .action = OrderBookAction::Add, .ts = 1000};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 10u);
    EXPECT_TRUE(std::holds_alternative<OrderBookAction>(ev));
}

TEST(RecordedEventTest, ConstructFromTrade) {
    Trade e{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 2000};
    RecordedEvent ev = e;
    EXPECT_EQ(ev.index(), 11u);
    EXPECT_TRUE(std::holds_alternative<Trade>(ev));
}

// --- Equality: same type, same values ---

TEST(RecordedEventTest, EqualityOrderAccepted) {
    RecordedEvent a = OrderAccepted{.id = 1, .client_order_id = 100, .ts = 1000};
    RecordedEvent b = OrderAccepted{.id = 1, .client_order_id = 100, .ts = 1000};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderRejected) {
    RecordedEvent a = OrderRejected{.client_order_id = 2, .ts = 500, .reason = RejectReason::InvalidPrice};
    RecordedEvent b = OrderRejected{.client_order_id = 2, .ts = 500, .reason = RejectReason::InvalidPrice};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderFilled) {
    RecordedEvent a = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    RecordedEvent b = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderPartiallyFilled) {
    OrderPartiallyFilled e{
        .aggressor_id = 3, .resting_id = 2,
        .price = 500000, .quantity = 5000,
        .aggressor_remaining = 3000, .resting_remaining = 2000,
        .ts = 7000
    };
    RecordedEvent a = e;
    RecordedEvent b = e;
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderCancelled) {
    RecordedEvent a = OrderCancelled{.id = 10, .ts = 6000, .reason = CancelReason::UserRequested};
    RecordedEvent b = OrderCancelled{.id = 10, .ts = 6000, .reason = CancelReason::UserRequested};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderCancelRejected) {
    RecordedEvent a = OrderCancelRejected{.id = 7, .client_order_id = 77, .ts = 8000, .reason = RejectReason::UnknownOrder};
    RecordedEvent b = OrderCancelRejected{.id = 7, .client_order_id = 77, .ts = 8000, .reason = RejectReason::UnknownOrder};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderModified) {
    RecordedEvent a = OrderModified{.id = 4, .client_order_id = 40, .new_price = 990000, .new_qty = 20000, .ts = 4000};
    RecordedEvent b = OrderModified{.id = 4, .client_order_id = 40, .new_price = 990000, .new_qty = 20000, .ts = 4000};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderModifyRejected) {
    RecordedEvent a = OrderModifyRejected{.id = 5, .client_order_id = 50, .ts = 9000, .reason = RejectReason::UnknownOrder};
    RecordedEvent b = OrderModifyRejected{.id = 5, .client_order_id = 50, .ts = 9000, .reason = RejectReason::UnknownOrder};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityTopOfBook) {
    RecordedEvent a = TopOfBook{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    RecordedEvent b = TopOfBook{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityDepthUpdate) {
    RecordedEvent a = DepthUpdate{.side = Side::Buy, .price = 1000, .total_qty = 500,
                                  .order_count = 3, .action = DepthUpdate::Add, .ts = 100};
    RecordedEvent b = DepthUpdate{.side = Side::Buy, .price = 1000, .total_qty = 500,
                                  .order_count = 3, .action = DepthUpdate::Add, .ts = 100};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityOrderBookAction) {
    RecordedEvent a = OrderBookAction{.id = 1, .side = Side::Buy, .price = 1005000, .qty = 10000,
                                      .action = OrderBookAction::Add, .ts = 1000};
    RecordedEvent b = OrderBookAction{.id = 1, .side = Side::Buy, .price = 1005000, .qty = 10000,
                                      .action = OrderBookAction::Add, .ts = 1000};
    EXPECT_EQ(a, b);
}

TEST(RecordedEventTest, EqualityTrade) {
    RecordedEvent a = Trade{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
                            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 2000};
    RecordedEvent b = Trade{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
                            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 2000};
    EXPECT_EQ(a, b);
}

// --- Inequality: same type, different field values ---

TEST(RecordedEventTest, InequalityDifferentFieldValues) {
    RecordedEvent a = OrderAccepted{.id = 1, .client_order_id = 100, .ts = 1000};
    RecordedEvent b = OrderAccepted{.id = 2, .client_order_id = 100, .ts = 1000};
    EXPECT_NE(a, b);
}

TEST(RecordedEventTest, InequalityOrderFilledDifferentPrice) {
    RecordedEvent a = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    RecordedEvent b = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1006000, .quantity = 10000, .ts = 2000};
    EXPECT_NE(a, b);
}

TEST(RecordedEventTest, InequalityOrderCancelledDifferentReason) {
    RecordedEvent a = OrderCancelled{.id = 1, .ts = 1000, .reason = CancelReason::UserRequested};
    RecordedEvent b = OrderCancelled{.id = 1, .ts = 1000, .reason = CancelReason::IOCRemainder};
    EXPECT_NE(a, b);
}

TEST(RecordedEventTest, InequalityDepthUpdateDifferentAction) {
    RecordedEvent a = DepthUpdate{.side = Side::Buy, .price = 1000, .total_qty = 500,
                                  .order_count = 3, .action = DepthUpdate::Add, .ts = 100};
    RecordedEvent b = DepthUpdate{.side = Side::Buy, .price = 1000, .total_qty = 500,
                                  .order_count = 3, .action = DepthUpdate::Remove, .ts = 100};
    EXPECT_NE(a, b);
}

// --- Inequality: different types ---

TEST(RecordedEventTest, InequalityOnDifferentTypes) {
    RecordedEvent a = OrderAccepted{.id = 1, .client_order_id = 100, .ts = 1000};
    RecordedEvent b = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    EXPECT_NE(a, b);
}

TEST(RecordedEventTest, InequalityTopOfBookVsDepthUpdate) {
    RecordedEvent a = TopOfBook{.best_bid = 1000, .bid_qty = 100, .best_ask = 0, .ask_qty = 0, .ts = 100};
    RecordedEvent b = DepthUpdate{.side = Side::Buy, .price = 1000, .total_qty = 100,
                                  .order_count = 1, .action = DepthUpdate::Add, .ts = 100};
    EXPECT_NE(a, b);
}

// --- to_string: non-empty for all types ---

TEST(RecordedEventTest, ToStringOrderAccepted) {
    RecordedEvent ev = OrderAccepted{.id = 1, .client_order_id = 100, .ts = 1000};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderAccepted"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderRejected) {
    RecordedEvent ev = OrderRejected{.client_order_id = 2, .ts = 500, .reason = RejectReason::InvalidPrice};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderRejected"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderFilled) {
    RecordedEvent ev = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderFilled"), std::string::npos);
    // Verify key field values appear in output
    EXPECT_NE(s.find("2"), std::string::npos);
    EXPECT_NE(s.find("1005000"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderPartiallyFilled) {
    RecordedEvent ev = OrderPartiallyFilled{
        .aggressor_id = 3, .resting_id = 2,
        .price = 500000, .quantity = 5000,
        .aggressor_remaining = 3000, .resting_remaining = 2000,
        .ts = 7000
    };
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderPartiallyFilled"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderCancelled) {
    RecordedEvent ev = OrderCancelled{.id = 10, .ts = 6000, .reason = CancelReason::UserRequested};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderCancelled"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderCancelRejected) {
    RecordedEvent ev = OrderCancelRejected{.id = 7, .client_order_id = 77, .ts = 8000,
                                           .reason = RejectReason::UnknownOrder};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderCancelRejected"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderModified) {
    RecordedEvent ev = OrderModified{.id = 4, .client_order_id = 40, .new_price = 990000, .new_qty = 20000, .ts = 4000};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderModified"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderModifyRejected) {
    RecordedEvent ev = OrderModifyRejected{.id = 5, .client_order_id = 50, .ts = 9000,
                                           .reason = RejectReason::UnknownOrder};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderModifyRejected"), std::string::npos);
}

TEST(RecordedEventTest, ToStringTopOfBook) {
    RecordedEvent ev = TopOfBook{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("TopOfBook"), std::string::npos);
}

TEST(RecordedEventTest, ToStringDepthUpdate) {
    RecordedEvent ev = DepthUpdate{.side = Side::Buy, .price = 1000, .total_qty = 500,
                                   .order_count = 3, .action = DepthUpdate::Add, .ts = 100};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("DepthUpdate"), std::string::npos);
}

TEST(RecordedEventTest, ToStringOrderBookAction) {
    RecordedEvent ev = OrderBookAction{.id = 1, .side = Side::Buy, .price = 1005000, .qty = 10000,
                                       .action = OrderBookAction::Add, .ts = 1000};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("OrderBookAction"), std::string::npos);
}

TEST(RecordedEventTest, ToStringTrade) {
    RecordedEvent ev = Trade{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
                             .resting_id = 1, .aggressor_side = Side::Sell, .ts = 2000};
    std::string s = to_string(ev);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("Trade"), std::string::npos);
    // Verify the example format from the spec: Trade{aggressor=2, resting=1, price=1005000, ...}
    EXPECT_NE(s.find("1005000"), std::string::npos);
    EXPECT_NE(s.find("10000"), std::string::npos);
}

// --- to_string: content spot-checks ---

TEST(RecordedEventTest, ToStringOrderFilledFormat) {
    // Spec example: OrderFilled{aggressor=2, resting=1, price=1005000, qty=10000, ts=2000}
    RecordedEvent ev = OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    std::string s = to_string(ev);
    EXPECT_NE(s.find("aggressor=2"), std::string::npos);
    EXPECT_NE(s.find("resting=1"), std::string::npos);
    EXPECT_NE(s.find("price=1005000"), std::string::npos);
    EXPECT_NE(s.find("qty=10000"), std::string::npos);
    EXPECT_NE(s.find("ts=2000"), std::string::npos);
}

TEST(RecordedEventTest, ToStringTopOfBookFormat) {
    RecordedEvent ev = TopOfBook{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    std::string s = to_string(ev);
    EXPECT_NE(s.find("bid=999000"), std::string::npos);
    EXPECT_NE(s.find("bid_qty=10000"), std::string::npos);
    EXPECT_NE(s.find("ask=1001000"), std::string::npos);
    EXPECT_NE(s.find("ask_qty=5000"), std::string::npos);
    EXPECT_NE(s.find("ts=500"), std::string::npos);
}

}  // namespace
}  // namespace exchange
