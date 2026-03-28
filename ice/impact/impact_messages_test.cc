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

TEST(ImpactMessagesTest, BundleStartSize) {
    EXPECT_EQ(sizeof(BundleStart), 14u);
}

TEST(ImpactMessagesTest, BundleEndSize) {
    EXPECT_EQ(sizeof(BundleEnd), 4u);
}

TEST(ImpactMessagesTest, AddModifyOrderSize) {
    EXPECT_EQ(sizeof(AddModifyOrder), 39u);
}

TEST(ImpactMessagesTest, OrderWithdrawalSize) {
    EXPECT_EQ(sizeof(OrderWithdrawal), 29u);
}

TEST(ImpactMessagesTest, DealTradeSize) {
    EXPECT_EQ(sizeof(DealTrade), 33u);
}

TEST(ImpactMessagesTest, MarketStatusSize) {
    EXPECT_EQ(sizeof(MarketStatus), 5u);
}

TEST(ImpactMessagesTest, SnapshotOrderSize) {
    EXPECT_EQ(sizeof(SnapshotOrder), 29u);
}

TEST(ImpactMessagesTest, PriceLevelSize) {
    EXPECT_EQ(sizeof(PriceLevel), 19u);
}

TEST(ImpactMessagesTest, InstrumentDefinitionSize) {
    EXPECT_EQ(sizeof(InstrumentDefinition), 89u);
}

// ---------------------------------------------------------------------------
// Field offset verification
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, HeaderOffsets) {
    EXPECT_EQ(offsetof(ImpactMessageHeader, msg_type), 0u);
    EXPECT_EQ(offsetof(ImpactMessageHeader, body_length), 1u);
}

TEST(ImpactMessagesTest, BundleStartOffsets) {
    EXPECT_EQ(offsetof(BundleStart, sequence_number), 0u);
    EXPECT_EQ(offsetof(BundleStart, message_count), 4u);
    EXPECT_EQ(offsetof(BundleStart, timestamp), 6u);
}

TEST(ImpactMessagesTest, BundleEndOffsets) {
    EXPECT_EQ(offsetof(BundleEnd, sequence_number), 0u);
}

TEST(ImpactMessagesTest, AddModifyOrderOffsets) {
    EXPECT_EQ(offsetof(AddModifyOrder, instrument_id), 0u);
    EXPECT_EQ(offsetof(AddModifyOrder, order_id), 4u);
    EXPECT_EQ(offsetof(AddModifyOrder, sequence_within_msg), 12u);
    EXPECT_EQ(offsetof(AddModifyOrder, side), 16u);
    EXPECT_EQ(offsetof(AddModifyOrder, price), 17u);
    EXPECT_EQ(offsetof(AddModifyOrder, quantity), 25u);
    EXPECT_EQ(offsetof(AddModifyOrder, is_implied), 29u);
    EXPECT_EQ(offsetof(AddModifyOrder, is_rfq), 30u);
    EXPECT_EQ(offsetof(AddModifyOrder, order_entry_date_time), 31u);
}

TEST(ImpactMessagesTest, OrderWithdrawalOffsets) {
    EXPECT_EQ(offsetof(OrderWithdrawal, instrument_id), 0u);
    EXPECT_EQ(offsetof(OrderWithdrawal, order_id), 4u);
    EXPECT_EQ(offsetof(OrderWithdrawal, sequence_within_msg), 12u);
    EXPECT_EQ(offsetof(OrderWithdrawal, side), 16u);
    EXPECT_EQ(offsetof(OrderWithdrawal, price), 17u);
    EXPECT_EQ(offsetof(OrderWithdrawal, quantity), 25u);
}

TEST(ImpactMessagesTest, DealTradeOffsets) {
    EXPECT_EQ(offsetof(DealTrade, instrument_id), 0u);
    EXPECT_EQ(offsetof(DealTrade, deal_id), 4u);
    EXPECT_EQ(offsetof(DealTrade, price), 12u);
    EXPECT_EQ(offsetof(DealTrade, quantity), 20u);
    EXPECT_EQ(offsetof(DealTrade, aggressor_side), 24u);
    EXPECT_EQ(offsetof(DealTrade, timestamp), 25u);
}

TEST(ImpactMessagesTest, MarketStatusOffsets) {
    EXPECT_EQ(offsetof(MarketStatus, instrument_id), 0u);
    EXPECT_EQ(offsetof(MarketStatus, trading_status), 4u);
}

