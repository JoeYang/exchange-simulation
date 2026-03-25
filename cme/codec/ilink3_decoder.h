#pragma once

#include "cme/codec/ilink3_messages.h"
#include "cme/codec/sbe_header.h"
#include "exchange-core/matching_engine.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace exchange::cme::sbe::ilink3 {

// ---------------------------------------------------------------------------
// Decode error codes — no exceptions on the hot path.
// ---------------------------------------------------------------------------

enum class DecodeResult : uint8_t {
    kOk,
    kBufferTooShort,
    kUnknownTemplateId,
    kGroupOverflow,
    kBadBlockLength,
};

// ---------------------------------------------------------------------------
// Price/quantity conversion: SBE wire -> engine types (reverse of encoder).
//
// PRICE9 mantissa = engine_price * (1e9 / PRICE_SCALE).
// Factor = 1e9 / 10000 = 100000.
// ---------------------------------------------------------------------------

constexpr int64_t PRICE9_TO_ENGINE_DIVISOR = 100000;  // 1e9 / PRICE_SCALE

inline Price price9_to_engine(PRICE9 p) {
    if (p.is_null()) return 0;
    return static_cast<Price>(p.mantissa / PRICE9_TO_ENGINE_DIVISOR);
}

inline Quantity wire_qty_to_engine(uint32_t q) {
    if (q == UINT32_NULL) return 0;
    return static_cast<Quantity>(q) * PRICE_SCALE;
}

// ---------------------------------------------------------------------------
// Enum conversion: iLink3 wire values -> engine enums (reverse of encoder).
// ---------------------------------------------------------------------------

inline Side decode_side(uint8_t wire) {
    return (wire == 2) ? Side::Sell : Side::Buy;
}

inline OrderType decode_ord_type(uint8_t wire) {
    switch (static_cast<OrdType>(wire)) {
        case OrdType::Limit:                return OrderType::Limit;
        case OrdType::MarketWithProtection: return OrderType::Market;
        case OrdType::StopWithProtection:   return OrderType::Stop;
        case OrdType::StopLimit:            return OrderType::StopLimit;
    }
    return OrderType::Limit;
}

inline exchange::TimeInForce decode_tif(uint8_t wire) {
    switch (static_cast<TimeInForce>(wire)) {
        case TimeInForce::Day: return exchange::TimeInForce::DAY;
        case TimeInForce::GTC: return exchange::TimeInForce::GTC;
        case TimeInForce::FAK: return exchange::TimeInForce::IOC;
        case TimeInForce::FOK: return exchange::TimeInForce::FOK;
        case TimeInForce::GTD: return exchange::TimeInForce::GTD;
    }
    return exchange::TimeInForce::DAY;
}

// Extract client_order_id from the cl_ord_id char[20] field.
// The encoder writes it as a numeric string via snprintf.
inline uint64_t decode_cl_ord_id(const char (&field)[20]) {
    uint64_t val = 0;
    for (int i = 0; i < 20 && field[i] >= '0' && field[i] <= '9'; ++i) {
        val = val * 10 + static_cast<uint64_t>(field[i] - '0');
    }
    return val;
}

// ---------------------------------------------------------------------------
// Decoded result structs — hold the decoded root block for each message type.
// For ExecutionReportTradeOutright525 we also decode the NoFills group.
// ---------------------------------------------------------------------------

constexpr size_t MAX_FILL_ENTRIES = 16;

struct DecodedNewOrder514 {
    NewOrderSingle514 root;
};

struct DecodedCancelRequest516 {
    OrderCancelRequest516 root;
};

struct DecodedReplaceRequest515 {
    OrderCancelReplaceRequest515 root;
};

struct DecodedMassAction529 {
    OrderMassActionRequest529 root;
};

struct DecodedExecNew522 {
    ExecutionReportNew522 root;
};

struct DecodedExecReject523 {
    ExecutionReportReject523 root;
};

struct DecodedExecTrade525 {
    ExecutionReportTradeOutright525 root;
    FillEntry fill_entries[MAX_FILL_ENTRIES];
    uint8_t   num_fills;
};

struct DecodedExecCancel534 {
    ExecutionReportCancel534 root;
};

struct DecodedCancelReject535 {
    OrderCancelReject535 root;
};

