#include "cme/codec/mdp3_messages.h"

#include <cstring>

#include "gtest/gtest.h"

namespace exchange::cme::sbe::mdp3 {
namespace {

// ---------------------------------------------------------------------------
// Struct size and layout validation
// ---------------------------------------------------------------------------

TEST(Mdp3MessagesTest, SecurityStatus30Size) {
    EXPECT_EQ(sizeof(SecurityStatus30), 30u);
    EXPECT_EQ(SecurityStatus30::TEMPLATE_ID, 30u);
    EXPECT_EQ(SecurityStatus30::BLOCK_LENGTH, 30u);
}

TEST(Mdp3MessagesTest, MDIncrementalRefreshBook46Size) {
    EXPECT_EQ(sizeof(MDIncrementalRefreshBook46), 11u);
    EXPECT_EQ(MDIncrementalRefreshBook46::TEMPLATE_ID, 46u);
    EXPECT_EQ(MDIncrementalRefreshBook46::BLOCK_LENGTH, 11u);
}

TEST(Mdp3MessagesTest, RefreshBookEntrySize) {
    EXPECT_EQ(sizeof(RefreshBookEntry), 32u);
}

TEST(Mdp3MessagesTest, RefreshBookOrderEntrySize) {
    EXPECT_EQ(sizeof(RefreshBookOrderEntry), 24u);
}

TEST(Mdp3MessagesTest, MDIncrementalRefreshTradeSummary48Size) {
    EXPECT_EQ(sizeof(MDIncrementalRefreshTradeSummary48), 11u);
    EXPECT_EQ(MDIncrementalRefreshTradeSummary48::TEMPLATE_ID, 48u);
}

TEST(Mdp3MessagesTest, TradeSummaryEntrySize) {
    EXPECT_EQ(sizeof(TradeSummaryEntry), 32u);
}

TEST(Mdp3MessagesTest, TradeSummaryOrderEntrySize) {
    EXPECT_EQ(sizeof(TradeSummaryOrderEntry), 16u);
}

TEST(Mdp3MessagesTest, SnapshotFullRefreshOrderBook53Size) {
    EXPECT_EQ(sizeof(SnapshotFullRefreshOrderBook53), 28u);
    EXPECT_EQ(SnapshotFullRefreshOrderBook53::TEMPLATE_ID, 53u);
}

TEST(Mdp3MessagesTest, SnapshotOrderBookEntrySize) {
    EXPECT_EQ(sizeof(SnapshotOrderBookEntry), 29u);
}

TEST(Mdp3MessagesTest, MDInstrumentDefinitionFuture54Size) {
    EXPECT_EQ(sizeof(MDInstrumentDefinitionFuture54), 224u);
    EXPECT_EQ(MDInstrumentDefinitionFuture54::TEMPLATE_ID, 54u);
    EXPECT_EQ(MDInstrumentDefinitionFuture54::BLOCK_LENGTH, 224u);
}

TEST(Mdp3MessagesTest, InstrDefGroupEntrySizes) {
    EXPECT_EQ(sizeof(InstrDefEventEntry), 9u);
    EXPECT_EQ(sizeof(InstrDefFeedTypeEntry), 4u);
    EXPECT_EQ(sizeof(InstrDefAttribEntry), 4u);
    EXPECT_EQ(sizeof(InstrDefLotTypeEntry), 5u);
}

TEST(Mdp3MessagesTest, GroupHeader8ByteSize) {
    EXPECT_EQ(sizeof(GroupHeader8Byte), 8u);
}

// ---------------------------------------------------------------------------
// Field offset verification via offsetof on packed structs
// ---------------------------------------------------------------------------

TEST(Mdp3MessagesTest, SecurityStatus30Offsets) {
    EXPECT_EQ(offsetof(SecurityStatus30, transact_time), 0u);
    EXPECT_EQ(offsetof(SecurityStatus30, security_group), 8u);
    EXPECT_EQ(offsetof(SecurityStatus30, asset), 14u);
    EXPECT_EQ(offsetof(SecurityStatus30, security_id), 20u);
    EXPECT_EQ(offsetof(SecurityStatus30, trade_date), 24u);
    EXPECT_EQ(offsetof(SecurityStatus30, match_event_indicator), 26u);
    EXPECT_EQ(offsetof(SecurityStatus30, security_trading_status), 27u);
    EXPECT_EQ(offsetof(SecurityStatus30, halt_reason), 28u);
    EXPECT_EQ(offsetof(SecurityStatus30, security_trading_event), 29u);
}

TEST(Mdp3MessagesTest, MDIncrementalRefreshBook46Offsets) {
    EXPECT_EQ(offsetof(MDIncrementalRefreshBook46, transact_time), 0u);
    EXPECT_EQ(offsetof(MDIncrementalRefreshBook46, match_event_indicator), 8u);
}

TEST(Mdp3MessagesTest, RefreshBookEntryOffsets) {
    EXPECT_EQ(offsetof(RefreshBookEntry, md_entry_px), 0u);
    EXPECT_EQ(offsetof(RefreshBookEntry, md_entry_size), 8u);
    EXPECT_EQ(offsetof(RefreshBookEntry, security_id), 12u);
    EXPECT_EQ(offsetof(RefreshBookEntry, rpt_seq), 16u);
    EXPECT_EQ(offsetof(RefreshBookEntry, number_of_orders), 20u);
    EXPECT_EQ(offsetof(RefreshBookEntry, md_price_level), 24u);
    EXPECT_EQ(offsetof(RefreshBookEntry, md_update_action), 25u);
    EXPECT_EQ(offsetof(RefreshBookEntry, md_entry_type), 26u);
    EXPECT_EQ(offsetof(RefreshBookEntry, tradeable_size), 27u);
}

TEST(Mdp3MessagesTest, TradeSummaryEntryOffsets) {
    EXPECT_EQ(offsetof(TradeSummaryEntry, md_entry_px), 0u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, md_entry_size), 8u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, security_id), 12u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, rpt_seq), 16u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, number_of_orders), 20u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, aggressor_side), 24u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, md_update_action), 25u);
    EXPECT_EQ(offsetof(TradeSummaryEntry, md_trade_entry_id), 26u);
}

