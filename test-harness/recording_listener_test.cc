#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// --- RecordingOrderListener ---

TEST(RecordingOrderListenerTest, InitiallyEmpty) {
    RecordingOrderListener listener;
    EXPECT_EQ(listener.size(), 0u);
    EXPECT_TRUE(listener.events().empty());
}

TEST(RecordingOrderListenerTest, RecordOrderAccepted) {
    RecordingOrderListener listener;
    OrderAccepted e{.id = 1, .client_order_id = 100, .ts = 1000};
    listener.on_order_accepted(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderRejected) {
    RecordingOrderListener listener;
    OrderRejected e{.client_order_id = 2, .ts = 500, .reason = RejectReason::InvalidPrice};
    listener.on_order_rejected(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderFilled) {
    RecordingOrderListener listener;
    OrderFilled e{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    listener.on_order_filled(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderFilled>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderPartiallyFilled) {
    RecordingOrderListener listener;
    OrderPartiallyFilled e{
        .aggressor_id = 3, .resting_id = 2,
        .price = 500000, .quantity = 5000,
        .aggressor_remaining = 3000, .resting_remaining = 2000,
        .ts = 7000
    };
    listener.on_order_partially_filled(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderPartiallyFilled>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderCancelled) {
    RecordingOrderListener listener;
    OrderCancelled e{.id = 10, .ts = 6000, .reason = CancelReason::UserRequested};
    listener.on_order_cancelled(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderCancelRejected) {
    RecordingOrderListener listener;
    OrderCancelRejected e{.id = 7, .client_order_id = 77, .ts = 8000, .reason = RejectReason::UnknownOrder};
    listener.on_order_cancel_rejected(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderCancelRejected>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderModified) {
    RecordingOrderListener listener;
    OrderModified e{.id = 4, .client_order_id = 40, .new_price = 990000, .new_qty = 20000, .ts = 4000};
    listener.on_order_modified(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderModified>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, RecordOrderModifyRejected) {
    RecordingOrderListener listener;
    OrderModifyRejected e{.id = 5, .client_order_id = 50, .ts = 9000, .reason = RejectReason::UnknownOrder};
    listener.on_order_modify_rejected(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderModifyRejected>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, MultipleEventOrderPreserved) {
    RecordingOrderListener listener;
    OrderAccepted accepted{.id = 1, .client_order_id = 100, .ts = 1000};
    OrderFilled filled{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    OrderCancelled cancelled{.id = 3, .ts = 3000, .reason = CancelReason::UserRequested};

    listener.on_order_accepted(accepted);
    listener.on_order_filled(filled);
    listener.on_order_cancelled(cancelled);

    EXPECT_EQ(listener.size(), 3u);
    EXPECT_EQ(listener.events()[0], RecordedEvent{accepted});
    EXPECT_EQ(listener.events()[1], RecordedEvent{filled});
    EXPECT_EQ(listener.events()[2], RecordedEvent{cancelled});
}

TEST(RecordingOrderListenerTest, MixedTypesOrderPreserved) {
    RecordingOrderListener listener;
    OrderRejected rejected{.client_order_id = 1, .ts = 100, .reason = RejectReason::InvalidPrice};
    OrderModified modified{.id = 2, .client_order_id = 20, .new_price = 999000, .new_qty = 5000, .ts = 200};
    OrderModifyRejected mod_rejected{.id = 3, .client_order_id = 30, .ts = 300, .reason = RejectReason::UnknownOrder};

    listener.on_order_rejected(rejected);
    listener.on_order_modified(modified);
    listener.on_order_modify_rejected(mod_rejected);

    EXPECT_EQ(listener.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(listener.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderModified>(listener.events()[1]));
    EXPECT_TRUE(std::holds_alternative<OrderModifyRejected>(listener.events()[2]));
}

TEST(RecordingOrderListenerTest, ClearResetsToEmpty) {
    RecordingOrderListener listener;
    listener.on_order_accepted(OrderAccepted{.id = 1, .client_order_id = 10, .ts = 1000});
    listener.on_order_filled(OrderFilled{.aggressor_id = 2, .resting_id = 1, .price = 100, .quantity = 10, .ts = 2000});
    EXPECT_EQ(listener.size(), 2u);

    listener.clear();
    EXPECT_EQ(listener.size(), 0u);
    EXPECT_TRUE(listener.events().empty());
}

TEST(RecordingOrderListenerTest, RecordAfterClear) {
    RecordingOrderListener listener;
    listener.on_order_accepted(OrderAccepted{.id = 1, .client_order_id = 10, .ts = 1000});
    listener.clear();

    OrderCancelled e{.id = 2, .ts = 2000, .reason = CancelReason::IOCRemainder};
    listener.on_order_cancelled(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingOrderListenerTest, EventsReturnsByConstRef) {
    RecordingOrderListener listener;
    listener.on_order_accepted(OrderAccepted{.id = 1, .client_order_id = 10, .ts = 1000});
    const auto& evs = listener.events();
    EXPECT_EQ(evs.size(), 1u);
}

// --- RecordingMdListener ---

TEST(RecordingMdListenerTest, InitiallyEmpty) {
    RecordingMdListener listener;
    EXPECT_EQ(listener.size(), 0u);
    EXPECT_TRUE(listener.events().empty());
}

TEST(RecordingMdListenerTest, RecordTopOfBook) {
    RecordingMdListener listener;
    TopOfBook e{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 500};
    listener.on_top_of_book(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<TopOfBook>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingMdListenerTest, RecordDepthUpdate) {
    RecordingMdListener listener;
    DepthUpdate e{.side = Side::Buy, .price = 1000, .total_qty = 500,
                  .order_count = 3, .action = DepthUpdate::Add, .ts = 100};
    listener.on_depth_update(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<DepthUpdate>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingMdListenerTest, RecordOrderBookAction) {
    RecordingMdListener listener;
    OrderBookAction e{.id = 1, .side = Side::Buy, .price = 1005000, .qty = 10000,
                      .action = OrderBookAction::Add, .ts = 1000};
    listener.on_order_book_action(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderBookAction>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingMdListenerTest, RecordTrade) {
    RecordingMdListener listener;
    Trade e{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 2000};
    listener.on_trade(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<Trade>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingMdListenerTest, AllFourEventTypesOrderPreserved) {
    RecordingMdListener listener;
    TopOfBook tob{.best_bid = 999000, .bid_qty = 10000, .best_ask = 1001000, .ask_qty = 5000, .ts = 100};
    DepthUpdate du{.side = Side::Sell, .price = 1001000, .total_qty = 5000,
                   .order_count = 1, .action = DepthUpdate::Add, .ts = 200};
    OrderBookAction oba{.id = 5, .side = Side::Sell, .price = 1001000, .qty = 5000,
                        .action = OrderBookAction::Add, .ts = 300};
    Trade tr{.price = 1000000, .quantity = 2000, .aggressor_id = 6,
             .resting_id = 5, .aggressor_side = Side::Buy, .ts = 400};

    listener.on_top_of_book(tob);
    listener.on_depth_update(du);
    listener.on_order_book_action(oba);
    listener.on_trade(tr);

    EXPECT_EQ(listener.size(), 4u);
    EXPECT_EQ(listener.events()[0], RecordedEvent{tob});
    EXPECT_EQ(listener.events()[1], RecordedEvent{du});
    EXPECT_EQ(listener.events()[2], RecordedEvent{oba});
    EXPECT_EQ(listener.events()[3], RecordedEvent{tr});
}

TEST(RecordingMdListenerTest, ClearResetsToEmpty) {
    RecordingMdListener listener;
    listener.on_top_of_book(TopOfBook{.best_bid = 1000, .bid_qty = 100, .best_ask = 0, .ask_qty = 0, .ts = 1});
    listener.on_trade(Trade{.price = 1000, .quantity = 100, .aggressor_id = 1,
                            .resting_id = 2, .aggressor_side = Side::Buy, .ts = 2});
    EXPECT_EQ(listener.size(), 2u);

    listener.clear();
    EXPECT_EQ(listener.size(), 0u);
    EXPECT_TRUE(listener.events().empty());
}

TEST(RecordingMdListenerTest, RecordAfterClear) {
    RecordingMdListener listener;
    listener.on_top_of_book(TopOfBook{.best_bid = 1000, .bid_qty = 100, .best_ask = 0, .ask_qty = 0, .ts = 1});
    listener.clear();

    Trade e{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 2000};
    listener.on_trade(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

// --- RecordedEvent equality round-trip through listeners ---

TEST(RecordingListenerEqualityTest, OrderListenerRecordedEventEquality) {
    RecordingOrderListener listener;
    OrderAccepted e{.id = 42, .client_order_id = 420, .ts = 9999};
    listener.on_order_accepted(e);

    RecordedEvent expected{e};
    EXPECT_EQ(listener.events()[0], expected);
}

TEST(RecordingListenerEqualityTest, MdListenerRecordedEventEquality) {
    RecordingMdListener listener;
    Trade e{.price = 500000, .quantity = 1000, .aggressor_id = 10,
            .resting_id = 11, .aggressor_side = Side::Sell, .ts = 3000};
    listener.on_trade(e);

    RecordedEvent expected{e};
    EXPECT_EQ(listener.events()[0], expected);
}

TEST(RecordingListenerEqualityTest, InequalityDifferentEventTypes) {
    RecordingOrderListener order_listener;
    RecordingMdListener md_listener;

    // Both record an event at the same timestamp but different types
    OrderAccepted oa{.id = 1, .client_order_id = 10, .ts = 1000};
    TopOfBook tob{.best_bid = 1000, .bid_qty = 100, .best_ask = 0, .ask_qty = 0, .ts = 1000};

    order_listener.on_order_accepted(oa);
    md_listener.on_top_of_book(tob);

    EXPECT_NE(order_listener.events()[0], md_listener.events()[0]);
}

// --- RecordingMdListener: MarketStatus and IndicativePrice ---

TEST(RecordingMdListenerTest, RecordMarketStatus) {
    RecordingMdListener listener;
    MarketStatus e{.state = SessionState::PreOpen, .ts = 3000};
    listener.on_market_status(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<MarketStatus>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingMdListenerTest, RecordIndicativePrice) {
    RecordingMdListener listener;
    IndicativePrice e{.price = 1005000, .matched_volume = 40000,
                      .buy_surplus = 5000, .sell_surplus = 0, .ts = 4000};
    listener.on_indicative_price(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<IndicativePrice>(listener.events()[0]));
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

TEST(RecordingMdListenerTest, MarketStatusAndIndicativePricePreserveOrder) {
    RecordingMdListener listener;
    MarketStatus ms{.state = SessionState::PreOpen, .ts = 1000};
    IndicativePrice ip1{.price = 1005000, .matched_volume = 30000,
                        .buy_surplus = 10000, .sell_surplus = 0, .ts = 2000};
    IndicativePrice ip2{.price = 1006000, .matched_volume = 40000,
                        .buy_surplus = 0, .sell_surplus = 5000, .ts = 3000};
    MarketStatus ms2{.state = SessionState::OpeningAuction, .ts = 4000};

    listener.on_market_status(ms);
    listener.on_indicative_price(ip1);
    listener.on_indicative_price(ip2);
    listener.on_market_status(ms2);

    EXPECT_EQ(listener.size(), 4u);
    EXPECT_EQ(listener.events()[0], RecordedEvent{ms});
    EXPECT_EQ(listener.events()[1], RecordedEvent{ip1});
    EXPECT_EQ(listener.events()[2], RecordedEvent{ip2});
    EXPECT_EQ(listener.events()[3], RecordedEvent{ms2});
}

TEST(RecordingMdListenerTest, MarketStatusClearedCorrectly) {
    RecordingMdListener listener;
    listener.on_market_status(MarketStatus{.state = SessionState::Continuous, .ts = 1});
    listener.clear();
    EXPECT_EQ(listener.size(), 0u);

    IndicativePrice e{.price = 1000, .matched_volume = 100,
                      .buy_surplus = 0, .sell_surplus = 0, .ts = 2};
    listener.on_indicative_price(e);
    EXPECT_EQ(listener.size(), 1u);
    EXPECT_EQ(listener.events()[0], RecordedEvent{e});
}

}  // namespace
}  // namespace exchange
