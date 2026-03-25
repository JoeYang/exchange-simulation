#pragma once

#include <cstdint>
#include <cstring>
#include <endian.h>
#include <limits>

namespace exchange::cme::sbe {

// ---------------------------------------------------------------------------
// Endian helpers — CME SBE is little-endian on the wire.
// On little-endian hosts (x86) these compile to no-ops.
// ---------------------------------------------------------------------------

inline uint16_t encode_le16(uint16_t v) { return htole16(v); }
inline uint16_t decode_le16(uint16_t v) { return le16toh(v); }
inline uint32_t encode_le32(uint32_t v) { return htole32(v); }
inline uint32_t decode_le32(uint32_t v) { return le32toh(v); }
inline uint64_t encode_le64(uint64_t v) { return htole64(v); }
inline uint64_t decode_le64(uint64_t v) { return le64toh(v); }

inline int64_t encode_le64_signed(int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    u = htole64(u);
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline int64_t decode_le64_signed(int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    u = le64toh(u);
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// SBE Message Header — 8 bytes, present at the start of every SBE message.
//
// Schema reference:
//   iLink3  ilinkbinary.xml  line 101  (schemaId=8, version=5)
//   MDP3    templates_FixBinary.xml line 90  (schemaId=1, version=13)
// ---------------------------------------------------------------------------

struct alignas(2) MessageHeader {
    uint16_t block_length;  // root block size in bytes
    uint16_t template_id;   // SBE template identifier
    uint16_t schema_id;     // schema identifier
    uint16_t version;       // schema version

    void encode() {
        block_length = encode_le16(block_length);
        template_id  = encode_le16(template_id);
        schema_id    = encode_le16(schema_id);
        version      = encode_le16(version);
    }

    void decode() {
        block_length = decode_le16(block_length);
        template_id  = decode_le16(template_id);
        schema_id    = decode_le16(schema_id);
        version      = decode_le16(version);
    }

    // Encode into a buffer. Returns pointer past the header.
    char* encode_to(char* buf) const {
        MessageHeader h = *this;
        h.encode();
        std::memcpy(buf, &h, sizeof(h));
        return buf + sizeof(h);
    }

    // Decode from a buffer. Returns pointer past the header.
    static const char* decode_from(const char* buf, MessageHeader& out) {
        std::memcpy(&out, buf, sizeof(out));
        out.decode();
        return buf + sizeof(out);
    }
};

static_assert(sizeof(MessageHeader) == 8, "SBE MessageHeader must be 8 bytes");

// ---------------------------------------------------------------------------
// SBE Group Header — used for repeating groups.
//
// Standard 3-byte group dimensions (iLink3 groupSize, MDP3 groupSize):
//   blockLength (uint16) + numInGroup (uint8)
// ---------------------------------------------------------------------------

struct __attribute__((packed)) GroupHeader {
    uint16_t block_length;
    uint8_t  num_in_group;

    void encode() { block_length = encode_le16(block_length); }
    void decode() { block_length = decode_le16(block_length); }

    char* encode_to(char* buf) const {
        GroupHeader h = *this;
        h.encode();
        std::memcpy(buf, &h, sizeof(h));
        return buf + sizeof(h);
    }

    static const char* decode_from(const char* buf, GroupHeader& out) {
        std::memcpy(&out, buf, sizeof(out));
        out.decode();
        return buf + sizeof(out);
    }
};

static_assert(sizeof(GroupHeader) == 3, "SBE GroupHeader must be 3 bytes");

// 4-byte group dimensions (MDP3 groupSizeEncoding): both fields uint16
struct __attribute__((packed)) GroupHeader16 {
    uint16_t block_length;
    uint16_t num_in_group;

    void encode() {
        block_length = encode_le16(block_length);
        num_in_group = encode_le16(num_in_group);
    }
    void decode() {
        block_length = decode_le16(block_length);
        num_in_group = decode_le16(num_in_group);
    }
};

static_assert(sizeof(GroupHeader16) == 4, "SBE GroupHeader16 must be 4 bytes");

// ---------------------------------------------------------------------------
// Null sentinel values — derived from the XML schemas.
// Used by optional fields: a field holding its null value means "not present".
// ---------------------------------------------------------------------------

constexpr uint8_t  UINT8_NULL  = 255;
constexpr uint16_t UINT16_NULL = 65535;
constexpr uint32_t UINT32_NULL = 4294967295u;
constexpr uint64_t UINT64_NULL = 18446744073709551615ull;
constexpr int32_t  INT32_NULL  = 2147483647;
constexpr int64_t  INT64_NULL  = 9223372036854775807ll;
constexpr int8_t   INT8_NULL   = 127;
constexpr char     CHAR_NULL   = '\0';
constexpr uint8_t  ENUM_NULL   = 255;

inline bool is_null(uint8_t  v) { return v == UINT8_NULL; }
inline bool is_null(uint16_t v) { return v == UINT16_NULL; }
inline bool is_null(uint32_t v) { return v == UINT32_NULL; }
inline bool is_null(uint64_t v) { return v == UINT64_NULL; }
inline bool is_null_i32(int32_t v) { return v == INT32_NULL; }
inline bool is_null_i64(int64_t v) { return v == INT64_NULL; }

// ---------------------------------------------------------------------------
// PRICE9 — fixed-point price with exponent = -9.
//
// Mantissa is int64; actual value = mantissa * 1e-9.
// Used in both iLink3 (PRICE9/PRICENULL9) and MDP3 (PRICE9/PRICENULL9).
// ---------------------------------------------------------------------------

constexpr int64_t PRICE9_EXPONENT = -9;
constexpr double  PRICE9_SCALE    = 1e9;

struct PRICE9 {
    int64_t mantissa;

    double to_double() const {
        return static_cast<double>(mantissa) / PRICE9_SCALE;
    }

    static PRICE9 from_double(double price) {
        return {static_cast<int64_t>(price * PRICE9_SCALE + (price >= 0 ? 0.5 : -0.5))};
    }

    bool is_null() const { return mantissa == INT64_NULL; }

    void encode() { mantissa = encode_le64_signed(mantissa); }
    void decode() { mantissa = decode_le64_signed(mantissa); }
};

static_assert(sizeof(PRICE9) == 8, "PRICE9 must be 8 bytes");

// ---------------------------------------------------------------------------
// Decimal9 — identical wire format to PRICE9 (exponent = -9).
// MDP3 uses this name (templates_FixBinary.xml line 52).
// ---------------------------------------------------------------------------

using Decimal9 = PRICE9;

// ---------------------------------------------------------------------------
// MaturityMonthYear — 5-byte composite for contract expiry.
//
// Fields: year(uint16), month(uint8), day(uint8), week(uint8).
// Null values: year=65535, month=255, day=255, week=255.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) MaturityMonthYear {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  week;

    bool is_null() const { return year == UINT16_NULL; }

    void encode() { year = encode_le16(year); }
    void decode() { year = decode_le16(year); }
};

static_assert(sizeof(MaturityMonthYear) == 5, "MaturityMonthYear must be 5 bytes");

// ---------------------------------------------------------------------------
// Schema constants
// ---------------------------------------------------------------------------

constexpr uint16_t ILINK3_SCHEMA_ID = 8;
constexpr uint16_t ILINK3_VERSION   = 5;
constexpr uint16_t MDP3_SCHEMA_ID   = 1;
constexpr uint16_t MDP3_VERSION     = 13;

}  // namespace exchange::cme::sbe
