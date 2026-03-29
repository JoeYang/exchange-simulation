#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace exchange::krx::fast {

// ---------------------------------------------------------------------------
// FAST 1.1 Primitive Encoding/Decoding
//
// FAST (FIX Adapted for STreaming) uses stop-bit encoding:
//   - Each byte carries 7 data bits (bits 0-6) and 1 stop bit (bit 7)
//   - Stop bit = 1 means this is the last byte of the field
//   - Stop bit = 0 means more bytes follow
//
// Nullable fields: a single byte 0x80 (stop bit set, value=0) encodes null
// for unsigned integers. For signed, null is also 0x80.
//
// Presence Map (PMAP): a bitfield encoded with stop-bit encoding that
// indicates which optional fields are present in the message.
// ---------------------------------------------------------------------------

// Maximum bytes for a stop-bit-encoded 64-bit integer.
// 64 data bits / 7 bits per byte = 10 bytes max.
constexpr size_t kMaxStopBitBytes = 10;

// Maximum bytes for a PMAP (we need at most 8 fields per template,
// so 1 byte of PMAP is sufficient; allow 2 for safety).
constexpr size_t kMaxPmapBytes = 2;

// Null indicator: a single byte with stop bit set and zero data bits.
constexpr uint8_t kNullByte = 0x80;

// ---------------------------------------------------------------------------
// Stop-bit encode unsigned integer.
//
// Writes value to buf using FAST stop-bit encoding.
// Returns pointer past the last written byte, or nullptr if buf_len
// is insufficient.
//
// Encoding: split value into 7-bit groups, big-endian order.
// The last byte has bit 7 set (stop bit).
// ---------------------------------------------------------------------------

