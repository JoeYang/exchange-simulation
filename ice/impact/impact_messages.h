#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace exchange::ice::impact {

// ---------------------------------------------------------------------------
// iMpact message type codes — single-character identifiers per ICE spec.
// ---------------------------------------------------------------------------

enum class MessageType : char {
    AddModifyOrder  = 'E',
    OrderWithdrawal = 'F',
    DealTrade       = 'T',
    MarketStatus    = 'M',
    BundleStart     = 'S',
    BundleEnd       = 'e',  // ICE wire uses context-dependent disambiguation
    SnapshotOrder   = 'D',
    PriceLevel      = 'L',
};

// ---------------------------------------------------------------------------
// Typed enums for Side and TradingStatus
// ---------------------------------------------------------------------------

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1,
};

enum class TradingStatus : uint8_t {
    PreOpen    = 0,
    Continuous = 1,
    Halt       = 2,
    Closed     = 3,
    Settlement = 4,
};

// Flags for is_implied, is_rfq, is_aggressor
enum class YesNo : uint8_t {
    No  = 0,
    Yes = 1,
};

// ---------------------------------------------------------------------------
// iMpact common message header (3 bytes on wire).
//
// Every iMpact message starts with:
//   msg_type    (char, 1 byte)    — message type code
//   body_length (uint16_t, LE)    — total message length including this header
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct ImpactMessageHeader {
    char     msg_type;      // offset 0
    uint16_t body_length;   // offset 1, little-endian, total wire size
};

static_assert(sizeof(ImpactMessageHeader) == 3,
    "ImpactMessageHeader must be 3 bytes");
static_assert(std::is_trivially_copyable_v<ImpactMessageHeader>);

// ---------------------------------------------------------------------------
// BundleStart ('S') — marks the beginning of a message bundle.
// ---------------------------------------------------------------------------

struct BundleStart {
    static constexpr char TYPE = static_cast<char>(MessageType::BundleStart);

    uint32_t sequence_number;  // offset 0
    uint16_t message_count;    // offset 4
    int64_t  timestamp;        // offset 6 — epoch nanoseconds
};

static_assert(sizeof(BundleStart) == 14,
    "BundleStart must be 14 bytes");
static_assert(std::is_trivially_copyable_v<BundleStart>);

// ---------------------------------------------------------------------------
// BundleEnd — marks the end of a message bundle.
// ---------------------------------------------------------------------------

struct BundleEnd {
    static constexpr char TYPE = static_cast<char>(MessageType::BundleEnd);

    uint32_t sequence_number;  // offset 0
};

static_assert(sizeof(BundleEnd) == 4,
    "BundleEnd must be 4 bytes");
static_assert(std::is_trivially_copyable_v<BundleEnd>);

// ---------------------------------------------------------------------------
// AddModifyOrder ('E') — order book add or modify.
// ---------------------------------------------------------------------------

struct AddModifyOrder {
    static constexpr char TYPE = static_cast<char>(MessageType::AddModifyOrder);

    int32_t  instrument_id;          // offset 0
    int64_t  order_id;               // offset 4
    uint32_t sequence_within_msg;    // offset 12
    uint8_t  side;                   // offset 16 — Side enum
    int64_t  price;                  // offset 17 — fixed-point
    uint32_t quantity;               // offset 25
    uint8_t  is_implied;             // offset 29 — YesNo enum
    uint8_t  is_rfq;                 // offset 30 — YesNo enum
    int64_t  order_entry_date_time;  // offset 31 — epoch nanoseconds
};

static_assert(sizeof(AddModifyOrder) == 39,
    "AddModifyOrder must be 39 bytes");
static_assert(std::is_trivially_copyable_v<AddModifyOrder>);

// ---------------------------------------------------------------------------
// OrderWithdrawal ('F') — order cancellation / withdrawal.
// ---------------------------------------------------------------------------

struct OrderWithdrawal {
    static constexpr char TYPE = static_cast<char>(MessageType::OrderWithdrawal);

    int32_t  instrument_id;        // offset 0
    int64_t  order_id;             // offset 4
    uint32_t sequence_within_msg;  // offset 12
    uint8_t  side;                 // offset 16 — Side enum
    int64_t  price;                // offset 17 — fixed-point
    uint32_t quantity;             // offset 25
};

static_assert(sizeof(OrderWithdrawal) == 29,
    "OrderWithdrawal must be 29 bytes");
