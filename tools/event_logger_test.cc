#include "tools/event_logger.h"

#include <sstream>
#include <string>

#include "gtest/gtest.h"

namespace exchange {
namespace {

class EventLoggerJsonTest : public ::testing::Test {
protected:
    std::ostringstream out_;
    EventLogger logger_{out_, EventLogger::Format::JSON};
};

class EventLoggerCsvTest : public ::testing::Test {
protected:
    std::ostringstream out_;
    EventLogger logger_{out_, EventLogger::Format::CSV};
};

// --- JSON tests ---

TEST_F(EventLoggerJsonTest, OrderAccepted) {
    logger_.on_order_accepted({.id = 1, .client_order_id = 100, .ts = 1000});
    EXPECT_EQ(out_.str(),
              R"({"ts":1000,"type":"OrderAccepted","order_id":1,"client_order_id":100})"
              "\n");
    EXPECT_EQ(logger_.event_count(), 1u);
}

TEST_F(EventLoggerJsonTest, OrderRejected) {
    logger_.on_order_rejected(
        {.client_order_id = 200, .ts = 2000, .reason = RejectReason::InvalidPrice});
    EXPECT_EQ(out_.str(),
              R"({"ts":2000,"type":"OrderRejected","client_order_id":200,"reason":"InvalidPrice"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, OrderFilled) {
    logger_.on_order_filled(
        {.aggressor_id = 5, .resting_id = 3, .price = 50000000, .quantity = 10000, .ts = 3000});
    EXPECT_EQ(out_.str(),
              R"({"ts":3000,"type":"OrderFilled","aggressor_id":5,"resting_id":3,"price":50000000,"qty":10000})"
              "\n");
}

TEST_F(EventLoggerJsonTest, OrderPartiallyFilled) {
    logger_.on_order_partially_filled(
        {.aggressor_id = 5,
         .resting_id = 3,
         .price = 50000000,
         .quantity = 5000,
         .aggressor_remaining = 5000,
         .resting_remaining = 0,
         .ts = 3500});
    EXPECT_EQ(out_.str(),
              R"({"ts":3500,"type":"OrderPartiallyFilled","aggressor_id":5,"resting_id":3,"price":50000000,"qty":5000,"aggressor_remaining":5000,"resting_remaining":0})"
              "\n");
}

TEST_F(EventLoggerJsonTest, OrderCancelled) {
    logger_.on_order_cancelled(
        {.id = 7, .ts = 4000, .reason = CancelReason::UserRequested});
    EXPECT_EQ(out_.str(),
              R"({"ts":4000,"type":"OrderCancelled","order_id":7,"reason":"UserRequested"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, OrderCancelRejected) {
    logger_.on_order_cancel_rejected(
        {.id = 8, .client_order_id = 300, .ts = 4500, .reason = RejectReason::UnknownOrder});
    EXPECT_EQ(out_.str(),
              R"({"ts":4500,"type":"OrderCancelRejected","order_id":8,"client_order_id":300,"reason":"UnknownOrder"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, OrderModified) {
    logger_.on_order_modified(
        {.id = 9, .client_order_id = 400, .new_price = 60000000, .new_qty = 20000, .ts = 5000});
    EXPECT_EQ(out_.str(),
              R"({"ts":5000,"type":"OrderModified","order_id":9,"client_order_id":400,"new_price":60000000,"new_qty":20000})"
              "\n");
}

TEST_F(EventLoggerJsonTest, OrderModifyRejected) {
    logger_.on_order_modify_rejected(
        {.id = 10, .client_order_id = 500, .ts = 5500, .reason = RejectReason::InvalidQuantity});
    EXPECT_EQ(out_.str(),
              R"({"ts":5500,"type":"OrderModifyRejected","order_id":10,"client_order_id":500,"reason":"InvalidQuantity"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, Trade) {
    logger_.on_trade(
        {.price = 50000000,
         .quantity = 10000,
         .aggressor_id = 2,
         .resting_id = 1,
         .aggressor_side = Side::Sell,
         .ts = 6000});
    EXPECT_EQ(out_.str(),
              R"({"ts":6000,"type":"Trade","price":50000000,"qty":10000,"aggressor_id":2,"resting_id":1,"aggressor_side":"Sell"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, TopOfBook) {
    logger_.on_top_of_book(
        {.best_bid = 49990000, .bid_qty = 50000, .best_ask = 50010000, .ask_qty = 30000, .ts = 7000});
    EXPECT_EQ(out_.str(),
              R"({"ts":7000,"type":"TopOfBook","best_bid":49990000,"bid_qty":50000,"best_ask":50010000,"ask_qty":30000})"
              "\n");
}

TEST_F(EventLoggerJsonTest, DepthUpdate) {
    logger_.on_depth_update(
        {.side = Side::Buy,
         .price = 49990000,
         .total_qty = 50000,
         .order_count = 3,
         .action = DepthUpdate::Add,
         .ts = 8000});
    EXPECT_EQ(out_.str(),
              R"({"ts":8000,"type":"DepthUpdate","side":"Buy","price":49990000,"total_qty":50000,"order_count":3,"action":"Add"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, MarketStatus) {
    logger_.on_market_status({.state = SessionState::Continuous, .ts = 9000});
    EXPECT_EQ(out_.str(),
              R"({"ts":9000,"type":"MarketStatus","state":"Continuous"})"
              "\n");
}

TEST_F(EventLoggerJsonTest, EventCountTracksAllTypes) {
    logger_.on_order_accepted({.id = 1, .client_order_id = 1, .ts = 100});
    logger_.on_trade({.price = 100, .quantity = 1, .aggressor_id = 2, .resting_id = 1,
                      .aggressor_side = Side::Buy, .ts = 200});
    logger_.on_top_of_book({.best_bid = 99, .bid_qty = 1, .best_ask = 101, .ask_qty = 1, .ts = 300});
    EXPECT_EQ(logger_.event_count(), 3u);
}

// --- CSV tests ---

TEST_F(EventLoggerCsvTest, OrderAccepted) {
    logger_.on_order_accepted({.id = 1, .client_order_id = 100, .ts = 1000});
    EXPECT_EQ(out_.str(), "1000,OrderAccepted,1,100\n");
}

TEST_F(EventLoggerCsvTest, OrderRejected) {
    logger_.on_order_rejected(
        {.client_order_id = 200, .ts = 2000, .reason = RejectReason::InvalidPrice});
    EXPECT_EQ(out_.str(), "2000,OrderRejected,200,InvalidPrice\n");
}

TEST_F(EventLoggerCsvTest, OrderFilled) {
    logger_.on_order_filled(
        {.aggressor_id = 5, .resting_id = 3, .price = 50000000, .quantity = 10000, .ts = 3000});
    EXPECT_EQ(out_.str(), "3000,OrderFilled,5,3,50000000,10000\n");
}

TEST_F(EventLoggerCsvTest, OrderPartiallyFilled) {
    logger_.on_order_partially_filled(
        {.aggressor_id = 5,
         .resting_id = 3,
         .price = 50000000,
         .quantity = 5000,
         .aggressor_remaining = 5000,
         .resting_remaining = 0,
         .ts = 3500});
    EXPECT_EQ(out_.str(), "3500,OrderPartiallyFilled,5,3,50000000,5000,5000,0\n");
}

TEST_F(EventLoggerCsvTest, OrderCancelled) {
    logger_.on_order_cancelled(
        {.id = 7, .ts = 4000, .reason = CancelReason::UserRequested});
    EXPECT_EQ(out_.str(), "4000,OrderCancelled,7,UserRequested\n");
}

TEST_F(EventLoggerCsvTest, OrderCancelRejected) {
    logger_.on_order_cancel_rejected(
        {.id = 8, .client_order_id = 300, .ts = 4500, .reason = RejectReason::UnknownOrder});
    EXPECT_EQ(out_.str(), "4500,OrderCancelRejected,8,300,UnknownOrder\n");
}

TEST_F(EventLoggerCsvTest, OrderModified) {
    logger_.on_order_modified(
        {.id = 9, .client_order_id = 400, .new_price = 60000000, .new_qty = 20000, .ts = 5000});
    EXPECT_EQ(out_.str(), "5000,OrderModified,9,400,60000000,20000\n");
}

TEST_F(EventLoggerCsvTest, OrderModifyRejected) {
    logger_.on_order_modify_rejected(
        {.id = 10, .client_order_id = 500, .ts = 5500, .reason = RejectReason::InvalidQuantity});
    EXPECT_EQ(out_.str(), "5500,OrderModifyRejected,10,500,InvalidQuantity\n");
}

TEST_F(EventLoggerCsvTest, Trade) {
    logger_.on_trade(
        {.price = 50000000,
         .quantity = 10000,
         .aggressor_id = 2,
         .resting_id = 1,
         .aggressor_side = Side::Sell,
         .ts = 6000});
    EXPECT_EQ(out_.str(), "6000,Trade,50000000,10000,2,1,Sell\n");
}

TEST_F(EventLoggerCsvTest, TopOfBook) {
    logger_.on_top_of_book(
        {.best_bid = 49990000, .bid_qty = 50000, .best_ask = 50010000, .ask_qty = 30000, .ts = 7000});
    EXPECT_EQ(out_.str(), "7000,TopOfBook,49990000,50000,50010000,30000\n");
}

TEST_F(EventLoggerCsvTest, DepthUpdate) {
    logger_.on_depth_update(
        {.side = Side::Buy,
         .price = 49990000,
         .total_qty = 50000,
         .order_count = 3,
         .action = DepthUpdate::Add,
         .ts = 8000});
    EXPECT_EQ(out_.str(), "8000,DepthUpdate,Buy,49990000,50000,3,Add\n");
}

TEST_F(EventLoggerCsvTest, MarketStatus) {
    logger_.on_market_status({.state = SessionState::Continuous, .ts = 9000});
    EXPECT_EQ(out_.str(), "9000,MarketStatus,Continuous\n");
}

TEST_F(EventLoggerCsvTest, EventCount) {
    logger_.on_order_accepted({.id = 1, .client_order_id = 1, .ts = 100});
    logger_.on_order_cancelled({.id = 1, .ts = 200, .reason = CancelReason::UserRequested});
    EXPECT_EQ(logger_.event_count(), 2u);
}

}  // namespace
}  // namespace exchange
