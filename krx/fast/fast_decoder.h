#pragma once

#include "krx/fast/fast_types.h"

#include <cstddef>
#include <cstdint>

namespace exchange::krx::fast {

// ---------------------------------------------------------------------------
// FAST 1.1 Decoder — KRX Market Data
//
// Decodes FAST binary messages back to application-level structs.
// Uses a visitor pattern: the caller provides a visitor with typed
// callbacks, and the decoder dispatches to the appropriate one based
// on the template ID.
//
// Visitor concept — implement the callbacks you need:
//   void on_quote(const FastQuote&);
//   void on_trade(const FastTrade&);
//   void on_status(const FastStatus&);
//   void on_snapshot(const FastSnapshot&);
//
// A no-op base class is provided for convenience.
// ---------------------------------------------------------------------------

class FastDecoderVisitorBase {
public:
    void on_quote(const FastQuote&) {}
    void on_trade(const FastTrade&) {}
    void on_status(const FastStatus&) {}
    void on_snapshot(const FastSnapshot&) {}
    void on_instrument_def(const FastInstrumentDef&) {}
    void on_full_snapshot(const FastFullSnapshot&) {}
};

// ---------------------------------------------------------------------------
// Decode a single Quote message from the buffer (after PMAP + template ID).
// Returns pointer past consumed bytes, or nullptr on decode failure.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_quote_fields(
    const uint8_t* buf, size_t len, FastQuote& msg)
{
    const uint8_t* p = buf;
    size_t remaining = len;

    p = decode_i64(p, remaining, msg.bid_price);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.bid_qty);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.ask_price);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.ask_qty);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.timestamp);
    if (!p) return nullptr;

    return p;
}

// ---------------------------------------------------------------------------
// Decode a single Trade message.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_trade_fields(
    const uint8_t* buf, size_t len, FastTrade& msg)
{
    const uint8_t* p = buf;
    size_t remaining = len;

    p = decode_i64(p, remaining, msg.price);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.quantity);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t side = 0;
    p = decode_u64(p, remaining, side);
    if (!p) return nullptr;
    msg.aggressor_side = static_cast<uint8_t>(side);
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.timestamp);
    if (!p) return nullptr;

    return p;
}

// ---------------------------------------------------------------------------
// Decode a single Status message.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_status_fields(
    const uint8_t* buf, size_t len, FastStatus& msg)
{
    const uint8_t* p = buf;
    size_t remaining = len;

    uint64_t state = 0;
    p = decode_u64(p, remaining, state);
    if (!p) return nullptr;
    msg.session_state = static_cast<uint8_t>(state);
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.timestamp);
    if (!p) return nullptr;

    return p;
}

// ---------------------------------------------------------------------------
// Decode a single Snapshot message.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_snapshot_fields(
    const uint8_t* buf, size_t len, FastSnapshot& msg)
{
    const uint8_t* p = buf;
    size_t remaining = len;

    p = decode_i64(p, remaining, msg.bid_price);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.bid_qty);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.ask_price);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.ask_qty);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t bc = 0;
    p = decode_u64(p, remaining, bc);
    if (!p) return nullptr;
    msg.bid_count = static_cast<uint32_t>(bc);
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t ac = 0;
    p = decode_u64(p, remaining, ac);
    if (!p) return nullptr;
    msg.ask_count = static_cast<uint32_t>(ac);
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.timestamp);
    if (!p) return nullptr;

    return p;
}

// ---------------------------------------------------------------------------
// Decode a single InstrumentDef message.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_instrument_def_fields(
    const uint8_t* buf, size_t len, FastInstrumentDef& msg)
{
    const uint8_t* p = buf;
    size_t remaining = len;

    uint64_t iid = 0;
    p = decode_u64(p, remaining, iid);
    if (!p) return nullptr;
    msg.instrument_id = static_cast<uint32_t>(iid);
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_bytes(p, remaining, msg.symbol, sizeof(msg.symbol));
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_bytes(p, remaining, msg.description, sizeof(msg.description));
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t pg = 0;
    p = decode_u64(p, remaining, pg);
    if (!p) return nullptr;
    msg.product_group = static_cast<uint8_t>(pg);
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.tick_size);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.lot_size);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.max_order_size);
    if (!p) return nullptr;
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t total = 0;
    p = decode_u64(p, remaining, total);
    if (!p) return nullptr;
    msg.total_instruments = static_cast<uint32_t>(total);
    remaining = len - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, msg.timestamp);
    if (!p) return nullptr;

    return p;
}

