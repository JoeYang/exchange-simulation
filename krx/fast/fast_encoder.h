#pragma once

#include "krx/fast/fast_types.h"
#include "exchange-core/events.h"

#include <cstddef>
#include <cstdint>

namespace exchange::krx::fast {

// ---------------------------------------------------------------------------
// FAST 1.1 Encoder — KRX Market Data
//
// Encodes engine market data events into FAST binary messages using
// stop-bit encoding. Four hardcoded templates:
//   1 = Quote   (TopOfBook → bid/ask price+qty)
//   2 = Trade   (Trade → price, qty, aggressor side)
//   3 = Status  (MarketStatus → session state)
//   4 = Snapshot (TopOfBook → full book view)
//
// Wire format per message:
//   [PMAP] [TemplateID] [field1] [field2] ...
//
// All fields are mandatory (no optional/delta encoding) for simplicity.
// PMAP bit 0 = template ID present (always 1 for our use case).
// ---------------------------------------------------------------------------

struct FastEncodeContext {
    uint32_t seq_num{0};
};

// ---------------------------------------------------------------------------
// Encode a FastQuote message.
//
// Wire layout: PMAP(1B) + TemplateID(1B) + bid_price + bid_qty +
//              ask_price + ask_qty + timestamp
// Returns bytes written, or 0 on failure.
// ---------------------------------------------------------------------------

inline size_t encode_quote(
    uint8_t* buf, size_t buf_len, const FastQuote& msg)
{
    uint8_t* p = buf;
    size_t remaining = buf_len;

    // PMAP: all fields present. Bit 0 = template ID.
    PresenceMap pmap{};
    pmap.set_bit(0);  // template ID present
    p = encode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    // Template ID
    p = encode_u64(p, remaining, static_cast<uint64_t>(TemplateId::Quote));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    // Fields: bid_price, bid_qty, ask_price, ask_qty, timestamp
    p = encode_i64(p, remaining, msg.bid_price);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.bid_qty);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.ask_price);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.ask_qty);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.timestamp);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode a FastTrade message.
//
// Wire layout: PMAP(1B) + TemplateID(1B) + price + quantity +
//              aggressor_side + timestamp
// ---------------------------------------------------------------------------

inline size_t encode_trade(
    uint8_t* buf, size_t buf_len, const FastTrade& msg)
{
    uint8_t* p = buf;
    size_t remaining = buf_len;

    PresenceMap pmap{};
    pmap.set_bit(0);
    p = encode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, static_cast<uint64_t>(TemplateId::Trade));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.price);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.quantity);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.aggressor_side);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.timestamp);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode a FastStatus message.
//
// Wire layout: PMAP(1B) + TemplateID(1B) + session_state + timestamp
// ---------------------------------------------------------------------------

inline size_t encode_status(
    uint8_t* buf, size_t buf_len, const FastStatus& msg)
{
    uint8_t* p = buf;
    size_t remaining = buf_len;

    PresenceMap pmap{};
    pmap.set_bit(0);
    p = encode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, static_cast<uint64_t>(TemplateId::Status));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.session_state);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.timestamp);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode a FastSnapshot message.
//
// Wire layout: PMAP(1B) + TemplateID(1B) + bid_price + bid_qty +
//              ask_price + ask_qty + bid_count + ask_count + timestamp
// ---------------------------------------------------------------------------

inline size_t encode_snapshot(
    uint8_t* buf, size_t buf_len, const FastSnapshot& msg)
{
    uint8_t* p = buf;
    size_t remaining = buf_len;

    PresenceMap pmap{};
    pmap.set_bit(0);
    p = encode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, static_cast<uint64_t>(TemplateId::Snapshot));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.bid_price);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.bid_qty);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.ask_price);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.ask_qty);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.bid_count);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.ask_count);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.timestamp);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode a FastInstrumentDef message.
//
// Wire layout: PMAP(1B) + TemplateID + instrument_id + symbol(8B raw) +
//              description(32B raw) + product_group + tick_size + lot_size +
//              max_order_size + total_instruments + timestamp
// ---------------------------------------------------------------------------

