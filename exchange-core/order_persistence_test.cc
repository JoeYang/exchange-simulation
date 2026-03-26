#include "exchange-core/order_persistence.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <limits>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Helper: build an Order with all fields populated for round-trip testing.
// ---------------------------------------------------------------------------

Order make_full_order() {
    Order o;
    o.id                 = 42;
    o.client_order_id    = 100;
    o.account_id         = 7;
    o.price              = 1005000;  // 100.5000
    o.quantity           = 50000;    // 5.0000
    o.filled_quantity    = 10000;    // 1.0000
    o.remaining_quantity = 40000;    // 4.0000
    o.side               = Side::Buy;
    o.type               = OrderType::Limit;
    o.tif                = TimeInForce::GTC;
    o.timestamp          = 1000000000;
    o.gtd_expiry         = 0;
    o.display_qty        = 0;
    o.total_qty          = 50000;
    // Set intrusive-list pointers to non-null to verify they get cleared.
    o.prev  = reinterpret_cast<Order*>(0xDEAD);
    o.next  = reinterpret_cast<Order*>(0xBEEF);
    o.level = reinterpret_cast<PriceLevel*>(0xCAFE);
    return o;
}

// ---------------------------------------------------------------------------
// Round-trip: serialize then deserialize, all fields match.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, RoundTripAllFields) {
    Order original = make_full_order();
    SerializedOrder serialized = serialize_order(original);
    Order restored{};
    deserialize_order(serialized, &restored);

    EXPECT_EQ(restored.id, original.id);
    EXPECT_EQ(restored.client_order_id, original.client_order_id);
    EXPECT_EQ(restored.account_id, original.account_id);
    EXPECT_EQ(restored.price, original.price);
    EXPECT_EQ(restored.quantity, original.quantity);
    EXPECT_EQ(restored.filled_quantity, original.filled_quantity);
    EXPECT_EQ(restored.remaining_quantity, original.remaining_quantity);
    EXPECT_EQ(restored.side, original.side);
    EXPECT_EQ(restored.type, original.type);
    EXPECT_EQ(restored.tif, original.tif);
    EXPECT_EQ(restored.timestamp, original.timestamp);
    EXPECT_EQ(restored.gtd_expiry, original.gtd_expiry);
    EXPECT_EQ(restored.display_qty, original.display_qty);
    EXPECT_EQ(restored.total_qty, original.total_qty);

    // Intrusive-list pointers must be cleared after deserialization.
    EXPECT_EQ(restored.prev, nullptr);
    EXPECT_EQ(restored.next, nullptr);
    EXPECT_EQ(restored.level, nullptr);
}

// ---------------------------------------------------------------------------
// Iceberg order: display_qty and total_qty preserved.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, IcebergOrderRoundTrip) {
    Order o{};
    o.id                 = 5;
    o.client_order_id    = 200;
    o.account_id         = 3;
    o.price              = 2000000;
    o.quantity           = 100000;
    o.filled_quantity    = 20000;
    o.remaining_quantity = 10000;   // visible tranche
    o.side               = Side::Sell;
    o.type               = OrderType::Limit;
    o.tif                = TimeInForce::GTC;
    o.timestamp          = 5000;
    o.display_qty        = 10000;   // iceberg display size
    o.total_qty          = 100000;  // total including hidden

    SerializedOrder s = serialize_order(o);
    Order restored{};
    deserialize_order(s, &restored);

    EXPECT_EQ(restored.display_qty, 10000);
    EXPECT_EQ(restored.total_qty, 100000);
    EXPECT_EQ(restored.remaining_quantity, 10000);
    EXPECT_EQ(restored.filled_quantity, 20000);
}

// ---------------------------------------------------------------------------
// GTD order: gtd_expiry preserved.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, GtdOrderRoundTrip) {
    Order o{};
    o.id                 = 10;
    o.client_order_id    = 300;
    o.price              = 500000;
    o.quantity           = 30000;
    o.filled_quantity    = 0;
    o.remaining_quantity = 30000;
    o.side               = Side::Buy;
    o.type               = OrderType::Limit;
    o.tif                = TimeInForce::GTD;
    o.timestamp          = 1000;
    o.gtd_expiry         = 9999999999;

    SerializedOrder s = serialize_order(o);
    Order restored{};
    deserialize_order(s, &restored);

    EXPECT_EQ(restored.tif, TimeInForce::GTD);
    EXPECT_EQ(restored.gtd_expiry, 9999999999);
}

// ---------------------------------------------------------------------------
// Partially filled order: filled_quantity, remaining_quantity correct.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, PartiallyFilledRoundTrip) {
    Order o{};
    o.id                 = 77;
    o.client_order_id    = 400;
    o.price              = 750000;
    o.quantity           = 80000;
    o.filled_quantity    = 55000;
    o.remaining_quantity = 25000;
    o.side               = Side::Sell;
    o.type               = OrderType::Limit;
    o.tif                = TimeInForce::GTC;
    o.timestamp          = 2000;

    SerializedOrder s = serialize_order(o);
    Order restored{};
    deserialize_order(s, &restored);

    EXPECT_EQ(restored.quantity, 80000);
    EXPECT_EQ(restored.filled_quantity, 55000);
    EXPECT_EQ(restored.remaining_quantity, 25000);
}

