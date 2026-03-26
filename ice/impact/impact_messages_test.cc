#include "ice/impact/impact_messages.h"

#include <cstddef>
#include <cstring>

#include "gtest/gtest.h"

namespace exchange::ice::impact {
namespace {

// ---------------------------------------------------------------------------
// Struct size validation
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, HeaderSize) {
    EXPECT_EQ(sizeof(ImpactMessageHeader), 3u);
}

TEST(ImpactMessagesTest, AddModifyOrderSize) {
    EXPECT_EQ(sizeof(AddModifyOrder), 39u);
}

TEST(ImpactMessagesTest, OrderWithdrawalSize) {
    EXPECT_EQ(sizeof(OrderWithdrawal), 29u);
}

TEST(ImpactMessagesTest, DealTradeSize) {
    EXPECT_EQ(sizeof(DealTrade), 42u);
}

TEST(ImpactMessagesTest, MarketStatusSize) {
    EXPECT_EQ(sizeof(MarketStatus), 5u);
}

TEST(ImpactMessagesTest, BundleStartSize) {
    EXPECT_EQ(sizeof(BundleStart), 4u);
}

TEST(ImpactMessagesTest, BundleEndSize) {
    EXPECT_EQ(sizeof(BundleEnd), 6u);
}

// ---------------------------------------------------------------------------
// Field offset verification
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, HeaderOffsets) {
    EXPECT_EQ(offsetof(ImpactMessageHeader, msg_type), 0u);
    EXPECT_EQ(offsetof(ImpactMessageHeader, msg_length), 1u);
}

TEST(ImpactMessagesTest, AddModifyOrderOffsets) {
    EXPECT_EQ(offsetof(AddModifyOrder, market_id), 0u);
    EXPECT_EQ(offsetof(AddModifyOrder, order_id), 4u);
    EXPECT_EQ(offsetof(AddModifyOrder, order_seq_id), 12u);
    EXPECT_EQ(offsetof(AddModifyOrder, side), 16u);
    EXPECT_EQ(offsetof(AddModifyOrder, price), 17u);
    EXPECT_EQ(offsetof(AddModifyOrder, quantity), 25u);
    EXPECT_EQ(offsetof(AddModifyOrder, is_implied), 29u);
    EXPECT_EQ(offsetof(AddModifyOrder, is_rfq), 30u);
    EXPECT_EQ(offsetof(AddModifyOrder, order_entry_date_time), 31u);
}

TEST(ImpactMessagesTest, OrderWithdrawalOffsets) {
    EXPECT_EQ(offsetof(OrderWithdrawal, market_id), 0u);
    EXPECT_EQ(offsetof(OrderWithdrawal, order_id), 4u);
    EXPECT_EQ(offsetof(OrderWithdrawal, order_seq_id), 12u);
    EXPECT_EQ(offsetof(OrderWithdrawal, side), 16u);
    EXPECT_EQ(offsetof(OrderWithdrawal, price), 17u);
    EXPECT_EQ(offsetof(OrderWithdrawal, quantity), 25u);
}

TEST(ImpactMessagesTest, DealTradeOffsets) {
    EXPECT_EQ(offsetof(DealTrade, market_id), 0u);
    EXPECT_EQ(offsetof(DealTrade, order_id), 4u);
    EXPECT_EQ(offsetof(DealTrade, deal_id), 12u);
    EXPECT_EQ(offsetof(DealTrade, price), 20u);
    EXPECT_EQ(offsetof(DealTrade, quantity), 28u);
    EXPECT_EQ(offsetof(DealTrade, is_aggressor), 32u);
    EXPECT_EQ(offsetof(DealTrade, aggressor_side), 33u);
    EXPECT_EQ(offsetof(DealTrade, deal_date_time), 34u);
}

TEST(ImpactMessagesTest, MarketStatusOffsets) {
    EXPECT_EQ(offsetof(MarketStatus, market_id), 0u);
    EXPECT_EQ(offsetof(MarketStatus, trading_status), 4u);
}

TEST(ImpactMessagesTest, BundleStartOffsets) {
    EXPECT_EQ(offsetof(BundleStart, seq_num), 0u);
}

TEST(ImpactMessagesTest, BundleEndOffsets) {
    EXPECT_EQ(offsetof(BundleEnd, seq_num), 0u);
    EXPECT_EQ(offsetof(BundleEnd, msg_count), 4u);
}

// ---------------------------------------------------------------------------
// Wire size helper
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, WireSizes) {
    EXPECT_EQ(wire_size<AddModifyOrder>(), 3u + 39u);
    EXPECT_EQ(wire_size<OrderWithdrawal>(), 3u + 29u);
    EXPECT_EQ(wire_size<DealTrade>(), 3u + 42u);
    EXPECT_EQ(wire_size<MarketStatus>(), 3u + 5u);
    EXPECT_EQ(wire_size<BundleStart>(), 3u + 4u);
    EXPECT_EQ(wire_size<BundleEnd>(), 3u + 6u);
}

// ---------------------------------------------------------------------------
// Encode / decode round-trip tests
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, AddModifyOrderRoundTrip) {
    AddModifyOrder msg{};
    msg.market_id = 12345;
    msg.order_id = 987654321LL;
    msg.order_seq_id = 42;
    msg.side = SIDE_BUY;
    msg.price = 750025;
    msg.quantity = 100;
    msg.is_implied = FLAG_NO;
    msg.is_rfq = FLAG_NO;
    msg.order_entry_date_time = 1711400000000000000LL;

    char buf[128];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, wire_size<AddModifyOrder>());

    AddModifyOrder decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);
    EXPECT_EQ(read_end - buf, wire_size<AddModifyOrder>());

    EXPECT_EQ(decoded.market_id, 12345);
    EXPECT_EQ(decoded.order_id, 987654321LL);
    EXPECT_EQ(decoded.order_seq_id, 42);
    EXPECT_EQ(decoded.side, SIDE_BUY);
    EXPECT_EQ(decoded.price, 750025);
    EXPECT_EQ(decoded.quantity, 100);
    EXPECT_EQ(decoded.is_implied, FLAG_NO);
    EXPECT_EQ(decoded.is_rfq, FLAG_NO);
    EXPECT_EQ(decoded.order_entry_date_time, 1711400000000000000LL);
}

