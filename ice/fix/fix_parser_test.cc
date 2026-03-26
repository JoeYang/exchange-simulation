#include "ice/fix/fix_parser.h"
#include "ice/fix/ice_fix_messages.h"

#include <gtest/gtest.h>

#include <string>

namespace ice::fix {
namespace {

// SOH delimiter (ASCII 0x01)
constexpr char SOH = '\x01';

// Helper: compute FIX checksum for everything before "10="
std::string compute_checksum(const std::string& body) {
    uint32_t sum = 0;
    for (char c : body) {
        sum += static_cast<uint8_t>(c);
    }
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03u", sum % 256);
    return std::string(buf, 3);
}

// Helper: build a raw FIX message with correct BodyLength and CheckSum
std::string make_fix_msg(const std::string& msg_type,
                         const std::string& body_tags) {
    // body = 35=X|SOH + body_tags (each already ends with SOH)
    std::string body_content = "35=" + msg_type + SOH + body_tags;
    std::string begin = "8=FIX.4.2" + std::string(1, SOH);
    std::string body_len =
        "9=" + std::to_string(body_content.size()) + SOH;
    std::string pre_checksum = begin + body_len + body_content;
    std::string checksum = compute_checksum(pre_checksum);
    return pre_checksum + "10=" + checksum + SOH;
}

// Helper: build tag=value|SOH fragment
std::string tag(int t, const std::string& v) {
    return std::to_string(t) + "=" + v + SOH;
}

// --- NewOrderSingle (D) tests ---

TEST(FixParserTest, ParseValidNewOrderSingle) {
    std::string body = tag(11, "ORD001") + tag(54, "1") + tag(40, "2") +
                       tag(44, "105.50") + tag(38, "100") +
                       tag(59, "0") + tag(55, "ES") + tag(1, "ACCT01") +
                       tag(111, "25");  // MaxFloor (iceberg)
    std::string msg = make_fix_msg("D", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    const auto& fix = result.value();
    EXPECT_EQ(fix.msg_type, "D");
    EXPECT_EQ(fix.get_string(11), "ORD001");  // ClOrdID
    EXPECT_EQ(fix.get_char(54), '1');          // Side=Buy
    EXPECT_EQ(fix.get_char(40), '2');          // OrdType=Limit
    EXPECT_DOUBLE_EQ(fix.get_double(44), 105.50);  // Price
    EXPECT_EQ(fix.get_int(38), 100);           // OrderQty
    EXPECT_EQ(fix.get_char(59), '0');          // TimeInForce=DAY
    EXPECT_EQ(fix.get_string(55), "ES");       // Symbol
    EXPECT_EQ(fix.get_string(1), "ACCT01");    // Account
    EXPECT_EQ(fix.get_int(111), 25);           // MaxFloor

    // Also verify typed struct conversion
    auto nos = FixNewOrderSingle::from_fix(fix);
    EXPECT_EQ(nos.cl_ord_id, "ORD001");
    EXPECT_EQ(nos.side, '1');
    EXPECT_EQ(nos.max_floor, 25);
    EXPECT_EQ(nos.account, "ACCT01");
}

// --- OrderCancelRequest (F) tests ---

TEST(FixParserTest, ParseValidOrderCancelRequest) {
    std::string body = tag(41, "ORD001") + tag(11, "CXL001") +
                       tag(55, "ES") + tag(54, "1");
    std::string msg = make_fix_msg("F", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    const auto& fix = result.value();
    EXPECT_EQ(fix.msg_type, "F");

    auto cr = FixCancelRequest::from_fix(fix);
    EXPECT_EQ(cr.orig_cl_ord_id, "ORD001");
    EXPECT_EQ(cr.cl_ord_id, "CXL001");
    EXPECT_EQ(cr.symbol, "ES");
    EXPECT_EQ(cr.side, '1');
}

// --- OrderCancelReplaceRequest (G) tests ---

TEST(FixParserTest, ParseValidOrderCancelReplaceRequest) {
    std::string body = tag(41, "ORD001") + tag(11, "REP001") +
                       tag(44, "110.25") + tag(38, "200") +
                       tag(55, "CL") + tag(54, "2") + tag(40, "2");
    std::string msg = make_fix_msg("G", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto crr = FixCancelReplaceRequest::from_fix(result.value());
    EXPECT_EQ(crr.orig_cl_ord_id, "ORD001");
    EXPECT_EQ(crr.cl_ord_id, "REP001");
    EXPECT_DOUBLE_EQ(crr.price, 110.25);
    EXPECT_EQ(crr.order_qty, 200);
    EXPECT_EQ(crr.symbol, "CL");
    EXPECT_EQ(crr.side, '2');
    EXPECT_EQ(crr.ord_type, '2');
}

// --- Checksum validation ---

TEST(FixParserTest, InvalidChecksum) {
    std::string body = tag(11, "ORD001") + tag(55, "ES");
    std::string msg = make_fix_msg("D", body);
    // Corrupt the checksum: replace last 4 chars before final SOH
    auto pos = msg.rfind("10=");
    ASSERT_NE(pos, std::string::npos);
    msg[pos + 3] = '0';
    msg[pos + 4] = '0';
    msg[pos + 5] = '0';

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("checksum"), std::string::npos);
}

// --- Malformed messages ---

TEST(FixParserTest, WrongBeginString) {
    // Use FIX.4.4 instead of FIX.4.2
    std::string raw = std::string("8=FIX.4.4") + SOH + "9=5" + SOH +
                      "35=D" + SOH + "10=000" + SOH;
    auto result = parse_fix_message(raw.data(), raw.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("BeginString"), std::string::npos);
}

TEST(FixParserTest, EmptyMessage) {
    auto result = parse_fix_message("", 0);
    ASSERT_FALSE(result.has_value());
}

TEST(FixParserTest, TruncatedMessage) {
    std::string raw = std::string("8=FIX.4.2") + SOH;
    auto result = parse_fix_message(raw.data(), raw.size());
    ASSERT_FALSE(result.has_value());
}

TEST(FixParserTest, MissingMsgType) {
    // Build a message without tag 35
    std::string body_content = tag(11, "ORD001");
    std::string begin = "8=FIX.4.2" + std::string(1, SOH);
    std::string body_len = "9=" + std::to_string(body_content.size()) + SOH;
    std::string pre_checksum = begin + body_len + body_content;
    std::string checksum = compute_checksum(pre_checksum);
    std::string msg = pre_checksum + "10=" + checksum + SOH;

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("MsgType"), std::string::npos);
}

// --- ICE custom tags ---

TEST(FixParserTest, IceCustomTags) {
    std::string body = tag(11, "ORD002") + tag(55, "CL") + tag(54, "2") +
                       tag(40, "2") + tag(44, "72.50") + tag(38, "50") +
                       tag(59, "1") +
                       tag(9139, "USR42") +     // OriginatorUserID
                       tag(9195, "HEDGE") +     // AccountCode
                       tag(9121, "memo text");  // MemoField
    std::string msg = make_fix_msg("D", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    const auto& fix = result.value();
    EXPECT_EQ(fix.get_string(9139), "USR42");
    EXPECT_EQ(fix.get_string(9195), "HEDGE");
    EXPECT_EQ(fix.get_string(9121), "memo text");
}

// --- Helper accessor edge cases ---

TEST(FixParserTest, GetIntMissingTag) {
    std::string body = tag(11, "ORD001") + tag(55, "ES");
    std::string msg = make_fix_msg("D", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    // Tag 38 (OrderQty) not present — should return 0
    EXPECT_EQ(result.value().get_int(38), 0);
}

TEST(FixParserTest, GetDoubleMissingTag) {
    std::string body = tag(11, "ORD001") + tag(55, "ES");
    std::string msg = make_fix_msg("D", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_DOUBLE_EQ(result.value().get_double(44), 0.0);
}

TEST(FixParserTest, GetCharMissingTag) {
    std::string body = tag(11, "ORD001") + tag(55, "ES");
    std::string msg = make_fix_msg("D", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result.value().get_char(54), '\0');
}

// --- Execution report (8) and session-level messages ---

TEST(FixParserTest, ParseExecutionReport) {
    std::string body = tag(11, "ORD001") + tag(17, "EXEC001") +
                       tag(150, "0") + tag(39, "0") +
                       tag(55, "ES") + tag(54, "1") +
                       tag(44, "100.00") + tag(38, "50");
    std::string msg = make_fix_msg("8", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto er = FixExecutionReport::from_fix(result.value());
    EXPECT_EQ(er.cl_ord_id, "ORD001");
    EXPECT_EQ(er.exec_id, "EXEC001");
    EXPECT_EQ(er.exec_type, '0');
    EXPECT_EQ(er.ord_status, '0');
    EXPECT_EQ(er.symbol, "ES");
    EXPECT_EQ(er.side, '1');
    EXPECT_DOUBLE_EQ(er.price, 100.00);
    EXPECT_EQ(er.order_qty, 50);
}

TEST(FixParserTest, ParseLogon) {
    std::string body = tag(98, "0") + tag(108, "30");
    std::string msg = make_fix_msg("A", body);

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().msg_type, "A");
}

TEST(FixParserTest, ParseHeartbeat) {
    std::string msg = make_fix_msg("0", "");

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().msg_type, "0");
}

// --- BodyLength validation ---

TEST(FixParserTest, InvalidBodyLength) {
    // Manually construct with wrong BodyLength
    std::string body_content = "35=D" + std::string(1, SOH) +
                               tag(11, "ORD001");
    std::string begin = "8=FIX.4.2" + std::string(1, SOH);
    // Intentionally wrong length
    std::string body_len = "9=999" + std::string(1, SOH);
    std::string pre_checksum = begin + body_len + body_content;
    std::string checksum = compute_checksum(pre_checksum);
    std::string msg = pre_checksum + "10=" + checksum + SOH;

    auto result = parse_fix_message(msg.data(), msg.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("BodyLength"), std::string::npos);
}

// --- Round-trip tests: build FixMessage -> encode -> parse -> verify ---

TEST(FixParserTest, RoundTripNewOrderSingle) {
    FixMessage orig;
    orig.msg_type = "D";
    orig.fields = {
        {35, "D"}, {11, "RT001"}, {1, "ACCT"}, {55, "ES"},
        {54, "1"}, {38, "100"}, {44, "99.75"}, {40, "2"},
        {59, "0"}, {111, "20"},
    };

    std::string wire = encode_fix_message(orig);
    auto result = parse_fix_message(wire.data(), wire.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto nos = FixNewOrderSingle::from_fix(result.value());
    EXPECT_EQ(nos.cl_ord_id, "RT001");
    EXPECT_EQ(nos.account, "ACCT");
    EXPECT_EQ(nos.symbol, "ES");
    EXPECT_EQ(nos.side, '1');
    EXPECT_EQ(nos.order_qty, 100);
    EXPECT_DOUBLE_EQ(nos.price, 99.75);
    EXPECT_EQ(nos.ord_type, '2');
    EXPECT_EQ(nos.time_in_force, '0');
    EXPECT_EQ(nos.max_floor, 20);
}

TEST(FixParserTest, RoundTripCancelRequest) {
    FixMessage orig;
    orig.msg_type = "F";
    orig.fields = {
        {35, "F"}, {11, "CXL99"}, {41, "ORIG99"}, {55, "CL"}, {54, "2"},
    };

    std::string wire = encode_fix_message(orig);
    auto result = parse_fix_message(wire.data(), wire.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto cr = FixCancelRequest::from_fix(result.value());
    EXPECT_EQ(cr.cl_ord_id, "CXL99");
    EXPECT_EQ(cr.orig_cl_ord_id, "ORIG99");
    EXPECT_EQ(cr.symbol, "CL");
    EXPECT_EQ(cr.side, '2');
}

TEST(FixParserTest, RoundTripExecutionReport) {
    FixMessage orig;
    orig.msg_type = "8";
    orig.fields = {
        {35, "8"}, {11, "ORD01"}, {17, "EX01"}, {150, "F"},
        {39, "2"}, {55, "ES"}, {54, "1"}, {44, "101.50"}, {38, "75"},
    };

    std::string wire = encode_fix_message(orig);
    auto result = parse_fix_message(wire.data(), wire.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto er = FixExecutionReport::from_fix(result.value());
    EXPECT_EQ(er.cl_ord_id, "ORD01");
    EXPECT_EQ(er.exec_id, "EX01");
    EXPECT_EQ(er.exec_type, 'F');
    EXPECT_EQ(er.ord_status, '2');
    EXPECT_DOUBLE_EQ(er.price, 101.50);
    EXPECT_EQ(er.order_qty, 75);
}

}  // namespace
}  // namespace ice::fix