// ---------------------------------------------------------------------------
// Sell side and various order types preserved.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, SellSidePreserved) {
    Order o{};
    o.side = Side::Sell;
    o.type = OrderType::StopLimit;
    o.tif  = TimeInForce::IOC;

    SerializedOrder s = serialize_order(o);
    Order restored{};
    deserialize_order(s, &restored);

    EXPECT_EQ(restored.side, Side::Sell);
    EXPECT_EQ(restored.type, OrderType::StopLimit);
    EXPECT_EQ(restored.tif, TimeInForce::IOC);
}

// ---------------------------------------------------------------------------
// Boundary: maximum values for integer fields.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, BoundaryMaxValues) {
    Order o{};
    o.id                 = std::numeric_limits<OrderId>::max();
    o.client_order_id    = std::numeric_limits<uint64_t>::max();
    o.account_id         = std::numeric_limits<uint64_t>::max();
    o.price              = std::numeric_limits<Price>::max();
    o.quantity           = std::numeric_limits<Quantity>::max();
    o.filled_quantity    = std::numeric_limits<Quantity>::max();
    o.remaining_quantity = std::numeric_limits<Quantity>::max();
    o.timestamp          = std::numeric_limits<Timestamp>::max();
    o.gtd_expiry         = std::numeric_limits<Timestamp>::max();
    o.display_qty        = std::numeric_limits<Quantity>::max();
    o.total_qty          = std::numeric_limits<Quantity>::max();

    SerializedOrder s = serialize_order(o);
    Order restored{};
    deserialize_order(s, &restored);

    EXPECT_EQ(restored.id, std::numeric_limits<OrderId>::max());
    EXPECT_EQ(restored.client_order_id, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(restored.account_id, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(restored.price, std::numeric_limits<Price>::max());
    EXPECT_EQ(restored.quantity, std::numeric_limits<Quantity>::max());
    EXPECT_EQ(restored.filled_quantity, std::numeric_limits<Quantity>::max());
    EXPECT_EQ(restored.remaining_quantity, std::numeric_limits<Quantity>::max());
    EXPECT_EQ(restored.timestamp, std::numeric_limits<Timestamp>::max());
    EXPECT_EQ(restored.gtd_expiry, std::numeric_limits<Timestamp>::max());
    EXPECT_EQ(restored.display_qty, std::numeric_limits<Quantity>::max());
    EXPECT_EQ(restored.total_qty, std::numeric_limits<Quantity>::max());
}

// ---------------------------------------------------------------------------
// Boundary: zero values for all fields.
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, BoundaryZeroValues) {
    Order o{};  // all zero-initialized

    SerializedOrder s = serialize_order(o);
    Order restored{};
    deserialize_order(s, &restored);

    EXPECT_EQ(restored.id, 0u);
    EXPECT_EQ(restored.client_order_id, 0u);
    EXPECT_EQ(restored.account_id, 0u);
    EXPECT_EQ(restored.price, 0);
    EXPECT_EQ(restored.quantity, 0);
    EXPECT_EQ(restored.filled_quantity, 0);
    EXPECT_EQ(restored.remaining_quantity, 0);
    EXPECT_EQ(restored.timestamp, 0);
    EXPECT_EQ(restored.gtd_expiry, 0);
    EXPECT_EQ(restored.display_qty, 0);
    EXPECT_EQ(restored.total_qty, 0);
}

// ---------------------------------------------------------------------------
// Serialize strips intrusive pointers (they do not appear in SerializedOrder).
// ---------------------------------------------------------------------------

TEST(OrderPersistenceTest, SerializeStripsPointers) {
    Order o = make_full_order();
    // Pointers are non-null in the original.
    ASSERT_NE(o.prev, nullptr);
    ASSERT_NE(o.next, nullptr);
    ASSERT_NE(o.level, nullptr);

    SerializedOrder s = serialize_order(o);

    // SerializedOrder has no pointer fields -- verify all value fields
    // match the original.  The absence of pointer fields is enforced by
    // the struct definition itself (compile-time guarantee).
    EXPECT_EQ(s.id, o.id);
    EXPECT_EQ(s.price, o.price);
    EXPECT_EQ(s.quantity, o.quantity);

    // Deserialize and confirm pointers are null.
    Order restored{};
    deserialize_order(s, &restored);
    EXPECT_EQ(restored.prev, nullptr);
    EXPECT_EQ(restored.next, nullptr);
    EXPECT_EQ(restored.level, nullptr);
}

}  // namespace
}  // namespace exchange
