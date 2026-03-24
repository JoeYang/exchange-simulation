#include "exchange-core/types.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

TEST(TypesTest, PriceScaleIsCorrect) {
    EXPECT_EQ(PRICE_SCALE, 10000);
}

TEST(TypesTest, SideEnumValues) {
    EXPECT_NE(Side::Buy, Side::Sell);
}

TEST(TypesTest, OrderTypeEnumValues) {
    EXPECT_NE(OrderType::Limit, OrderType::Market);
    EXPECT_NE(OrderType::Stop, OrderType::StopLimit);
}

TEST(TypesTest, OrderDefaultConstruction) {
    Order order{};
    EXPECT_EQ(order.id, 0u);
    EXPECT_EQ(order.price, 0);
    EXPECT_EQ(order.quantity, 0);
    EXPECT_EQ(order.filled_quantity, 0);
    EXPECT_EQ(order.remaining_quantity, 0);
    EXPECT_EQ(order.side, Side::Buy);
    EXPECT_EQ(order.type, OrderType::Limit);
    EXPECT_EQ(order.tif, TimeInForce::GTC);
    EXPECT_EQ(order.prev, nullptr);
    EXPECT_EQ(order.next, nullptr);
    EXPECT_EQ(order.level, nullptr);
}

TEST(TypesTest, PriceLevelDefaultConstruction) {
    PriceLevel level{};
    EXPECT_EQ(level.price, 0);
    EXPECT_EQ(level.total_quantity, 0);
    EXPECT_EQ(level.order_count, 0u);
    EXPECT_EQ(level.head, nullptr);
    EXPECT_EQ(level.tail, nullptr);
    EXPECT_EQ(level.prev, nullptr);
    EXPECT_EQ(level.next, nullptr);
}

TEST(TypesTest, FillResultDefaultConstruction) {
    FillResult fill{};
    EXPECT_EQ(fill.resting_order, nullptr);
    EXPECT_EQ(fill.price, 0);
    EXPECT_EQ(fill.quantity, 0);
    EXPECT_EQ(fill.resting_remaining, 0);
}

TEST(TypesTest, RejectReasonEnumValues) {
    // Ensure all values are distinct
    EXPECT_NE(static_cast<uint8_t>(RejectReason::PoolExhausted),
              static_cast<uint8_t>(RejectReason::InvalidPrice));
    EXPECT_NE(static_cast<uint8_t>(RejectReason::UnknownOrder),
              static_cast<uint8_t>(RejectReason::PriceBandViolation));
}

TEST(TypesTest, CancelReasonEnumValues) {
    EXPECT_NE(static_cast<uint8_t>(CancelReason::UserRequested),
              static_cast<uint8_t>(CancelReason::IOCRemainder));
    EXPECT_NE(static_cast<uint8_t>(CancelReason::FOKFailed),
              static_cast<uint8_t>(CancelReason::SelfMatchPrevention));
}

TEST(TypesTest, PriceRepresentation) {
    // 100.5000 should be represented as 1005000
    Price p = 1005000;
    EXPECT_EQ(p / PRICE_SCALE, 100);
    EXPECT_EQ(p % PRICE_SCALE, 5000);
}

}  // namespace
}  // namespace exchange