// ---------------------------------------------------------------------------
// Decode a single FullSnapshot message.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_full_snapshot_fields(
    const uint8_t* buf, size_t len, FastFullSnapshot& msg)
{
    const uint8_t* p = buf;
    size_t remaining = len;

    uint64_t iid = 0;
    p = decode_u64(p, remaining, iid);
    if (!p) return nullptr;
    msg.instrument_id = static_cast<uint32_t>(iid);
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t seq = 0;
    p = decode_u64(p, remaining, seq);
    if (!p) return nullptr;
    msg.seq_num = static_cast<uint32_t>(seq);
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t nb = 0;
    p = decode_u64(p, remaining, nb);
    if (!p) return nullptr;
    msg.num_bid_levels = static_cast<uint8_t>(nb);
    remaining = len - static_cast<size_t>(p - buf);

    uint64_t na = 0;
    p = decode_u64(p, remaining, na);
    if (!p) return nullptr;
    msg.num_ask_levels = static_cast<uint8_t>(na);
    remaining = len - static_cast<size_t>(p - buf);

    // Decode bid levels.
    for (uint8_t i = 0; i < msg.num_bid_levels && i < kSnapshotBookDepth; ++i) {
        p = decode_i64(p, remaining, msg.bids[i].price);
        if (!p) return nullptr;
        remaining = len - static_cast<size_t>(p - buf);

        p = decode_i64(p, remaining, msg.bids[i].quantity);
        if (!p) return nullptr;
        remaining = len - static_cast<size_t>(p - buf);

        uint64_t oc = 0;
        p = decode_u64(p, remaining, oc);
        if (!p) return nullptr;
        msg.bids[i].order_count = static_cast<uint32_t>(oc);
        remaining = len - static_cast<size_t>(p - buf);
    }

    // Decode ask levels.
    for (uint8_t i = 0; i < msg.num_ask_levels && i < kSnapshotBookDepth; ++i) {
        p = decode_i64(p, remaining, msg.asks[i].price);
        if (!p) return nullptr;
        remaining = len - static_cast<size_t>(p - buf);

        p = decode_i64(p, remaining, msg.asks[i].quantity);
        if (!p) return nullptr;
        remaining = len - static_cast<size_t>(p - buf);

        uint64_t oc = 0;
        p = decode_u64(p, remaining, oc);
        if (!p) return nullptr;
        msg.asks[i].order_count = static_cast<uint32_t>(oc);
        remaining = len - static_cast<size_t>(p - buf);
    }

    p = decode_i64(p, remaining, msg.timestamp);
    if (!p) return nullptr;

    return p;
}

// ---------------------------------------------------------------------------
// decode_message() — decode a single FAST message and dispatch to visitor.
//
// Reads PMAP + template ID, then dispatches to the appropriate field
// decoder and visitor callback. Returns bytes consumed, or 0 on failure.
// ---------------------------------------------------------------------------

template <typename VisitorT>
size_t decode_message(const uint8_t* buf, size_t len, VisitorT& visitor) {
    if (buf == nullptr || len == 0) return 0;

    const uint8_t* p = buf;
    size_t remaining = len;

    // Read PMAP
    PresenceMap pmap{};
    p = decode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = len - static_cast<size_t>(p - buf);

    // Read template ID (always present — PMAP bit 0)
    uint64_t raw_tid = 0;
    p = decode_u64(p, remaining, raw_tid);
    if (!p) return 0;
    remaining = len - static_cast<size_t>(p - buf);

    auto tid = static_cast<TemplateId>(raw_tid);

    switch (tid) {
        case TemplateId::Quote: {
            FastQuote msg{};
            p = decode_quote_fields(p, remaining, msg);
            if (!p) return 0;
            visitor.on_quote(msg);
            break;
        }
        case TemplateId::Trade: {
            FastTrade msg{};
            p = decode_trade_fields(p, remaining, msg);
            if (!p) return 0;
            visitor.on_trade(msg);
            break;
        }
        case TemplateId::Status: {
            FastStatus msg{};
            p = decode_status_fields(p, remaining, msg);
            if (!p) return 0;
            visitor.on_status(msg);
            break;
        }
        case TemplateId::Snapshot: {
            FastSnapshot msg{};
            p = decode_snapshot_fields(p, remaining, msg);
            if (!p) return 0;
            visitor.on_snapshot(msg);
            break;
        }
        case TemplateId::InstrumentDef: {
            FastInstrumentDef msg{};
            p = decode_instrument_def_fields(p, remaining, msg);
            if (!p) return 0;
            visitor.on_instrument_def(msg);
            break;
        }
        case TemplateId::FullSnapshot: {
            FastFullSnapshot msg{};
            p = decode_full_snapshot_fields(p, remaining, msg);
            if (!p) return 0;
            visitor.on_full_snapshot(msg);
            break;
        }
        default:
            // Unknown template — cannot skip (no length prefix in FAST).
            return 0;
    }

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// decode_messages() — decode multiple concatenated FAST messages.
//
// Keeps consuming messages until the buffer is exhausted or a decode
// error occurs. Returns total bytes consumed.
// ---------------------------------------------------------------------------

template <typename VisitorT>
size_t decode_messages(const uint8_t* buf, size_t len, VisitorT& visitor) {
    size_t offset = 0;
    while (offset < len) {
        size_t consumed = decode_message(buf + offset, len - offset, visitor);
        if (consumed == 0) break;
        offset += consumed;
    }
    return offset;
}

}  // namespace exchange::krx::fast
