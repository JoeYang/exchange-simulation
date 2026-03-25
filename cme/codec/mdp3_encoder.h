#pragma once

#include "cme/codec/mdp3_messages.h"
#include "cme/codec/sbe_header.h"
#include "exchange-core/events.h"

#include <cstddef>
#include <cstring>

namespace exchange::cme::sbe::mdp3 {

// ---------------------------------------------------------------------------
// Price/qty conversion: engine fixed-point (PRICE_SCALE=10000) <-> SBE PRICE9
//
// engine_price * (1e9 / 10000) = engine_price * 100000
// ---------------------------------------------------------------------------

constexpr int64_t ENGINE_TO_PRICE9_FACTOR = 100000;  // 1e9 / PRICE_SCALE

inline PRICE9 engine_price_to_price9(Price p) {
    return PRICE9{p * ENGINE_TO_PRICE9_FACTOR};
}

inline PRICE9 engine_price_to_pricenull9(Price p) {
    if (p == 0) return PRICE9{INT64_NULL};
    return PRICE9{p * ENGINE_TO_PRICE9_FACTOR};
}

inline int32_t engine_qty_to_wire(Quantity q) {
    return static_cast<int32_t>(q / PRICE_SCALE);
}

// ---------------------------------------------------------------------------
// Enum conversion: engine enums -> MDP3 wire values
// ---------------------------------------------------------------------------

inline uint8_t encode_md_update_action(DepthUpdate::Action a) {
    switch (a) {
        case DepthUpdate::Add:    return static_cast<uint8_t>(MDUpdateAction::New);
        case DepthUpdate::Update: return static_cast<uint8_t>(MDUpdateAction::Change);
        case DepthUpdate::Remove: return static_cast<uint8_t>(MDUpdateAction::Delete);
    }
    return static_cast<uint8_t>(MDUpdateAction::New);
}

inline char encode_md_entry_type_book(Side side) {
    return (side == Side::Buy)
        ? static_cast<char>(MDEntryTypeBook::Bid)
        : static_cast<char>(MDEntryTypeBook::Offer);
}

inline uint8_t encode_aggressor_side(Side side) {
    return (side == Side::Buy)
        ? static_cast<uint8_t>(AggressorSide::Buy)
        : static_cast<uint8_t>(AggressorSide::Sell);
}

inline uint8_t encode_security_trading_status(SessionState state) {
    switch (state) {
        case SessionState::Closed:
            return static_cast<uint8_t>(SecurityTradingStatus::Close);
        case SessionState::PreOpen:
            return static_cast<uint8_t>(SecurityTradingStatus::PreOpen);
        case SessionState::OpeningAuction:
            return static_cast<uint8_t>(SecurityTradingStatus::PreCross);
        case SessionState::Continuous:
            return static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade);
        case SessionState::PreClose:
            return static_cast<uint8_t>(SecurityTradingStatus::PostClose);
        case SessionState::ClosingAuction:
            return static_cast<uint8_t>(SecurityTradingStatus::Cross);
        case SessionState::Halt:
            return static_cast<uint8_t>(SecurityTradingStatus::TradingHalt);
        case SessionState::VolatilityAuction:
            return static_cast<uint8_t>(SecurityTradingStatus::PreCross);
    }
    return static_cast<uint8_t>(SecurityTradingStatus::UnknownOrInvalid);
}

// ---------------------------------------------------------------------------
// Encode context — instrument-level state carried through encoding calls.
// ---------------------------------------------------------------------------

struct Mdp3EncodeContext {
    int32_t  security_id{0};
    uint32_t rpt_seq{0};       // per-instrument sequence number
    char     security_group[6]{};
    char     asset[6]{};
};

// ---------------------------------------------------------------------------
// Encoder functions.
//
// Each returns total bytes written into buf.
// Caller must ensure buf has at least MAX_MDP3_ENCODED_SIZE bytes.
// ---------------------------------------------------------------------------