TEST(Mdp3MessagesTest, SnapshotFullRefreshOrderBook53Offsets) {
    EXPECT_EQ(offsetof(SnapshotFullRefreshOrderBook53, last_msg_seq_num_processed), 0u);
    EXPECT_EQ(offsetof(SnapshotFullRefreshOrderBook53, tot_num_reports), 4u);
    EXPECT_EQ(offsetof(SnapshotFullRefreshOrderBook53, security_id), 8u);
    EXPECT_EQ(offsetof(SnapshotFullRefreshOrderBook53, no_chunks), 12u);
    EXPECT_EQ(offsetof(SnapshotFullRefreshOrderBook53, current_chunk), 16u);
    EXPECT_EQ(offsetof(SnapshotFullRefreshOrderBook53, transact_time), 20u);
}

TEST(Mdp3MessagesTest, SnapshotOrderBookEntryOffsets) {
    EXPECT_EQ(offsetof(SnapshotOrderBookEntry, order_id), 0u);
    EXPECT_EQ(offsetof(SnapshotOrderBookEntry, md_order_priority), 8u);
    EXPECT_EQ(offsetof(SnapshotOrderBookEntry, md_entry_px), 16u);
    EXPECT_EQ(offsetof(SnapshotOrderBookEntry, md_display_qty), 24u);
    EXPECT_EQ(offsetof(SnapshotOrderBookEntry, md_entry_type), 28u);
}

TEST(Mdp3MessagesTest, MDInstrumentDefinitionFuture54Offsets) {
    using T = MDInstrumentDefinitionFuture54;
    EXPECT_EQ(offsetof(T, match_event_indicator), 0u);
    EXPECT_EQ(offsetof(T, tot_num_reports), 1u);
    EXPECT_EQ(offsetof(T, security_update_action), 5u);
    EXPECT_EQ(offsetof(T, last_update_time), 6u);
    EXPECT_EQ(offsetof(T, md_security_trading_status), 14u);
    EXPECT_EQ(offsetof(T, appl_id), 15u);
    EXPECT_EQ(offsetof(T, market_segment_id), 17u);
    EXPECT_EQ(offsetof(T, underlying_product), 18u);
    EXPECT_EQ(offsetof(T, security_exchange), 19u);
    EXPECT_EQ(offsetof(T, security_group), 23u);
    EXPECT_EQ(offsetof(T, asset), 29u);
    EXPECT_EQ(offsetof(T, symbol), 35u);
    EXPECT_EQ(offsetof(T, security_id), 55u);
    EXPECT_EQ(offsetof(T, security_type), 59u);
    EXPECT_EQ(offsetof(T, cfi_code), 65u);
    EXPECT_EQ(offsetof(T, maturity_month_year), 71u);
    EXPECT_EQ(offsetof(T, currency), 76u);
    EXPECT_EQ(offsetof(T, settl_currency), 79u);
    EXPECT_EQ(offsetof(T, match_algorithm), 82u);
    EXPECT_EQ(offsetof(T, min_trade_vol), 83u);
    EXPECT_EQ(offsetof(T, max_trade_vol), 87u);
    EXPECT_EQ(offsetof(T, min_price_increment), 91u);
    EXPECT_EQ(offsetof(T, display_factor), 99u);
    EXPECT_EQ(offsetof(T, main_fraction), 107u);
    EXPECT_EQ(offsetof(T, sub_fraction), 108u);
    EXPECT_EQ(offsetof(T, price_display_format), 109u);
    EXPECT_EQ(offsetof(T, unit_of_measure), 110u);
    EXPECT_EQ(offsetof(T, unit_of_measure_qty), 140u);
    EXPECT_EQ(offsetof(T, trading_reference_price), 148u);
    EXPECT_EQ(offsetof(T, settl_price_type), 156u);
    EXPECT_EQ(offsetof(T, open_interest_qty), 157u);
    EXPECT_EQ(offsetof(T, cleared_volume), 161u);
    EXPECT_EQ(offsetof(T, high_limit_price), 165u);
    EXPECT_EQ(offsetof(T, low_limit_price), 173u);
    EXPECT_EQ(offsetof(T, max_price_variation), 181u);
    EXPECT_EQ(offsetof(T, decay_quantity), 189u);
    EXPECT_EQ(offsetof(T, decay_start_date), 193u);
    EXPECT_EQ(offsetof(T, original_contract_size), 195u);
    EXPECT_EQ(offsetof(T, contract_multiplier), 199u);
    EXPECT_EQ(offsetof(T, contract_multiplier_unit), 203u);
    EXPECT_EQ(offsetof(T, flow_schedule_type), 204u);
    EXPECT_EQ(offsetof(T, min_price_increment_amount), 205u);
    EXPECT_EQ(offsetof(T, user_defined_instrument), 213u);
    EXPECT_EQ(offsetof(T, trading_reference_date), 214u);
    EXPECT_EQ(offsetof(T, instrument_guid), 216u);
}

