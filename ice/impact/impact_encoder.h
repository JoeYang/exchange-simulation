#pragma once

#include "ice/impact/impact_messages.h"
#include "exchange-core/events.h"

#include <cstddef>
#include <cstring>

namespace exchange::ice::impact {

// ---------------------------------------------------------------------------
// Conversion: engine types -> iMpact wire types.
//
// Engine uses Price = int64_t (PRICE_SCALE=10000) and Quantity = int64_t
// (also PRICE_SCALE=10000). iMpact uses int64_t price directly and uint32_t
// quantity in whole lots.
// ---------------------------------------------------------------------------

inline int64_t engine_price_to_wire(Price p) {
    return p;  // pass through; both use int64_t fixed-point
}

inline uint32_t engine_qty_to_wire(Quantity q) {
    return static_cast<uint32_t>(q / PRICE_SCALE);
}

inline uint8_t encode_side(exchange::Side s) {
    return (s == exchange::Side::Buy)
        ? static_cast<uint8_t>(Side::Buy)
        : static_cast<uint8_t>(Side::Sell);
}

inline uint8_t encode_trading_status(SessionState state) {
    switch (state) {
        case SessionState::PreOpen:
        case SessionState::OpeningAuction:
            return static_cast<uint8_t>(TradingStatus::PreOpen);
        case SessionState::Continuous:
            return static_cast<uint8_t>(TradingStatus::Continuous);
        case SessionState::Halt:
        case SessionState::VolatilityAuction:
        case SessionState::LockLimit:
            return static_cast<uint8_t>(TradingStatus::Halt);
        case SessionState::Closed:
        case SessionState::PreClose:
        case SessionState::ClosingAuction:
            return static_cast<uint8_t>(TradingStatus::Closed);
    }
    return static_cast<uint8_t>(TradingStatus::Closed);
}

// ---------------------------------------------------------------------------
// Encode context — per-instrument state carried through encoding calls.
// ---------------------------------------------------------------------------

struct ImpactEncodeContext {
    int32_t  instrument_id{0};
    uint32_t seq_num{0};              // block sequence number (bundles)
    uint32_t sequence_within_msg{0};  // per-message sequence counter
    int64_t  next_deal_id{1};         // monotonic deal ID generator
};

// ---------------------------------------------------------------------------
// Maximum buffer size for any single encoded iMpact bundle.
//
// Largest single-event bundle:
//   BundleStart(3+14) + DealTrade(3+33) + BundleEnd(3+4) = 60
// Provide headroom for future multi-message bundles.
// ---------------------------------------------------------------------------

constexpr size_t MAX_IMPACT_ENCODED_SIZE = 128;

// ---------------------------------------------------------------------------
// Bundle helpers — write BundleStart / BundleEnd framing.
// ---------------------------------------------------------------------------

inline char* write_bundle_start(
    char* buf, size_t buf_len, uint32_t seq_num,
    uint16_t msg_count, int64_t timestamp)
{
    BundleStart bs{};
    bs.sequence_number = seq_num;
    bs.message_count   = msg_count;
    bs.timestamp       = timestamp;
    return encode(buf, buf_len, bs);
}

inline char* write_bundle_end(char* buf, size_t buf_len, uint32_t seq_num) {
    BundleEnd be{};
    be.sequence_number = seq_num;
    return encode(buf, buf_len, be);
}

// ---------------------------------------------------------------------------
// Encode DepthUpdate -> bundled PriceLevel.
//
// DepthUpdate is aggregated MBP data — maps to PriceLevel, not per-order
// AddModifyOrder. For Remove action: PriceLevel with qty=0, order_count=0.
// Returns total bytes written, or 0 on failure.
// ---------------------------------------------------------------------------

inline size_t encode_depth_update(
    char* buf, size_t buf_len,
    const DepthUpdate& evt,
    ImpactEncodeContext& ctx)
{
    uint32_t seq = ++ctx.seq_num;
    char* p = write_bundle_start(buf, buf_len, seq, 1, evt.ts);
    if (!p) return 0;

    PriceLevel msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.instrument_id = ctx.instrument_id;
    msg.side          = encode_side(evt.side);
    msg.price         = engine_price_to_wire(evt.price);
    msg.quantity      = engine_qty_to_wire(evt.total_qty);
    msg.order_count   = static_cast<uint16_t>(evt.order_count);

    p = encode(p, buf_len - static_cast<size_t>(p - buf), msg);
    if (!p) return 0;

    p = write_bundle_end(p, buf_len - static_cast<size_t>(p - buf), seq);
    if (!p) return 0;
    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode OrderCancelled -> bundled OrderWithdrawal.
//
// Produces a withdrawal message for a specific cancelled order.
// Caller provides the order details (side, price, qty) since OrderCancelled
// only carries the order ID; these are looked up from the order book.
// ---------------------------------------------------------------------------

inline size_t encode_order_cancelled(
    char* buf, size_t buf_len,
    const OrderCancelled& evt,
    exchange::Side side, Price price, Quantity quantity,
    ImpactEncodeContext& ctx)
{
    uint32_t seq = ++ctx.seq_num;
    char* p = write_bundle_start(buf, buf_len, seq, 1, evt.ts);
    if (!p) return 0;

    OrderWithdrawal msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.instrument_id      = ctx.instrument_id;
    msg.order_id           = static_cast<int64_t>(evt.id);
    msg.sequence_within_msg = ++ctx.sequence_within_msg;
    msg.side               = encode_side(side);
    msg.price              = engine_price_to_wire(price);
    msg.quantity           = engine_qty_to_wire(quantity);

    p = encode(p, buf_len - static_cast<size_t>(p - buf), msg);
    if (!p) return 0;

    p = write_bundle_end(p, buf_len - static_cast<size_t>(p - buf), seq);
    if (!p) return 0;
    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode Trade -> bundled DealTrade.
// ---------------------------------------------------------------------------

inline size_t encode_trade(
    char* buf, size_t buf_len,
    const Trade& evt,
    ImpactEncodeContext& ctx)
{
    uint32_t seq = ++ctx.seq_num;
    char* p = write_bundle_start(buf, buf_len, seq, 1, evt.ts);
    if (!p) return 0;

    DealTrade msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.instrument_id  = ctx.instrument_id;
    msg.deal_id        = ctx.next_deal_id++;
    msg.price          = engine_price_to_wire(evt.price);
    msg.quantity       = engine_qty_to_wire(evt.quantity);
    msg.aggressor_side = encode_side(evt.aggressor_side);
    msg.timestamp      = evt.ts;

    p = encode(p, buf_len - static_cast<size_t>(p - buf), msg);
    if (!p) return 0;

    p = write_bundle_end(p, buf_len - static_cast<size_t>(p - buf), seq);
    if (!p) return 0;
    return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Encode exchange::MarketStatus -> bundled impact::MarketStatus.
//
// Uses fully-qualified type to avoid ambiguity with impact::MarketStatus.
// ---------------------------------------------------------------------------

inline size_t encode_market_status(
    char* buf, size_t buf_len,
    const exchange::MarketStatus& evt,
    ImpactEncodeContext& ctx)
{
    uint32_t seq = ++ctx.seq_num;
    char* p = write_bundle_start(buf, buf_len, seq, 1, evt.ts);
    if (!p) return 0;

    impact::MarketStatus msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.instrument_id  = ctx.instrument_id;
    msg.trading_status = encode_trading_status(evt.state);

    p = encode(p, buf_len - static_cast<size_t>(p - buf), msg);
    if (!p) return 0;

    p = write_bundle_end(p, buf_len - static_cast<size_t>(p - buf), seq);
    if (!p) return 0;
    return static_cast<size_t>(p - buf);
}

}  // namespace exchange::ice::impact
