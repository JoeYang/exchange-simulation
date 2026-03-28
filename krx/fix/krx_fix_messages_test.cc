#include "krx/fix/krx_fix_messages.h"

#include <gtest/gtest.h>

#include <string>

namespace exchange::krx::fix {
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

// --- KrxNewOrderSingle ---

TEST(KrxNewOrderSingleTest, ParsesStandardFields) {
    std::string body = tag(11, "42") + tag(1, "100") + tag(55, "KOSPI200") +
                       tag(54, "1") + tag(38, "10") + tag(44, "350.25") +
                       tag(40, "2") + tag(59, "0") + tag(111, "5");
    std::string msg = make_fix_msg("D", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto nos = KrxNewOrderSingle::from_fix(result.value());
    EXPECT_EQ(nos.cl_ord_id, "42");
    EXPECT_EQ(nos.account, "100");
    EXPECT_EQ(nos.symbol, "KOSPI200");
    EXPECT_EQ(nos.side, '1');
    EXPECT_EQ(nos.order_qty, 10);
    EXPECT_DOUBLE_EQ(nos.price, 350.25);
    EXPECT_EQ(nos.ord_type, '2');
    EXPECT_EQ(nos.time_in_force, '0');
    EXPECT_EQ(nos.max_floor, 5);
}

TEST(KrxNewOrderSingleTest, ParsesKrxCustomTags) {
    std::string body = tag(11, "99") + tag(54, "2") + tag(40, "2") +
                       tag(44, "100") + tag(38, "5") + tag(59, "0") +
                       tag(55, "KOSPI200") +
                       tag(5001, "1") + tag(5002, "FRG") + tag(5003, "KOSPI200");
    std::string msg = make_fix_msg("D", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto nos = KrxNewOrderSingle::from_fix(result.value());
    EXPECT_EQ(nos.program_trading, '1');
    EXPECT_EQ(nos.investor_type, "FRG");
    EXPECT_EQ(nos.board_id, "KOSPI200");
}

TEST(KrxNewOrderSingleTest, MissingCustomTagsDefaultEmpty) {
    std::string body = tag(11, "1") + tag(54, "1") + tag(40, "2") +
                       tag(44, "100") + tag(38, "10") + tag(59, "0") +
                       tag(55, "KTB");
    std::string msg = make_fix_msg("D", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto nos = KrxNewOrderSingle::from_fix(result.value());
    EXPECT_EQ(nos.program_trading, '\0');
    EXPECT_EQ(nos.investor_type, "");
    EXPECT_EQ(nos.board_id, "");
}

// --- KrxCancelRequest ---

TEST(KrxCancelRequestTest, ParsesAllFields) {
    std::string body = tag(11, "50") + tag(41, "42") +
                       tag(55, "KOSPI200") + tag(54, "1");
    std::string msg = make_fix_msg("F", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto cr = KrxCancelRequest::from_fix(result.value());
    EXPECT_EQ(cr.cl_ord_id, "50");
    EXPECT_EQ(cr.orig_cl_ord_id, "42");
    EXPECT_EQ(cr.symbol, "KOSPI200");
    EXPECT_EQ(cr.side, '1');
}

// --- KrxCancelReplaceRequest ---

TEST(KrxCancelReplaceRequestTest, ParsesAllFields) {
    std::string body = tag(11, "60") + tag(41, "42") +
                       tag(55, "KOSPI200") + tag(54, "2") +
                       tag(38, "20") + tag(44, "355.00") + tag(40, "2");
    std::string msg = make_fix_msg("G", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto crr = KrxCancelReplaceRequest::from_fix(result.value());
    EXPECT_EQ(crr.cl_ord_id, "60");
    EXPECT_EQ(crr.orig_cl_ord_id, "42");
    EXPECT_EQ(crr.symbol, "KOSPI200");
    EXPECT_EQ(crr.side, '2');
    EXPECT_EQ(crr.order_qty, 20);
    EXPECT_DOUBLE_EQ(crr.price, 355.0);
    EXPECT_EQ(crr.ord_type, '2');
}

// --- KrxExecutionReport ---

TEST(KrxExecutionReportTest, ParsesAllFields) {
    std::string body = tag(11, "42") + tag(17, "1001") +
                       tag(150, "0") + tag(39, "0") +
                       tag(55, "KOSPI200") + tag(54, "1") +
                       tag(44, "350.25") + tag(38, "10") +
                       tag(5003, "KOSPI200");
    std::string msg = make_fix_msg("8", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto er = KrxExecutionReport::from_fix(result.value());
    EXPECT_EQ(er.cl_ord_id, "42");
    EXPECT_EQ(er.exec_id, "1001");
    EXPECT_EQ(er.exec_type, '0');
    EXPECT_EQ(er.ord_status, '0');
    EXPECT_EQ(er.symbol, "KOSPI200");
    EXPECT_EQ(er.side, '1');
    EXPECT_DOUBLE_EQ(er.price, 350.25);
    EXPECT_EQ(er.order_qty, 10);
    EXPECT_EQ(er.board_id, "KOSPI200");
}

TEST(KrxExecutionReportTest, MissingBoardIdDefaultsEmpty) {
    std::string body = tag(11, "42") + tag(17, "1001") +
                       tag(150, "8") + tag(39, "8") +
                       tag(55, "KTB") + tag(44, "0") + tag(38, "0");
    std::string msg = make_fix_msg("8", body);

    auto result = ::ice::fix::parse_fix_message(msg.data(), msg.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    auto er = KrxExecutionReport::from_fix(result.value());
    EXPECT_EQ(er.board_id, "");
}

}  // namespace
}  // namespace exchange::krx::fix