TEST(ImpactMessagesTest, SnapshotOrderOffsets) {
    EXPECT_EQ(offsetof(SnapshotOrder, instrument_id), 0u);
    EXPECT_EQ(offsetof(SnapshotOrder, order_id), 4u);
    EXPECT_EQ(offsetof(SnapshotOrder, side), 12u);
    EXPECT_EQ(offsetof(SnapshotOrder, price), 13u);
    EXPECT_EQ(offsetof(SnapshotOrder, quantity), 21u);
    EXPECT_EQ(offsetof(SnapshotOrder, sequence), 25u);
}

TEST(ImpactMessagesTest, PriceLevelOffsets) {
    EXPECT_EQ(offsetof(PriceLevel, instrument_id), 0u);
    EXPECT_EQ(offsetof(PriceLevel, side), 4u);
    EXPECT_EQ(offsetof(PriceLevel, price), 5u);
    EXPECT_EQ(offsetof(PriceLevel, quantity), 13u);
    EXPECT_EQ(offsetof(PriceLevel, order_count), 17u);
}

TEST(ImpactMessagesTest, InstrumentDefinitionOffsets) {
    EXPECT_EQ(offsetof(InstrumentDefinition, instrument_id), 0u);
    EXPECT_EQ(offsetof(InstrumentDefinition, symbol), 4u);
    EXPECT_EQ(offsetof(InstrumentDefinition, description), 12u);
    EXPECT_EQ(offsetof(InstrumentDefinition, product_group), 44u);
    EXPECT_EQ(offsetof(InstrumentDefinition, tick_size), 60u);
    EXPECT_EQ(offsetof(InstrumentDefinition, lot_size), 68u);
    EXPECT_EQ(offsetof(InstrumentDefinition, max_order_size), 76u);
    EXPECT_EQ(offsetof(InstrumentDefinition, match_algo), 84u);
    EXPECT_EQ(offsetof(InstrumentDefinition, currency), 85u);
}

// ---------------------------------------------------------------------------
// Wire size helpers
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, WireSizes) {
    EXPECT_EQ(wire_size<BundleStart>(), 3u + 14u);
    EXPECT_EQ(wire_size<BundleEnd>(), 3u + 4u);
    EXPECT_EQ(wire_size<AddModifyOrder>(), 3u + 39u);
    EXPECT_EQ(wire_size<OrderWithdrawal>(), 3u + 29u);
    EXPECT_EQ(wire_size<DealTrade>(), 3u + 33u);
    EXPECT_EQ(wire_size<MarketStatus>(), 3u + 5u);
    EXPECT_EQ(wire_size<SnapshotOrder>(), 3u + 29u);
    EXPECT_EQ(wire_size<PriceLevel>(), 3u + 19u);
    EXPECT_EQ(wire_size<InstrumentDefinition>(), 3u + 89u);
}

// ---------------------------------------------------------------------------
// Enum values
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, SideEnum) {
    EXPECT_EQ(static_cast<uint8_t>(Side::Buy), 0u);
    EXPECT_EQ(static_cast<uint8_t>(Side::Sell), 1u);
}

TEST(ImpactMessagesTest, TradingStatusEnum) {
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::PreOpen), 0u);
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::Continuous), 1u);
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::Halt), 2u);
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::Closed), 3u);
    EXPECT_EQ(static_cast<uint8_t>(TradingStatus::Settlement), 4u);
}

TEST(ImpactMessagesTest, MessageTypeEnum) {
    EXPECT_EQ(static_cast<char>(MessageType::AddModifyOrder), 'E');
    EXPECT_EQ(static_cast<char>(MessageType::OrderWithdrawal), 'F');
    EXPECT_EQ(static_cast<char>(MessageType::DealTrade), 'T');
    EXPECT_EQ(static_cast<char>(MessageType::MarketStatus), 'M');
    EXPECT_EQ(static_cast<char>(MessageType::BundleStart), 'S');
    EXPECT_EQ(static_cast<char>(MessageType::SnapshotOrder), 'D');
    EXPECT_EQ(static_cast<char>(MessageType::PriceLevel), 'L');
    EXPECT_EQ(static_cast<char>(MessageType::InstrumentDefinition), 'I');
}