inline size_t encode_instrument_def(
    uint8_t* buf, size_t buf_len, const FastInstrumentDef& msg)
{
    uint8_t* p = buf;
    size_t remaining = buf_len;

    PresenceMap pmap{};
    pmap.set_bit(0);
    p = encode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, static_cast<uint64_t>(TemplateId::InstrumentDef));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.instrument_id);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    // Fixed-length byte fields (no stop-bit encoding).
    p = encode_bytes(p, remaining, msg.symbol, sizeof(msg.symbol));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_bytes(p, remaining, msg.description, sizeof(msg.description));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.product_group);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.tick_size);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.lot_size);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.max_order_size);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.total_instruments);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_i64(p, remaining, msg.timestamp);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode a FastFullSnapshot message (template 6).
//
// Wire layout: PMAP(1B) + TemplateID + instrument_id + seq_num +
//              num_bid_levels + num_ask_levels +
//              [bid levels: price + qty + order_count] * num_bid +
//              [ask levels: price + qty + order_count] * num_ask +
//              timestamp
// ---------------------------------------------------------------------------

inline size_t encode_full_snapshot(
    uint8_t* buf, size_t buf_len, const FastFullSnapshot& msg)
{
    uint8_t* p = buf;
    size_t remaining = buf_len;

    PresenceMap pmap{};
    pmap.set_bit(0);
    p = encode_pmap(p, remaining, pmap);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, static_cast<uint64_t>(TemplateId::FullSnapshot));
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.instrument_id);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.seq_num);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.num_bid_levels);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    p = encode_u64(p, remaining, msg.num_ask_levels);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    // Encode bid levels.
    for (uint8_t i = 0; i < msg.num_bid_levels; ++i) {
        p = encode_i64(p, remaining, msg.bids[i].price);
        if (!p) return 0;
        remaining = buf_len - static_cast<size_t>(p - buf);

        p = encode_i64(p, remaining, msg.bids[i].quantity);
        if (!p) return 0;
        remaining = buf_len - static_cast<size_t>(p - buf);

        p = encode_u64(p, remaining, msg.bids[i].order_count);
        if (!p) return 0;
        remaining = buf_len - static_cast<size_t>(p - buf);
    }

    // Encode ask levels.
    for (uint8_t i = 0; i < msg.num_ask_levels; ++i) {
        p = encode_i64(p, remaining, msg.asks[i].price);
        if (!p) return 0;
        remaining = buf_len - static_cast<size_t>(p - buf);

        p = encode_i64(p, remaining, msg.asks[i].quantity);
        if (!p) return 0;
        remaining = buf_len - static_cast<size_t>(p - buf);

        p = encode_u64(p, remaining, msg.asks[i].order_count);
        if (!p) return 0;
        remaining = buf_len - static_cast<size_t>(p - buf);
    }

    p = encode_i64(p, remaining, msg.timestamp);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Convenience: encode from engine event types.
// ---------------------------------------------------------------------------

inline FastQuote to_fast_quote(const TopOfBook& e) {
    return FastQuote{
        .bid_price = e.best_bid,
        .bid_qty   = e.bid_qty,
        .ask_price = e.best_ask,
        .ask_qty   = e.ask_qty,
        .timestamp = e.ts,
    };
}

inline FastTrade to_fast_trade(const Trade& e) {
    return FastTrade{
        .price          = e.price,
        .quantity        = e.quantity,
        .aggressor_side = static_cast<uint8_t>(e.aggressor_side),
        .timestamp      = e.ts,
    };
}

inline FastStatus to_fast_status(const exchange::MarketStatus& e) {
    return FastStatus{
        .session_state = static_cast<uint8_t>(e.state),
        .timestamp     = e.ts,
    };
}

inline FastSnapshot to_fast_snapshot(const TopOfBook& e) {
    return FastSnapshot{
        .bid_price = e.best_bid,
        .bid_qty   = e.bid_qty,
        .ask_price = e.best_ask,
        .ask_qty   = e.ask_qty,
        .bid_count = (e.best_bid != 0) ? 1u : 0u,
        .ask_count = (e.best_ask != 0) ? 1u : 0u,
        .timestamp = e.ts,
    };
}

}  // namespace exchange::krx::fast