// Encode DepthUpdate -> MDIncrementalRefreshBook46 with 1 entry.
// Wire layout: MessageHeader(8) + root(11) + GroupHeader(3) + entry(32)
//            + GroupHeader8Byte(8) + 0 order entries = 62 bytes.
inline size_t encode_depth_update(
    char* buf,
    const DepthUpdate& evt,
    Mdp3EncodeContext& ctx)
{
    auto hdr = make_header<MDIncrementalRefreshBook46>();
    char* p = hdr.encode_to(buf);

    // Root block.
    MDIncrementalRefreshBook46 root{};
    std::memset(&root, 0, sizeof(root));
    root.transact_time = static_cast<uint64_t>(evt.ts);
    root.match_event_indicator = 0x80;  // EndOfEvent
    std::memcpy(p, &root, sizeof(root));
    p += sizeof(root);

    // NoMDEntries group header (groupSize: 3 bytes).
    GroupHeader gh{};
    gh.block_length = RefreshBookEntry::BLOCK_LENGTH;
    gh.num_in_group = 1;
    p = gh.encode_to(p);

    // Single entry.
    RefreshBookEntry entry{};
    std::memset(&entry, 0, sizeof(entry));
    entry.md_entry_px = engine_price_to_price9(evt.price);
    entry.md_entry_size = engine_qty_to_wire(evt.total_qty);
    entry.security_id = ctx.security_id;
    entry.rpt_seq = ++ctx.rpt_seq;
    entry.number_of_orders = static_cast<int32_t>(evt.order_count);
    entry.md_price_level = 1;
    entry.md_update_action = encode_md_update_action(evt.action);
    entry.md_entry_type = encode_md_entry_type_book(evt.side);
    entry.tradeable_size = engine_qty_to_wire(evt.total_qty);
    std::memcpy(p, &entry, sizeof(entry));
    p += sizeof(entry);

    // Empty NoOrderIDEntries group (groupSize8Byte: 8 bytes).
    GroupHeader8Byte gh8{};
    std::memset(&gh8, 0, sizeof(gh8));
    gh8.block_length = RefreshBookOrderEntry::BLOCK_LENGTH;
    gh8.num_in_group = 0;
    p = gh8.encode_to(p);

    return static_cast<size_t>(p - buf);
}

// Encode TopOfBook -> MDIncrementalRefreshBook46 with 2 entries (bid + ask).
// Wire layout: MessageHeader(8) + root(11) + GroupHeader(3) + 2*entry(64)
//            + GroupHeader8Byte(8) = 94 bytes.
inline size_t encode_top_of_book(
    char* buf,
    const TopOfBook& evt,
    Mdp3EncodeContext& ctx)
{
    auto hdr = make_header<MDIncrementalRefreshBook46>();
    char* p = hdr.encode_to(buf);

    MDIncrementalRefreshBook46 root{};
    std::memset(&root, 0, sizeof(root));
    root.transact_time = static_cast<uint64_t>(evt.ts);
    root.match_event_indicator = 0x80;
    std::memcpy(p, &root, sizeof(root));
    p += sizeof(root);

    // NoMDEntries: 2 entries (bid, ask).
    GroupHeader gh{};
    gh.block_length = RefreshBookEntry::BLOCK_LENGTH;
    gh.num_in_group = 2;
    p = gh.encode_to(p);

    auto write_entry = [&](Price price, Quantity qty, Side side) {
        RefreshBookEntry entry{};
        std::memset(&entry, 0, sizeof(entry));
        entry.md_entry_px = engine_price_to_pricenull9(price);
        entry.md_entry_size = engine_qty_to_wire(qty);
        entry.security_id = ctx.security_id;
        entry.rpt_seq = ++ctx.rpt_seq;
        entry.number_of_orders = INT32_NULL;
        entry.md_price_level = 1;
        entry.md_update_action = static_cast<uint8_t>(MDUpdateAction::Change);
        entry.md_entry_type = encode_md_entry_type_book(side);
        entry.tradeable_size = engine_qty_to_wire(qty);
        std::memcpy(p, &entry, sizeof(entry));
        p += sizeof(entry);
    };

    write_entry(evt.best_bid, evt.bid_qty, Side::Buy);
    write_entry(evt.best_ask, evt.ask_qty, Side::Sell);

    // Empty NoOrderIDEntries group.
    GroupHeader8Byte gh8{};
    std::memset(&gh8, 0, sizeof(gh8));
    gh8.block_length = RefreshBookOrderEntry::BLOCK_LENGTH;
    gh8.num_in_group = 0;
    p = gh8.encode_to(p);

    return static_cast<size_t>(p - buf);
}