TEST(ImpactMessagesTest, YesNoEnum) {
    EXPECT_EQ(static_cast<uint8_t>(YesNo::No), 0u);
    EXPECT_EQ(static_cast<uint8_t>(YesNo::Yes), 1u);
}

// ---------------------------------------------------------------------------
// TYPE constants match MessageType enum
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, TypeConstants) {
    EXPECT_EQ(AddModifyOrder::TYPE, 'E');
    EXPECT_EQ(OrderWithdrawal::TYPE, 'F');
    EXPECT_EQ(DealTrade::TYPE, 'T');
    EXPECT_EQ(MarketStatus::TYPE, 'M');
    EXPECT_EQ(BundleStart::TYPE, 'S');
    EXPECT_EQ(SnapshotOrder::TYPE, 'D');
    EXPECT_EQ(PriceLevel::TYPE, 'L');
    EXPECT_EQ(InstrumentDefinition::TYPE, 'I');
}

// ---------------------------------------------------------------------------
// Encode / decode round-trip tests
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, BundleStartRoundTrip) {
    BundleStart msg{};
    msg.sequence_number = 42000;
    msg.message_count = 5;
    msg.timestamp = 1711400000000000000LL;

    char buf[32];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, wire_size<BundleStart>());

    BundleStart decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.sequence_number, 42000u);
    EXPECT_EQ(decoded.message_count, 5u);
    EXPECT_EQ(decoded.timestamp, 1711400000000000000LL);
}

TEST(ImpactMessagesTest, BundleEndRoundTrip) {
    BundleEnd msg{};
    msg.sequence_number = 42000;

    char buf[16];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    BundleEnd decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.sequence_number, 42000u);
}

TEST(ImpactMessagesTest, AddModifyOrderRoundTrip) {
    AddModifyOrder msg{};
    msg.instrument_id = 12345;
    msg.order_id = 987654321LL;
    msg.sequence_within_msg = 42;
    msg.side = static_cast<uint8_t>(Side::Buy);
    msg.price = 750025;
    msg.quantity = 100;
    msg.is_implied = static_cast<uint8_t>(YesNo::No);
    msg.is_rfq = static_cast<uint8_t>(YesNo::No);
    msg.order_entry_date_time = 1711400000000000000LL;

    char buf[128];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, wire_size<AddModifyOrder>());

    AddModifyOrder decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 12345);
    EXPECT_EQ(decoded.order_id, 987654321LL);
    EXPECT_EQ(decoded.sequence_within_msg, 42u);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(decoded.price, 750025);
    EXPECT_EQ(decoded.quantity, 100u);
    EXPECT_EQ(decoded.is_implied, static_cast<uint8_t>(YesNo::No));
    EXPECT_EQ(decoded.is_rfq, static_cast<uint8_t>(YesNo::No));
    EXPECT_EQ(decoded.order_entry_date_time, 1711400000000000000LL);
}

TEST(ImpactMessagesTest, OrderWithdrawalRoundTrip) {
    OrderWithdrawal msg{};
    msg.instrument_id = 54321;
    msg.order_id = 111222333LL;
    msg.sequence_within_msg = 7;
    msg.side = static_cast<uint8_t>(Side::Sell);
    msg.price = 750050;
    msg.quantity = 50;

    char buf[64];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    OrderWithdrawal decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 54321);
    EXPECT_EQ(decoded.order_id, 111222333LL);
    EXPECT_EQ(decoded.sequence_within_msg, 7u);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(decoded.price, 750050);
    EXPECT_EQ(decoded.quantity, 50u);
}

TEST(ImpactMessagesTest, DealTradeRoundTrip) {
    DealTrade msg{};
    msg.instrument_id = 99999;
    msg.deal_id = 777888999LL;
    msg.price = 750075;
    msg.quantity = 25;
    msg.aggressor_side = static_cast<uint8_t>(Side::Buy);
    msg.timestamp = 1711400001000000000LL;

    char buf[64];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    DealTrade decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 99999);
    EXPECT_EQ(decoded.deal_id, 777888999LL);
    EXPECT_EQ(decoded.price, 750075);
    EXPECT_EQ(decoded.quantity, 25u);
    EXPECT_EQ(decoded.aggressor_side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(decoded.timestamp, 1711400001000000000LL);
}