// ---------------------------------------------------------------------------
// Per-message decode functions.
//
// Each takes a pointer past the MessageHeader (start of root block),
// the remaining buffer length, and the decoded header for block_length.
// ---------------------------------------------------------------------------

inline DecodeResult decode_new_order_514(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedNewOrder514& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_cancel_request_516(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedCancelRequest516& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_replace_request_515(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedReplaceRequest515& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_mass_action_529(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedMassAction529& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_exec_new_522(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedExecNew522& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_exec_reject_523(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedExecReject523& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_exec_trade_525(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedExecTrade525& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;

    std::memcpy(&out.root, buf, sizeof(out.root));
    const char* p = buf + hdr.block_length;
    size_t remaining = len - hdr.block_length;

    // NoFills group (GroupHeader = 3 bytes: blockLength uint16 + numInGroup uint8).
    if (remaining < sizeof(GroupHeader)) return DecodeResult::kBufferTooShort;
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    remaining -= sizeof(GroupHeader);

    if (gh.num_in_group > MAX_FILL_ENTRIES) return DecodeResult::kGroupOverflow;
    out.num_fills = gh.num_in_group;

    if (gh.num_in_group > 0 && gh.block_length < sizeof(FillEntry))
        return DecodeResult::kBadBlockLength;

    for (uint8_t i = 0; i < gh.num_in_group; ++i) {
        if (remaining < gh.block_length) return DecodeResult::kBufferTooShort;
        std::memcpy(&out.fill_entries[i], p, sizeof(FillEntry));
        p += gh.block_length;
        remaining -= gh.block_length;
    }

    // Skip NoOrderEvents group — not needed for E2E verification.

    return DecodeResult::kOk;
}

inline DecodeResult decode_exec_cancel_534(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedExecCancel534& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_cancel_reject_535(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedCancelReject535& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

// ---------------------------------------------------------------------------
// Top-level decode dispatch: decode MessageHeader, then dispatch by template_id.
//
// Caller provides a visitor with overloads for each decoded type:
//   void operator()(const DecodedNewOrder514&);
//   void operator()(const DecodedCancelRequest516&);
//   void operator()(const DecodedReplaceRequest515&);
//   void operator()(const DecodedMassAction529&);
//   void operator()(const DecodedExecNew522&);
//   void operator()(const DecodedExecReject523&);
//   void operator()(const DecodedExecTrade525&);
//   void operator()(const DecodedExecCancel534&);
//   void operator()(const DecodedCancelReject535&);
//
// Returns DecodeResult. On kUnknownTemplateId the visitor is not called.
// ---------------------------------------------------------------------------

template <typename Visitor>
DecodeResult decode_ilink3_message(
    const char* buf, size_t len,
    Visitor&& visitor)
{
    if (len < sizeof(MessageHeader)) return DecodeResult::kBufferTooShort;

    MessageHeader hdr{};
    const char* body = MessageHeader::decode_from(buf, hdr);
    size_t body_len = len - sizeof(MessageHeader);

    switch (hdr.template_id) {
        case NEW_ORDER_SINGLE_ID: {
            DecodedNewOrder514 out{};
            auto rc = decode_new_order_514(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case ORDER_CANCEL_REPLACE_REQUEST_ID: {
            DecodedReplaceRequest515 out{};
            auto rc = decode_replace_request_515(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case ORDER_CANCEL_REQUEST_ID: {
            DecodedCancelRequest516 out{};
            auto rc = decode_cancel_request_516(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case ORDER_MASS_ACTION_REQUEST_ID: {
            DecodedMassAction529 out{};
            auto rc = decode_mass_action_529(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case EXEC_REPORT_NEW_ID: {
            DecodedExecNew522 out{};
            auto rc = decode_exec_new_522(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case EXEC_REPORT_REJECT_ID: {
            DecodedExecReject523 out{};
            auto rc = decode_exec_reject_523(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case EXEC_REPORT_TRADE_OUTRIGHT_ID: {
            DecodedExecTrade525 out{};
            auto rc = decode_exec_trade_525(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case EXEC_REPORT_CANCEL_ID: {
            DecodedExecCancel534 out{};
            auto rc = decode_exec_cancel_534(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case ORDER_CANCEL_REJECT_ID: {
            DecodedCancelReject535 out{};
            auto rc = decode_cancel_reject_535(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        default:
            return DecodeResult::kUnknownTemplateId;
    }
}

}  // namespace exchange::cme::sbe::ilink3