inline uint8_t* encode_u64(uint8_t* buf, size_t buf_len, uint64_t value) {
    // Determine number of 7-bit groups needed.
    // At least 1 byte even for value=0.
    uint8_t tmp[kMaxStopBitBytes];
    int n = 0;

    // Encode in reverse (LSB first), then we'll reverse.
    tmp[0] = static_cast<uint8_t>((value & 0x7F) | 0x80);  // last byte, stop bit set
    value >>= 7;
    n = 1;

    while (value > 0) {
        tmp[n] = static_cast<uint8_t>(value & 0x7F);  // no stop bit
        value >>= 7;
        ++n;
    }

    if (static_cast<size_t>(n) > buf_len) return nullptr;

    // Write in big-endian order (reverse of tmp).
    for (int i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    return buf + n;
}

// ---------------------------------------------------------------------------
// Stop-bit encode signed integer (int64_t).
//
// FAST signed encoding: two's complement, 7-bit groups, big-endian.
// The sign is indicated by bit 6 of the most significant encoded byte:
//   - bit 6 = 0 -> positive (or zero)
//   - bit 6 = 1 -> negative
// If the natural encoding would be ambiguous, a leading 0x00 (positive)
// or 0x7F (negative) byte is prepended.
// ---------------------------------------------------------------------------

inline uint8_t* encode_i64(uint8_t* buf, size_t buf_len, int64_t value) {
    // Build 7-bit groups in reverse (LSB first into tmp[0]).
    uint8_t tmp[kMaxStopBitBytes];
    int n = 0;

    // Use arithmetic right shift on the signed value itself.
    // This correctly propagates the sign bit.
    int64_t v = value;

    tmp[0] = static_cast<uint8_t>((static_cast<uint64_t>(v) & 0x7F) | 0x80);
    v >>= 7;  // arithmetic shift — preserves sign
    n = 1;

    // For positive: stop when v == 0 and sign bit (bit 6) of leading byte is 0.
    // For negative: stop when v == -1 and sign bit (bit 6) of leading byte is 1.
    while (!((v == 0 && !(tmp[n - 1] & 0x40)) ||
             (v == -1 && (tmp[n - 1] & 0x40)))) {
        if (n >= static_cast<int>(kMaxStopBitBytes)) break;
        tmp[n] = static_cast<uint8_t>(static_cast<uint64_t>(v) & 0x7F);
        v >>= 7;
        ++n;
    }

    if (static_cast<size_t>(n) > buf_len) return nullptr;

    // Write big-endian (reverse of tmp).
    for (int i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    return buf + n;
}

// ---------------------------------------------------------------------------
// Stop-bit encode nullable unsigned integer.
// Nullable unsigned uses value+1 on wire (0 is reserved for null).
// To encode null, write kNullByte.
// ---------------------------------------------------------------------------

inline uint8_t* encode_nullable_u64(
    uint8_t* buf, size_t buf_len, std::optional<uint64_t> value)
{
    if (!value.has_value()) {
        if (buf_len < 1) return nullptr;
        buf[0] = kNullByte;
        return buf + 1;
    }
    // Nullable encoding: wire value = actual value + 1 (0 reserved for null)
    return encode_u64(buf, buf_len, value.value() + 1);
}

// ---------------------------------------------------------------------------
// Stop-bit encode nullable signed integer.
// Nullable signed: null is 0x80. Non-null positive values are incremented
// by 1 on wire (to make room for null=0).
// ---------------------------------------------------------------------------

inline uint8_t* encode_nullable_i64(
    uint8_t* buf, size_t buf_len, std::optional<int64_t> value)
{
    if (!value.has_value()) {
        if (buf_len < 1) return nullptr;
        buf[0] = kNullByte;
        return buf + 1;
    }
    // Nullable: non-negative values get +1, negative stay as-is.
    int64_t wire_val = value.value();
    if (wire_val >= 0) wire_val += 1;
    return encode_i64(buf, buf_len, wire_val);
}

// ---------------------------------------------------------------------------
// Stop-bit decode unsigned integer.
//
// Reads from buf, returns pointer past the last consumed byte.
// Returns nullptr if the buffer is truncated (no stop bit found).
// ---------------------------------------------------------------------------

inline const uint8_t* decode_u64(
    const uint8_t* buf, size_t buf_len, uint64_t& value)
{
    value = 0;
    for (size_t i = 0; i < buf_len && i < kMaxStopBitBytes; ++i) {
        uint8_t byte = buf[i];
        value = (value << 7) | (byte & 0x7F);
        if (byte & 0x80) {  // stop bit
            return buf + i + 1;
        }
    }
    return nullptr;  // truncated
}

// ---------------------------------------------------------------------------
// Stop-bit decode signed integer.
//
// Sign is determined by the MSB of the first byte's data portion (bit 6).
// If set, the value is negative (sign-extend).
// ---------------------------------------------------------------------------

inline const uint8_t* decode_i64(
    const uint8_t* buf, size_t buf_len, int64_t& value)
{
    if (buf_len == 0) return nullptr;

    // Check sign from first byte's bit 6.
    bool negative = (buf[0] & 0x40) != 0;

    uint64_t uval = negative ? ~uint64_t{0} : 0;

    for (size_t i = 0; i < buf_len && i < kMaxStopBitBytes; ++i) {
        uint8_t byte = buf[i];
        uval = (uval << 7) | (byte & 0x7F);
        if (byte & 0x80) {  // stop bit
            value = static_cast<int64_t>(uval);
            return buf + i + 1;
        }
    }
    return nullptr;  // truncated
}

// ---------------------------------------------------------------------------
// Decode nullable unsigned.
// Returns nullopt if wire value is null (single 0x80 byte with decoded=0).
// Otherwise returns wire_value - 1.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_nullable_u64(
    const uint8_t* buf, size_t buf_len, std::optional<uint64_t>& value)
{
    uint64_t wire_val = 0;
    const uint8_t* end = decode_u64(buf, buf_len, wire_val);
    if (!end) return nullptr;
    if (wire_val == 0) {
        value = std::nullopt;  // null
    } else {
        value = wire_val - 1;
    }
    return end;
}

// ---------------------------------------------------------------------------
// Decode nullable signed.
// A single 0x80 byte decodes as signed value 0 — but since we check for
// exactly the single null byte pattern, null = single byte 0x80.
// Non-null non-negative values have +1 subtracted.
// ---------------------------------------------------------------------------

inline const uint8_t* decode_nullable_i64(
    const uint8_t* buf, size_t buf_len, std::optional<int64_t>& value)
{
    // Check for null: single byte 0x80
    if (buf_len >= 1 && buf[0] == kNullByte) {
        // Disambiguate: 0x80 as a standalone is null.
        // As a signed decode, 0x80 = stop bit + 0 data = value 0.
        // In nullable context, wire value 0 for non-negative = null.
        value = std::nullopt;
        return buf + 1;
    }

    int64_t wire_val = 0;
    const uint8_t* end = decode_i64(buf, buf_len, wire_val);
    if (!end) return nullptr;
    if (wire_val >= 0) {
        value = wire_val - 1;  // undo the +1 shift
    } else {
        value = wire_val;
    }
    return end;
}

// ---------------------------------------------------------------------------
// Presence Map (PMAP)
//
// A compact bitfield indicating which optional fields are present.
// Encoded with stop-bit encoding (same as integers).
// Bit 0 of byte 0 = field 6 (rightmost), bit 6 of byte 0 = field 0 (leftmost).
// We limit to 14 fields (2 bytes max) which is plenty for our 4 templates.
// ---------------------------------------------------------------------------

struct PresenceMap {
    uint8_t bytes[kMaxPmapBytes]{};
    size_t byte_count{0};
    uint8_t bit_index{0};  // next bit to read (for decoding)

    // Set bit at position (0 = first field). Used during encoding.
    void set_bit(uint8_t pos) {
        uint8_t byte_idx = pos / 7;
        uint8_t bit_pos = 6 - (pos % 7);  // MSB first within each byte
        if (byte_idx < kMaxPmapBytes) {
            bytes[byte_idx] |= (1 << bit_pos);
        }
    }

    // Check if bit at current position is set. Advances the index.
    // Used during decoding.
    bool next_bit() {
        uint8_t byte_idx = bit_index / 7;
        uint8_t bit_pos = 6 - (bit_index % 7);
        ++bit_index;
        if (byte_idx >= byte_count) return false;
        return (bytes[byte_idx] & (1 << bit_pos)) != 0;
    }

    // Reset read position for decoding.
    void reset_read() { bit_index = 0; }
};

// Encode PMAP to buffer. Returns pointer past written bytes.
inline uint8_t* encode_pmap(uint8_t* buf, size_t buf_len, const PresenceMap& pmap) {
    // Determine how many bytes we need.
    // Find the last byte with any bits set (or use 1 byte minimum).
    size_t n = 1;
    for (size_t i = 0; i < kMaxPmapBytes; ++i) {
        if (pmap.bytes[i] != 0) n = i + 1;
    }
    if (n > buf_len) return nullptr;

    for (size_t i = 0; i < n; ++i) {
        buf[i] = pmap.bytes[i];
    }
    // Set stop bit on last byte.
    buf[n - 1] |= 0x80;
    // Clear stop bit on non-last bytes.
    for (size_t i = 0; i + 1 < n; ++i) {
        buf[i] &= 0x7F;
    }
    return buf + n;
}

// Decode PMAP from buffer. Returns pointer past consumed bytes.
inline const uint8_t* decode_pmap(
    const uint8_t* buf, size_t buf_len, PresenceMap& pmap)
{
    pmap = PresenceMap{};
    for (size_t i = 0; i < buf_len && i < kMaxPmapBytes; ++i) {
        pmap.bytes[i] = buf[i] & 0x7F;  // strip stop bit for storage
        pmap.byte_count = i + 1;
        if (buf[i] & 0x80) {  // stop bit
            return buf + i + 1;
        }
    }
    return nullptr;  // truncated
}

// ---------------------------------------------------------------------------
// Fixed-length byte string encode/decode for FAST.
//
// Encodes/decodes a fixed-length char array as raw bytes (no stop-bit).
// Used for symbol/description fields in InstrumentDef.
// ---------------------------------------------------------------------------

inline uint8_t* encode_bytes(uint8_t* buf, size_t buf_len,
                             const char* src, size_t field_len) {
    if (buf_len < field_len) return nullptr;
    std::memcpy(buf, src, field_len);
    return buf + field_len;
}

inline const uint8_t* decode_bytes(const uint8_t* buf, size_t buf_len,
                                   char* dst, size_t field_len) {
    if (buf_len < field_len) return nullptr;
    std::memcpy(dst, buf, field_len);
    return buf + field_len;
}

// ---------------------------------------------------------------------------
// FAST message template IDs for KRX market data.
// ---------------------------------------------------------------------------

enum class TemplateId : uint32_t {
    Quote         = 1,  // Top-of-book quote (bid/ask price + qty)
    Trade         = 2,  // Trade execution (price, qty, aggressor side)
    Status        = 3,  // Session/market status change
    Snapshot      = 4,  // Full order book snapshot (bid + ask levels)
    InstrumentDef = 5,  // Security definition (secdef channel)
    FullSnapshot  = 6,  // Full-depth book snapshot (snapshot channel)
};

// ---------------------------------------------------------------------------
// KRX FAST message structs (decoded form).
//
// These are the application-level messages that the encoder produces
// and the decoder consumes. Field layout matches what the engine provides.
// ---------------------------------------------------------------------------

struct FastQuote {
    static constexpr TemplateId TEMPLATE_ID = TemplateId::Quote;

    int64_t bid_price{0};   // engine fixed-point
    int64_t bid_qty{0};     // engine fixed-point
    int64_t ask_price{0};   // engine fixed-point
    int64_t ask_qty{0};     // engine fixed-point
    int64_t timestamp{0};   // epoch nanoseconds
};

struct FastTrade {
    static constexpr TemplateId TEMPLATE_ID = TemplateId::Trade;

    int64_t price{0};          // engine fixed-point
    int64_t quantity{0};       // engine fixed-point
    uint8_t aggressor_side{0}; // 0=Buy, 1=Sell
    int64_t timestamp{0};      // epoch nanoseconds
};

struct FastStatus {
    static constexpr TemplateId TEMPLATE_ID = TemplateId::Status;

    uint8_t session_state{0};  // exchange::SessionState as uint8_t
    int64_t timestamp{0};      // epoch nanoseconds
};

struct FastSnapshot {
    static constexpr TemplateId TEMPLATE_ID = TemplateId::Snapshot;

    // Snapshot encodes top-of-book for both sides.
    int64_t bid_price{0};
    int64_t bid_qty{0};
    int64_t ask_price{0};
    int64_t ask_qty{0};
    uint32_t bid_count{0};   // number of bid levels
    uint32_t ask_count{0};   // number of ask levels
    int64_t timestamp{0};
};

struct FastInstrumentDef {
    static constexpr TemplateId TEMPLATE_ID = TemplateId::InstrumentDef;

    uint32_t instrument_id{0};      // unique ID from krx_products.h
    char     symbol[8]{};           // null-padded (e.g. "KS\0\0\0\0\0\0")
    char     description[32]{};     // null-padded
    uint8_t  product_group{0};      // KrxProductGroup as uint8_t
    int64_t  tick_size{0};          // engine fixed-point
    int64_t  lot_size{0};           // engine fixed-point
    int64_t  max_order_size{0};     // engine fixed-point
    uint32_t total_instruments{0};  // total count (for completion detection)
    int64_t  timestamp{0};          // epoch nanoseconds
};

// Maximum encoded size for any single FAST message.
// Maximum depth levels in a full snapshot.
constexpr int kSnapshotBookDepth = 5;

struct FastSnapshotLevel {
    int64_t  price{0};
    int64_t  quantity{0};
    uint32_t order_count{0};
};

struct FastFullSnapshot {
    static constexpr TemplateId TEMPLATE_ID = TemplateId::FullSnapshot;

    uint32_t instrument_id{0};
    uint32_t seq_num{0};            // for gap detection
    uint8_t  num_bid_levels{0};
    uint8_t  num_ask_levels{0};
    FastSnapshotLevel bids[kSnapshotBookDepth]{};
    FastSnapshotLevel asks[kSnapshotBookDepth]{};
    int64_t  timestamp{0};
};

// Maximum encoded size for any single FAST message.
// FullSnapshot is the largest: PMAP(2) + TemplateID(10) + instrument_id(5) +
// seq_num(5) + num_bid(1) + num_ask(1) + 10 levels * (i64+i64+u32 = 25) +
// timestamp(10) = ~284. Round up for safety.
constexpr size_t kMaxFastEncodedSize = 512;

}  // namespace exchange::krx::fast
