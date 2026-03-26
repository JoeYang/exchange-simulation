#include "ice/fix/ice_fix_gateway.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace exchange::ice::fix {
namespace {

constexpr char SOH = '\x01';

// Helper: compute FIX checksum
std::string compute_checksum(const std::string& body) {
    uint32_t sum = 0;
    for (char c : body) sum += static_cast<uint8_t>(c);
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03u", sum % 256);
    return {buf, 3};
}

std::string tag(int t, const std::string& v) {
    return std::to_string(t) + "=" + v + SOH;
}

std::string make_fix_msg(const std::string& msg_type,
                         const std::string& body_tags) {
    std::string body_content = "35=" + msg_type + SOH + body_tags;
    std::string begin = std::string("8=FIX.4.2") + SOH;
    std::string body_len = "9=" + std::to_string(body_content.size()) + SOH;
    std::string pre_checksum = begin + body_len + body_content;
    std::string checksum = compute_checksum(pre_checksum);
    return pre_checksum + "10=" + checksum + SOH;
}

// ---------------------------------------------------------------------------
// Mock engine: records calls for verification.
// ---------------------------------------------------------------------------

struct RecordedNewOrder {
    uint64_t client_order_id;
    uint64_t account_id;
    Side side;
    OrderType type;
    TimeInForce tif;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    Quantity display_qty;
};

struct RecordedCancel {
    OrderId order_id;
    Timestamp timestamp;
};

struct RecordedModify {
    OrderId order_id;
    uint64_t client_order_id;
    Price new_price;
    Quantity new_quantity;
    Timestamp timestamp;
};

struct RecordedMassCancel {
    uint64_t account_id;
    Timestamp timestamp;
};

struct MockEngine {
    std::vector<RecordedNewOrder> new_orders;
    std::vector<RecordedCancel> cancels;
    std::vector<RecordedModify> modifies;
    std::vector<RecordedMassCancel> mass_cancels;

    void new_order(const OrderRequest& req) {
        new_orders.push_back({
            req.client_order_id, req.account_id,
            req.side, req.type, req.tif,
            req.price, req.quantity, req.timestamp,
            req.display_qty
        });
    }

    void cancel_order(OrderId id, Timestamp ts) {
        cancels.push_back({id, ts});
    }

    void modify_order(const ModifyRequest& req) {
        modifies.push_back({
            req.order_id, req.client_order_id,
            req.new_price, req.new_quantity, req.timestamp
        });
    }