TEST(ImpactMessagesTest, OrderWithdrawalRoundTrip) {
    OrderWithdrawal msg{};
    msg.market_id = 54321;
    msg.order_id = 111222333LL;
    msg.order_seq_id = 7;
    msg.side = SIDE_SELL;
    msg.price = 750050;
    msg.quantity = 50;

    char buf[64];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    OrderWithdrawal decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.market_id, 54321);
    EXPECT_EQ(decoded.order_id, 111222333LL);
    EXPECT_EQ(decoded.order_seq_id, 7);
    EXPECT_EQ(decoded.side, SIDE_SELL);
    EXPECT_EQ(decoded.price, 750050);
    EXPECT_EQ(decoded.quantity, 50);
}

TEST(ImpactMessagesTest, DealTradeRoundTrip) {
    DealTrade msg{};
    msg.market_id = 99999;
    msg.order_id = 444555666LL;
    msg.deal_id = 777888999LL;
    msg.price = 750075;
    msg.quantity = 25;
    msg.is_aggressor = FLAG_YES;
    msg.aggressor_side = SIDE_BUY;
    msg.deal_date_time = 1711400001000000000LL;

    char buf[64];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    DealTrade decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.market_id, 99999);
    EXPECT_EQ(decoded.order_id, 444555666LL);
    EXPECT_EQ(decoded.deal_id, 777888999LL);
    EXPECT_EQ(decoded.price, 750075);
    EXPECT_EQ(decoded.quantity, 25);
    EXPECT_EQ(decoded.is_aggressor, FLAG_YES);
    EXPECT_EQ(decoded.aggressor_side, SIDE_BUY);
    EXPECT_EQ(decoded.deal_date_time, 1711400001000000000LL);
}

TEST(ImpactMessagesTest, MarketStatusRoundTrip) {
    MarketStatus msg{};
    msg.market_id = 12345;
    msg.trading_status = TRADING_STATUS_OPEN;

    char buf[16];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    MarketStatus decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.market_id, 12345);
    EXPECT_EQ(decoded.trading_status, TRADING_STATUS_OPEN);
}

TEST(ImpactMessagesTest, BundleStartRoundTrip) {
    BundleStart msg{};
    msg.seq_num = 1000;

    char buf[16];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    BundleStart decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.seq_num, 1000);
}

TEST(ImpactMessagesTest, BundleEndRoundTrip) {
    BundleEnd msg{};
    msg.seq_num = 1000;
    msg.msg_count = 5;

    char buf[16];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    BundleEnd decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.seq_num, 1000);
    EXPECT_EQ(decoded.msg_count, 5u);
}

// ---------------------------------------------------------------------------
// Encode failure: buffer too small
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, EncodeBufferTooSmall) {
    AddModifyOrder msg{};
    char buf[2];  // way too small
    auto* end = encode(buf, sizeof(buf), msg);
    EXPECT_EQ(end, nullptr);
}

// ---------------------------------------------------------------------------
// Decode failure: buffer too small
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, DecodeBufferTooSmall) {
    AddModifyOrder msg{};
    char buf[128];
    encode(buf, sizeof(buf), msg);

    AddModifyOrder decoded{};
    auto* end = decode(buf, 2, decoded);  // truncated buffer
    EXPECT_EQ(end, nullptr);
}

// ---------------------------------------------------------------------------
// Decode failure: wrong message type
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, DecodeWrongType) {
    AddModifyOrder msg{};
    msg.market_id = 100;
    char buf[128];
    encode(buf, sizeof(buf), msg);

    // Try to decode as OrderWithdrawal — should fail on type mismatch
    OrderWithdrawal decoded{};
    auto* end = decode(buf, sizeof(buf), decoded);
    EXPECT_EQ(end, nullptr);
}

// ---------------------------------------------------------------------------
// Header field verification after encode
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, EncodedHeaderFields) {
    DealTrade msg{};
    msg.market_id = 1;
    char buf[64];
    encode(buf, sizeof(buf), msg);

    ImpactMessageHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));

    EXPECT_EQ(hdr.msg_type, MSG_TYPE_DEAL_TRADE);
    EXPECT_EQ(hdr.msg_length, wire_size<DealTrade>());
}

// ---------------------------------------------------------------------------
// All trading status codes
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, TradingStatusCodes) {
    EXPECT_EQ(TRADING_STATUS_OPEN, 'O');
    EXPECT_EQ(TRADING_STATUS_CLOSED, 'C');
    EXPECT_EQ(TRADING_STATUS_PRE_OPEN, 'P');
    EXPECT_EQ(TRADING_STATUS_HALT, 'H');
    EXPECT_EQ(TRADING_STATUS_SETTLEMENT, 'S');
}

// ---------------------------------------------------------------------------
// Message type constants
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, MessageTypeConstants) {
    EXPECT_EQ(AddModifyOrder::TYPE, 'E');
    EXPECT_EQ(OrderWithdrawal::TYPE, 'F');
    EXPECT_EQ(DealTrade::TYPE, 'T');
    EXPECT_EQ(MarketStatus::TYPE, 'M');
    EXPECT_EQ(BundleStart::TYPE, 'S');
}

}  // namespace
}  // namespace exchange::ice::impact