static_assert(std::is_trivially_copyable_v<OrderWithdrawal>);

// ---------------------------------------------------------------------------
// DealTrade ('T') — executed trade / deal.
// ---------------------------------------------------------------------------

struct DealTrade {
    static constexpr char TYPE = static_cast<char>(MessageType::DealTrade);

    int32_t  instrument_id;     // offset 0
    int64_t  deal_id;           // offset 4
    int64_t  price;             // offset 12 — fixed-point
    uint32_t quantity;          // offset 20
    uint8_t  aggressor_side;    // offset 24 — Side enum
    int64_t  timestamp;         // offset 25 — epoch nanoseconds
};

static_assert(sizeof(DealTrade) == 33,
    "DealTrade must be 33 bytes");
static_assert(std::is_trivially_copyable_v<DealTrade>);

// ---------------------------------------------------------------------------
// MarketStatus ('M') — trading status change for a market.
// ---------------------------------------------------------------------------

struct MarketStatus {
    static constexpr char TYPE = static_cast<char>(MessageType::MarketStatus);

    int32_t  instrument_id;    // offset 0
    uint8_t  trading_status;   // offset 4 — TradingStatus enum
};

static_assert(sizeof(MarketStatus) == 5,
    "MarketStatus must be 5 bytes");
static_assert(std::is_trivially_copyable_v<MarketStatus>);

// ---------------------------------------------------------------------------
// SnapshotOrder ('D') — full book snapshot order entry.
//
// Sent during initial snapshot synchronization (not incremental).
// ---------------------------------------------------------------------------

struct SnapshotOrder {
    static constexpr char TYPE = static_cast<char>(MessageType::SnapshotOrder);

    int32_t  instrument_id;   // offset 0
    int64_t  order_id;        // offset 4
    uint8_t  side;            // offset 12 — Side enum
    int64_t  price;           // offset 13 — fixed-point
    uint32_t quantity;        // offset 21
    uint32_t sequence;        // offset 25
};

static_assert(sizeof(SnapshotOrder) == 29,
    "SnapshotOrder must be 29 bytes");
static_assert(std::is_trivially_copyable_v<SnapshotOrder>);

// ---------------------------------------------------------------------------
// PriceLevel — aggregated price level (MBP top-of-book / top-5).
// ---------------------------------------------------------------------------

struct PriceLevel {
    static constexpr char TYPE = static_cast<char>(MessageType::PriceLevel);

    int32_t  instrument_id;   // offset 0
    uint8_t  side;            // offset 4 — Side enum
    int64_t  price;           // offset 5 — fixed-point
    uint32_t quantity;        // offset 13
    uint16_t order_count;     // offset 17
};

static_assert(sizeof(PriceLevel) == 19,
    "PriceLevel must be 19 bytes");
static_assert(std::is_trivially_copyable_v<PriceLevel>);

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Wire-size helpers: total message size = header + payload.
// ---------------------------------------------------------------------------

template <typename MsgT>
constexpr uint16_t wire_size() {
    return static_cast<uint16_t>(sizeof(ImpactMessageHeader) + sizeof(MsgT));
}

// ---------------------------------------------------------------------------
// Encode: write header + message body into buffer.
// Returns pointer past the written bytes, or nullptr if buf_len is too small.
// ---------------------------------------------------------------------------

template <typename MsgT>
char* encode(char* buf, std::size_t buf_len, const MsgT& msg) {
    constexpr auto total = wire_size<MsgT>();
    if (buf_len < total) return nullptr;

    ImpactMessageHeader hdr{};
    hdr.msg_type    = MsgT::TYPE;
    hdr.body_length = total;

    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), &msg, sizeof(msg));
    return buf + total;
}

// ---------------------------------------------------------------------------
// Decode: read header + message body from buffer.
// Returns pointer past the read bytes, or nullptr on error.
// Validates msg_type matches MsgT::TYPE and body_length == wire_size<MsgT>().
// ---------------------------------------------------------------------------

template <typename MsgT>
const char* decode(const char* buf, std::size_t buf_len, MsgT& msg) {
    constexpr auto total = wire_size<MsgT>();
    if (buf_len < total) return nullptr;

    ImpactMessageHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.msg_type != MsgT::TYPE) return nullptr;
    if (hdr.body_length != total) return nullptr;

    std::memcpy(&msg, buf + sizeof(hdr), sizeof(msg));
    return buf + total;
}

}  // namespace exchange::ice::impact
