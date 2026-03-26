#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace exchange::ice::impact {

// ---------------------------------------------------------------------------
// iMpact message type codes — single-character identifiers per ICE spec.
// ---------------------------------------------------------------------------

constexpr char MSG_TYPE_ADD_MODIFY_ORDER = 'E';
constexpr char MSG_TYPE_ORDER_WITHDRAWAL = 'F';
constexpr char MSG_TYPE_DEAL_TRADE       = 'T';
constexpr char MSG_TYPE_MARKET_STATUS    = 'M';
constexpr char MSG_TYPE_BUNDLE_START     = 'S';
// NOTE: ICE uses 'E' for both AddModifyOrder and BundleEnd on the wire.
// Disambiguation happens by context (BundleEnd only appears after BundleStart
// and has a different msg_length). We define a separate constant for clarity.
constexpr char MSG_TYPE_BUNDLE_END       = 'e';

// ---------------------------------------------------------------------------
// Side and flag constants
// ---------------------------------------------------------------------------

constexpr char SIDE_BUY  = 'B';
constexpr char SIDE_SELL = 'S';
constexpr char FLAG_YES  = 'Y';
constexpr char FLAG_NO   = 'N';

// Trading status codes for MarketStatus message
constexpr char TRADING_STATUS_OPEN       = 'O';
constexpr char TRADING_STATUS_CLOSED     = 'C';
constexpr char TRADING_STATUS_PRE_OPEN   = 'P';
constexpr char TRADING_STATUS_HALT       = 'H';
constexpr char TRADING_STATUS_SETTLEMENT = 'S';

// ---------------------------------------------------------------------------
// iMpact common message header (3 bytes on wire).
//
// Every iMpact message starts with:
//   msg_type   (char, 1 byte)  — message type code
//   msg_length (uint16_t, LE)  — total message length including this header
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct ImpactMessageHeader {
    char     msg_type;      // offset 0
    uint16_t msg_length;    // offset 1, little-endian
};

static_assert(sizeof(ImpactMessageHeader) == 3,
    "ImpactMessageHeader must be 3 bytes");
static_assert(std::is_trivially_copyable_v<ImpactMessageHeader>);

// ---------------------------------------------------------------------------
// AddModifyOrder ('E') — order book add or modify.
//
// Sent when a new order enters the book or an existing order is modified.
// Fields are little-endian, packed with no alignment padding.
// ---------------------------------------------------------------------------

struct AddModifyOrder {
    static constexpr char TYPE = MSG_TYPE_ADD_MODIFY_ORDER;

    int32_t  market_id;              // offset 0
    int64_t  order_id;               // offset 4
    int32_t  order_seq_id;           // offset 12
    char     side;                   // offset 16 — 'B' or 'S'
    int64_t  price;                  // offset 17
    int32_t  quantity;               // offset 25
    char     is_implied;             // offset 29 — 'Y' or 'N'
    char     is_rfq;                 // offset 30 — 'Y' or 'N'
    int64_t  order_entry_date_time;  // offset 31
};

static_assert(sizeof(AddModifyOrder) == 39,
    "AddModifyOrder must be 39 bytes");
static_assert(std::is_trivially_copyable_v<AddModifyOrder>);

// ---------------------------------------------------------------------------
// OrderWithdrawal ('F') — order cancellation / withdrawal.
// ---------------------------------------------------------------------------

struct OrderWithdrawal {
    static constexpr char TYPE = MSG_TYPE_ORDER_WITHDRAWAL;

    int32_t  market_id;     // offset 0
    int64_t  order_id;      // offset 4
    int32_t  order_seq_id;  // offset 12
    char     side;          // offset 16 — 'B' or 'S'
    int64_t  price;         // offset 17
    int32_t  quantity;      // offset 25
};

static_assert(sizeof(OrderWithdrawal) == 29,
    "OrderWithdrawal must be 29 bytes");
static_assert(std::is_trivially_copyable_v<OrderWithdrawal>);

// ---------------------------------------------------------------------------
// DealTrade ('T') — executed trade / deal.
// ---------------------------------------------------------------------------

struct DealTrade {
    static constexpr char TYPE = MSG_TYPE_DEAL_TRADE;

    int32_t  market_id;         // offset 0
    int64_t  order_id;          // offset 4
    int64_t  deal_id;           // offset 12
    int64_t  price;             // offset 20
    int32_t  quantity;          // offset 28
    char     is_aggressor;      // offset 32 — 'Y' or 'N'
    char     aggressor_side;    // offset 33 — 'B' or 'S'
    int64_t  deal_date_time;    // offset 34
};

static_assert(sizeof(DealTrade) == 42,
    "DealTrade must be 42 bytes");
static_assert(std::is_trivially_copyable_v<DealTrade>);

// ---------------------------------------------------------------------------
// MarketStatus ('M') — trading status change for a market.
// ---------------------------------------------------------------------------

struct MarketStatus {
    static constexpr char TYPE = MSG_TYPE_MARKET_STATUS;

    int32_t  market_id;        // offset 0
    char     trading_status;   // offset 4 — 'O','C','P','H','S'
};

static_assert(sizeof(MarketStatus) == 5,
    "MarketStatus must be 5 bytes");
static_assert(std::is_trivially_copyable_v<MarketStatus>);

// ---------------------------------------------------------------------------
// BundleStart ('S') — marks the beginning of a message bundle.
// ---------------------------------------------------------------------------

struct BundleStart {
    static constexpr char TYPE = MSG_TYPE_BUNDLE_START;

    int32_t  seq_num;  // offset 0
};

static_assert(sizeof(BundleStart) == 4,
    "BundleStart must be 4 bytes");
static_assert(std::is_trivially_copyable_v<BundleStart>);

// ---------------------------------------------------------------------------
// BundleEnd — marks the end of a message bundle.
// ---------------------------------------------------------------------------

struct BundleEnd {
    static constexpr char TYPE = MSG_TYPE_BUNDLE_END;

    int32_t  seq_num;    // offset 0
    uint16_t msg_count;  // offset 4
};

static_assert(sizeof(BundleEnd) == 6,
    "BundleEnd must be 6 bytes");
static_assert(std::is_trivially_copyable_v<BundleEnd>);

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
// Buffer must be at least wire_size<MsgT>() bytes.
// ---------------------------------------------------------------------------

template <typename MsgT>
char* encode(char* buf, std::size_t buf_len, const MsgT& msg) {
    constexpr auto total = wire_size<MsgT>();
    if (buf_len < total) return nullptr;

    ImpactMessageHeader hdr{};
    hdr.msg_type   = MsgT::TYPE;
    hdr.msg_length = total;

    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), &msg, sizeof(msg));
    return buf + total;
}

// ---------------------------------------------------------------------------
// Decode: read header + message body from buffer.
// Returns pointer past the read bytes, or nullptr on error.
// Validates msg_type matches MsgT::TYPE and msg_length == wire_size<MsgT>().
// ---------------------------------------------------------------------------

template <typename MsgT>
const char* decode(const char* buf, std::size_t buf_len, MsgT& msg) {
    constexpr auto total = wire_size<MsgT>();
    if (buf_len < total) return nullptr;

    ImpactMessageHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.msg_type != MsgT::TYPE) return nullptr;
    if (hdr.msg_length != total) return nullptr;

    std::memcpy(&msg, buf + sizeof(hdr), sizeof(msg));
    return buf + total;
}

}  // namespace exchange::ice::impact