TEST(ImpactMessagesTest, MarketStatusRoundTrip) {
    MarketStatus msg{};
    msg.instrument_id = 12345;
    msg.trading_status = static_cast<uint8_t>(TradingStatus::Continuous);

    char buf[16];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    MarketStatus decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 12345);
    EXPECT_EQ(decoded.trading_status, static_cast<uint8_t>(TradingStatus::Continuous));
}

TEST(ImpactMessagesTest, SnapshotOrderRoundTrip) {
    SnapshotOrder msg{};
    msg.instrument_id = 10001;
    msg.order_id = 5555666677LL;
    msg.side = static_cast<uint8_t>(Side::Sell);
    msg.price = 800100;
    msg.quantity = 200;
    msg.sequence = 3;

    char buf[64];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    SnapshotOrder decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 10001);
    EXPECT_EQ(decoded.order_id, 5555666677LL);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(decoded.price, 800100);
    EXPECT_EQ(decoded.quantity, 200u);
    EXPECT_EQ(decoded.sequence, 3u);
}

TEST(ImpactMessagesTest, PriceLevelRoundTrip) {
    PriceLevel msg{};
    msg.instrument_id = 20002;
    msg.side = static_cast<uint8_t>(Side::Buy);
    msg.price = 750000;
    msg.quantity = 500;
    msg.order_count = 12;

    char buf[32];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);

    PriceLevel decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 20002);
    EXPECT_EQ(decoded.side, static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(decoded.price, 750000);
    EXPECT_EQ(decoded.quantity, 500u);
    EXPECT_EQ(decoded.order_count, 12u);
}

TEST(ImpactMessagesTest, InstrumentDefinitionRoundTrip) {
    InstrumentDefinition msg{};
    msg.instrument_id = 1;
    std::memset(msg.symbol, 0, sizeof(msg.symbol));
    std::memcpy(msg.symbol, "B", 1);
    std::memset(msg.description, 0, sizeof(msg.description));
    std::memcpy(msg.description, "Brent Crude Futures", 19);
    std::memset(msg.product_group, 0, sizeof(msg.product_group));
    std::memcpy(msg.product_group, "Energy", 6);
    msg.tick_size = 100;
    msg.lot_size = 10000;
    msg.max_order_size = 50000000;
    msg.match_algo = 0;  // FIFO
    std::memset(msg.currency, 0, sizeof(msg.currency));
    std::memcpy(msg.currency, "USD", 3);

    char buf[128];
    auto* end = encode(buf, sizeof(buf), msg);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, wire_size<InstrumentDefinition>());

    InstrumentDefinition decoded{};
    auto* read_end = decode(buf, sizeof(buf), decoded);
    ASSERT_NE(read_end, nullptr);

    EXPECT_EQ(decoded.instrument_id, 1);
    EXPECT_EQ(std::string(decoded.symbol, 1), "B");
    EXPECT_EQ(decoded.symbol[1], '\0');
    EXPECT_EQ(std::string(decoded.description, 19), "Brent Crude Futures");
    EXPECT_EQ(std::string(decoded.product_group, 6), "Energy");
    EXPECT_EQ(decoded.tick_size, 100);
    EXPECT_EQ(decoded.lot_size, 10000);
    EXPECT_EQ(decoded.max_order_size, 50000000);
    EXPECT_EQ(decoded.match_algo, 0u);
    EXPECT_EQ(std::string(decoded.currency, 3), "USD");
}

// ---------------------------------------------------------------------------
// Encode failure: buffer too small
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, EncodeBufferTooSmall) {
    AddModifyOrder msg{};
    char buf[2];
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
    auto* end = decode(buf, 2, decoded);
    EXPECT_EQ(end, nullptr);
}

// ---------------------------------------------------------------------------
// Decode failure: wrong message type
// ---------------------------------------------------------------------------

TEST(ImpactMessagesTest, DecodeWrongType) {
    AddModifyOrder msg{};
    msg.instrument_id = 100;
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
    msg.instrument_id = 1;
    char buf[64];
    encode(buf, sizeof(buf), msg);

    ImpactMessageHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));

    EXPECT_EQ(hdr.msg_type, static_cast<char>(MessageType::DealTrade));
    EXPECT_EQ(hdr.body_length, wire_size<DealTrade>());
}

}  // namespace
}  // namespace exchange::ice::impact