// ---------------------------------------------------------------------------
// make_header produces correct MDP3 headers
// ---------------------------------------------------------------------------

TEST(Mdp3MessagesTest, MakeHeaderSecurityStatus30) {
    auto hdr = make_header<SecurityStatus30>();
    EXPECT_EQ(hdr.block_length, 30u);
    EXPECT_EQ(hdr.template_id, 30u);
    EXPECT_EQ(hdr.schema_id, MDP3_SCHEMA_ID);
    EXPECT_EQ(hdr.version, MDP3_VERSION);
}

TEST(Mdp3MessagesTest, MakeHeaderBook46) {
    auto hdr = make_header<MDIncrementalRefreshBook46>();
    EXPECT_EQ(hdr.block_length, 11u);
    EXPECT_EQ(hdr.template_id, 46u);
}

TEST(Mdp3MessagesTest, MakeHeaderTrade48) {
    auto hdr = make_header<MDIncrementalRefreshTradeSummary48>();
    EXPECT_EQ(hdr.block_length, 11u);
    EXPECT_EQ(hdr.template_id, 48u);
}

TEST(Mdp3MessagesTest, MakeHeaderSnapshot53) {
    auto hdr = make_header<SnapshotFullRefreshOrderBook53>();
    EXPECT_EQ(hdr.block_length, 28u);
    EXPECT_EQ(hdr.template_id, 53u);
}

TEST(Mdp3MessagesTest, MakeHeaderInstrDef54) {
    auto hdr = make_header<MDInstrumentDefinitionFuture54>();
    EXPECT_EQ(hdr.block_length, 224u);
    EXPECT_EQ(hdr.template_id, 54u);
}

// ---------------------------------------------------------------------------
// Populate and memcpy round-trip (verify no padding corruption)
// ---------------------------------------------------------------------------

TEST(Mdp3MessagesTest, SecurityStatus30RoundTrip) {
    SecurityStatus30 msg{};
    msg.transact_time = 1234567890000000000ULL;
    std::memcpy(msg.security_group, "ES    ", 6);
    std::memcpy(msg.asset, "ES    ", 6);
    msg.security_id = 12345;
    msg.trade_date = 19000;
    msg.match_event_indicator = 0x80; // EndOfEvent
    msg.security_trading_status = static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade);
    msg.halt_reason = static_cast<uint8_t>(HaltReason::GroupSchedule);
    msg.security_trading_event = static_cast<uint8_t>(SecurityTradingEvent::NoEvent);

    char buf[sizeof(SecurityStatus30)];
    std::memcpy(buf, &msg, sizeof(msg));

    SecurityStatus30 decoded{};
    std::memcpy(&decoded, buf, sizeof(decoded));

    EXPECT_EQ(decoded.transact_time, 1234567890000000000ULL);
    EXPECT_EQ(decoded.security_id, 12345);
    EXPECT_EQ(decoded.match_event_indicator, 0x80);
    EXPECT_EQ(decoded.security_trading_status,
              static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade));
}

