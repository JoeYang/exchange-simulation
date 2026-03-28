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