    size_t mass_cancel(uint64_t account_id, Timestamp ts) {
        mass_cancels.push_back({account_id, ts});
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Conversion helper tests
// ---------------------------------------------------------------------------

TEST(FixConversionTest, PriceConversion) {
    // "105.50" -> 105.50 * 10000 = 1055000
    EXPECT_EQ(fix_price_to_engine("105.50"), 1055000);
    EXPECT_EQ(fix_price_to_engine("0.0001"), 1);
    EXPECT_EQ(fix_price_to_engine("100"), 1000000);
    EXPECT_EQ(fix_price_to_engine(""), 0);
}

TEST(FixConversionTest, SideConversion) {
    Side s;
    EXPECT_TRUE(fix_to_side('1', s));
    EXPECT_EQ(s, Side::Buy);
    EXPECT_TRUE(fix_to_side('2', s));
    EXPECT_EQ(s, Side::Sell);
    EXPECT_FALSE(fix_to_side('3', s));
}

TEST(FixConversionTest, OrderTypeConversion) {
    OrderType ot;
    EXPECT_TRUE(fix_to_order_type('1', ot));
    EXPECT_EQ(ot, OrderType::Market);
    EXPECT_TRUE(fix_to_order_type('2', ot));
    EXPECT_EQ(ot, OrderType::Limit);
    EXPECT_TRUE(fix_to_order_type('3', ot));
    EXPECT_EQ(ot, OrderType::Stop);
    EXPECT_TRUE(fix_to_order_type('4', ot));
    EXPECT_EQ(ot, OrderType::StopLimit);
    EXPECT_FALSE(fix_to_order_type('9', ot));
}

TEST(FixConversionTest, TifConversion) {
    TimeInForce tif;
    EXPECT_TRUE(fix_to_tif('0', tif));
    EXPECT_EQ(tif, TimeInForce::DAY);
    EXPECT_TRUE(fix_to_tif('1', tif));
    EXPECT_EQ(tif, TimeInForce::GTC);
    EXPECT_TRUE(fix_to_tif('3', tif));
    EXPECT_EQ(tif, TimeInForce::IOC);
    EXPECT_TRUE(fix_to_tif('4', tif));
    EXPECT_EQ(tif, TimeInForce::FOK);
    EXPECT_TRUE(fix_to_tif('6', tif));
    EXPECT_EQ(tif, TimeInForce::GTD);
    EXPECT_FALSE(fix_to_tif('9', tif));
}

// ---------------------------------------------------------------------------
// Gateway dispatch tests
// ---------------------------------------------------------------------------

class IceFixGatewayTest : public ::testing::Test {
protected:
    MockEngine engine;
    IceFixGateway<MockEngine> gw{engine};
};

TEST_F(IceFixGatewayTest, NewOrderSingle) {
    std::string body = tag(11, "42") + tag(1, "100") + tag(54, "1") +
                       tag(40, "2") + tag(44, "105.50") + tag(38, "10") +
                       tag(59, "0") + tag(55, "CL") + tag(111, "5");
    std::string msg = make_fix_msg("D", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_TRUE(result.ok) << result.error;

    ASSERT_EQ(engine.new_orders.size(), 1u);
    const auto& o = engine.new_orders[0];
    EXPECT_EQ(o.client_order_id, 42u);
    EXPECT_EQ(o.account_id, 100u);
    EXPECT_EQ(o.side, Side::Buy);
    EXPECT_EQ(o.type, OrderType::Limit);
    EXPECT_EQ(o.tif, TimeInForce::DAY);
    EXPECT_EQ(o.price, 1055000);     // 105.50 * 10000
    EXPECT_EQ(o.quantity, 100000);   // 10 * 10000
    EXPECT_EQ(o.timestamp, 1000);
    EXPECT_EQ(o.display_qty, 50000); // 5 * 10000
}

TEST_F(IceFixGatewayTest, CancelRequest) {
    std::string body = tag(41, "7") + tag(11, "99") + tag(55, "CL") +
                       tag(54, "1");
    std::string msg = make_fix_msg("F", body);

    auto result = gw.on_message(msg.data(), msg.size(), 2000);
    ASSERT_TRUE(result.ok) << result.error;

    ASSERT_EQ(engine.cancels.size(), 1u);
    EXPECT_EQ(engine.cancels[0].order_id, 7u);
    EXPECT_EQ(engine.cancels[0].timestamp, 2000);
}

TEST_F(IceFixGatewayTest, ReplaceRequest) {
    std::string body = tag(41, "7") + tag(11, "99") + tag(44, "110.25") +
                       tag(38, "20") + tag(55, "CL") + tag(54, "2") +
                       tag(40, "2");
    std::string msg = make_fix_msg("G", body);

    auto result = gw.on_message(msg.data(), msg.size(), 3000);
    ASSERT_TRUE(result.ok) << result.error;

    ASSERT_EQ(engine.modifies.size(), 1u);
    const auto& m = engine.modifies[0];
    EXPECT_EQ(m.order_id, 7u);
    EXPECT_EQ(m.client_order_id, 99u);
    EXPECT_EQ(m.new_price, 1102500);  // 110.25 * 10000
    EXPECT_EQ(m.new_quantity, 200000); // 20 * 10000
    EXPECT_EQ(m.timestamp, 3000);
}

TEST_F(IceFixGatewayTest, MassCancel) {
    auto result = gw.mass_cancel(100, 4000);
    ASSERT_TRUE(result.ok);

    ASSERT_EQ(engine.mass_cancels.size(), 1u);
    EXPECT_EQ(engine.mass_cancels[0].account_id, 100u);
    EXPECT_EQ(engine.mass_cancels[0].timestamp, 4000);
}

// ---------------------------------------------------------------------------
// Error handling tests
// ---------------------------------------------------------------------------

TEST_F(IceFixGatewayTest, MalformedMessage) {
    auto result = gw.on_message("garbage", 7, 1000);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("parse error"), std::string::npos);
}

TEST_F(IceFixGatewayTest, InvalidSide) {
    std::string body = tag(11, "1") + tag(54, "9") + tag(40, "2") +
                       tag(44, "100") + tag(38, "10") + tag(59, "0") +
                       tag(55, "CL");
    std::string msg = make_fix_msg("D", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Side"), std::string::npos);
}

TEST_F(IceFixGatewayTest, InvalidOrdType) {
    std::string body = tag(11, "1") + tag(54, "1") + tag(40, "9") +
                       tag(44, "100") + tag(38, "10") + tag(59, "0") +
                       tag(55, "CL");
    std::string msg = make_fix_msg("D", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("OrdType"), std::string::npos);
}

TEST_F(IceFixGatewayTest, InvalidTif) {
    std::string body = tag(11, "1") + tag(54, "1") + tag(40, "2") +
                       tag(44, "100") + tag(38, "10") + tag(59, "9") +
                       tag(55, "CL");
    std::string msg = make_fix_msg("D", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("TimeInForce"), std::string::npos);
}

TEST_F(IceFixGatewayTest, MissingClOrdId) {
    // NOS without ClOrdID (tag 11)
    std::string body = tag(54, "1") + tag(40, "2") + tag(44, "100") +
                       tag(38, "10") + tag(59, "0") + tag(55, "CL");
    std::string msg = make_fix_msg("D", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("ClOrdID"), std::string::npos);
}

TEST_F(IceFixGatewayTest, MissingOrigClOrdIdOnCancel) {
    // Cancel without OrigClOrdID (tag 41)
    std::string body = tag(11, "99") + tag(55, "CL") + tag(54, "1");
    std::string msg = make_fix_msg("F", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("OrigClOrdID"), std::string::npos);
}

TEST_F(IceFixGatewayTest, SessionMessagePassthrough) {
    // Heartbeat (35=0) should succeed without dispatching anything
    std::string msg = make_fix_msg("0", "");

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(engine.new_orders.size(), 0u);
    EXPECT_EQ(engine.cancels.size(), 0u);
}

TEST_F(IceFixGatewayTest, IceCustomTagsPreserved) {
    // Verify ICE custom tags don't break parsing; gateway ignores them
    // but they're available in the parsed FixMessage for upstream use.
    std::string body = tag(11, "42") + tag(54, "1") + tag(40, "2") +
                       tag(44, "100") + tag(38, "10") + tag(59, "0") +
                       tag(55, "CL") + tag(1, "100") +
                       tag(9139, "USR1") + tag(9195, "HEDGE") +
                       tag(9121, "memo");
    std::string msg = make_fix_msg("D", body);

    auto result = gw.on_message(msg.data(), msg.size(), 1000);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(engine.new_orders.size(), 1u);
    EXPECT_EQ(engine.new_orders[0].client_order_id, 42u);
}

}  // namespace
}  // namespace exchange::ice::fix