// Encode Trade -> MDIncrementalRefreshTradeSummary48 with 1 entry.
// Wire layout: MessageHeader(8) + root(11) + GroupHeader(3) + entry(32)
//            + GroupHeader8Byte(8) + 0 order entries = 62 bytes.
inline size_t encode_trade(
    char* buf,
    const Trade& evt,
    Mdp3EncodeContext& ctx)
{
    auto hdr = make_header<MDIncrementalRefreshTradeSummary48>();
    char* p = hdr.encode_to(buf);

    MDIncrementalRefreshTradeSummary48 root{};
    std::memset(&root, 0, sizeof(root));
    root.transact_time = static_cast<uint64_t>(evt.ts);
    root.match_event_indicator = 0x80;
    std::memcpy(p, &root, sizeof(root));
    p += sizeof(root);

    // NoMDEntries group header.
    GroupHeader gh{};
    gh.block_length = TradeSummaryEntry::BLOCK_LENGTH;
    gh.num_in_group = 1;
    p = gh.encode_to(p);

    // Single trade entry.
    TradeSummaryEntry entry{};
    std::memset(&entry, 0, sizeof(entry));
    entry.md_entry_px = engine_price_to_price9(evt.price);
    entry.md_entry_size = engine_qty_to_wire(evt.quantity);
    entry.security_id = ctx.security_id;
    entry.rpt_seq = ++ctx.rpt_seq;
    entry.number_of_orders = 2;  // 1 aggressor + 1 resting
    entry.aggressor_side = encode_aggressor_side(evt.aggressor_side);
    entry.md_update_action = static_cast<uint8_t>(MDUpdateAction::New);
    entry.md_trade_entry_id = ctx.rpt_seq;
    std::memcpy(p, &entry, sizeof(entry));
    p += sizeof(entry);

    // Empty NoOrderIDEntries group.
    GroupHeader8Byte gh8{};
    std::memset(&gh8, 0, sizeof(gh8));
    gh8.block_length = TradeSummaryOrderEntry::BLOCK_LENGTH;
    gh8.num_in_group = 0;
    p = gh8.encode_to(p);

    return static_cast<size_t>(p - buf);
}

// Encode MarketStatus -> SecurityStatus30.
// Wire layout: MessageHeader(8) + root(30) = 38 bytes. No groups.
inline size_t encode_market_status(
    char* buf,
    const MarketStatus& evt,
    const Mdp3EncodeContext& ctx)
{
    auto hdr = make_header<SecurityStatus30>();
    char* p = hdr.encode_to(buf);

    SecurityStatus30 msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.transact_time = static_cast<uint64_t>(evt.ts);
    std::memcpy(msg.security_group, ctx.security_group, sizeof(msg.security_group));
    std::memcpy(msg.asset, ctx.asset, sizeof(msg.asset));
    msg.security_id = ctx.security_id;
    msg.trade_date = UINT16_NULL;
    msg.match_event_indicator = 0x80;
    msg.security_trading_status = encode_security_trading_status(evt.state);
    msg.halt_reason = static_cast<uint8_t>(HaltReason::GroupSchedule);
    msg.security_trading_event = static_cast<uint8_t>(SecurityTradingEvent::NoEvent);

    std::memcpy(p, &msg, sizeof(msg));
    p += sizeof(msg);
    return static_cast<size_t>(p - buf);
}

// Maximum buffer size needed for any single MDP3 encoded message.
// TopOfBook produces the largest: 8 + 11 + 3 + 64 + 8 = 94 bytes.
constexpr size_t MAX_MDP3_ENCODED_SIZE = 128;

}  // namespace exchange::cme::sbe::mdp3