TEST(Mdp3MessagesTest, RefreshBookEntryRoundTrip) {
    RefreshBookEntry entry{};
    entry.md_entry_px = PRICE9::from_double(4500.25);
    entry.md_entry_size = 100;
    entry.security_id = 12345;
    entry.rpt_seq = 42;
    entry.number_of_orders = 5;
    entry.md_price_level = 1;
    entry.md_update_action = static_cast<uint8_t>(MDUpdateAction::New);
    entry.md_entry_type = static_cast<char>(MDEntryTypeBook::Bid);
    entry.tradeable_size = 100;

    char buf[sizeof(RefreshBookEntry)];
    std::memcpy(buf, &entry, sizeof(entry));

    RefreshBookEntry decoded{};
    std::memcpy(&decoded, buf, sizeof(decoded));

    EXPECT_DOUBLE_EQ(decoded.md_entry_px.to_double(), 4500.25);
    EXPECT_EQ(decoded.md_entry_size, 100);
    EXPECT_EQ(decoded.security_id, 12345);
    EXPECT_EQ(decoded.md_price_level, 1u);
    EXPECT_EQ(decoded.md_entry_type, '0');
    EXPECT_EQ(decoded.tradeable_size, 100);
}

TEST(Mdp3MessagesTest, InstrumentDefinitionRoundTrip) {
    MDInstrumentDefinitionFuture54 msg{};
    msg.match_event_indicator = 0x80;
    msg.tot_num_reports = 500;
    msg.security_update_action = static_cast<char>(SecurityUpdateAction::Add);
    msg.last_update_time = 9876543210ULL;
    msg.security_id = 54321;
    std::memcpy(msg.symbol, "ESH6", 4);
    msg.min_price_increment = PRICE9::from_double(0.25);
    msg.min_trade_vol = 1;
    msg.max_trade_vol = 10000;
    msg.maturity_month_year = {2026, 3, 255, 255};
    msg.instrument_guid = UINT64_NULL; // optional, null

    char buf[sizeof(MDInstrumentDefinitionFuture54)];
    std::memcpy(buf, &msg, sizeof(msg));

    MDInstrumentDefinitionFuture54 decoded{};
    std::memcpy(&decoded, buf, sizeof(decoded));

    EXPECT_EQ(decoded.security_id, 54321);
    EXPECT_EQ(std::string(decoded.symbol, 4), "ESH6");
    EXPECT_DOUBLE_EQ(decoded.min_price_increment.to_double(), 0.25);
    EXPECT_EQ(decoded.maturity_month_year.year, 2026u);
    EXPECT_EQ(decoded.maturity_month_year.month, 3u);
    EXPECT_TRUE(is_null(decoded.instrument_guid));
}

// ---------------------------------------------------------------------------
// GroupHeader8Byte encode/decode round-trip
// ---------------------------------------------------------------------------

TEST(Mdp3MessagesTest, GroupHeader8ByteRoundTrip) {
    GroupHeader8Byte hdr{};
    hdr.block_length = 24;
    hdr.num_in_group = 3;
    std::memset(hdr.padding, 0, sizeof(hdr.padding));

    char buf[8];
    hdr.encode_to(buf);

    GroupHeader8Byte decoded{};
    GroupHeader8Byte::decode_from(buf, decoded);

    EXPECT_EQ(decoded.block_length, 24u);
    EXPECT_EQ(decoded.num_in_group, 3u);
}

// ---------------------------------------------------------------------------
// Enum value tests
// ---------------------------------------------------------------------------

TEST(Mdp3MessagesTest, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(MDUpdateAction::New), 0u);
    EXPECT_EQ(static_cast<uint8_t>(MDUpdateAction::Change), 1u);
    EXPECT_EQ(static_cast<uint8_t>(MDUpdateAction::Delete), 2u);
    EXPECT_EQ(static_cast<uint8_t>(MDUpdateAction::Overlay), 5u);

    EXPECT_EQ(static_cast<uint8_t>(AggressorSide::NoAggressor), 0u);
    EXPECT_EQ(static_cast<uint8_t>(AggressorSide::Buy), 1u);
    EXPECT_EQ(static_cast<uint8_t>(AggressorSide::Sell), 2u);

    EXPECT_EQ(static_cast<char>(MDEntryTypeBook::Bid), '0');
    EXPECT_EQ(static_cast<char>(MDEntryTypeBook::Offer), '1');
    EXPECT_EQ(static_cast<char>(MDEntryTypeBook::BookReset), 'J');

    EXPECT_EQ(static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade), 17u);
    EXPECT_EQ(static_cast<uint8_t>(SecurityTradingStatus::TradingHalt), 2u);

    EXPECT_EQ(static_cast<char>(SecurityUpdateAction::Add), 'A');
    EXPECT_EQ(static_cast<char>(SecurityUpdateAction::Delete), 'D');
}

}  // namespace
}  // namespace exchange::cme::sbe::mdp3
