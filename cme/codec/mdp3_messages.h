#pragma once

#include "cme/codec/sbe_header.h"

#include <cstdint>
#include <cstring>

namespace exchange::cme::sbe::mdp3 {

// ---------------------------------------------------------------------------
// Template IDs — used for message dispatch after decoding the SBE header.
// ---------------------------------------------------------------------------

constexpr uint16_t SECURITY_STATUS_30_ID                      = 30;
constexpr uint16_t MD_INCREMENTAL_REFRESH_BOOK_46_ID          = 46;
constexpr uint16_t MD_INCREMENTAL_REFRESH_TRADE_SUMMARY_48_ID = 48;
constexpr uint16_t SNAPSHOT_FULL_REFRESH_ORDER_BOOK_53_ID     = 53;
constexpr uint16_t MD_INSTRUMENT_DEFINITION_FUTURE_54_ID      = 54;

// ---------------------------------------------------------------------------
// MDP3 Enum types
// ---------------------------------------------------------------------------

enum class MDUpdateAction : uint8_t {
    New        = 0,
    Change     = 1,
    Delete     = 2,
    DeleteThru = 3,
    DeleteFrom = 4,
    Overlay    = 5,
};

enum class AggressorSide : uint8_t {
    NoAggressor = 0,
    Buy         = 1,
    Sell        = 2,
};

enum class MDEntryTypeBook : char {
    Bid          = '0',
    Offer        = '1',
    ImpliedBid   = 'E',
    ImpliedOffer = 'F',
    BookReset    = 'J',
};

enum class OrderUpdateAction : uint8_t {
    New    = 0,
    Update = 1,
    Delete = 2,
};

enum class HaltReason : uint8_t {
    GroupSchedule            = 0,
    SurveillanceIntervention = 1,
    MarketEvent              = 2,
    InstrumentActivation     = 3,
    InstrumentExpiration     = 4,
    Unknown                  = 5,
    RecoveryInProcess        = 6,
    TradeDateRoll            = 7,
};

enum class SecurityTradingStatus : uint8_t {
    TradingHalt             = 2,
    Close                   = 4,
    NewPriceIndication      = 15,
    ReadyToTrade            = 17,
    NotAvailableForTrading  = 18,
    UnknownOrInvalid        = 20,
    PreOpen                 = 21,
    PreCross                = 24,
    Cross                   = 25,
    PostClose               = 26,
    NoChange                = 103,
    PrivateWorkup           = 201,
    PublicWorkup            = 202,
};

enum class SecurityTradingEvent : uint8_t {
    NoEvent           = 0,
    NoCancel          = 1,
    ResetStatistics   = 4,
    ImpliedMatchingON = 5,
    ImpliedMatchingOFF = 6,
    EndOfWorkup       = 7,
};

enum class SecurityUpdateAction : char {
    Add    = 'A',
    Delete = 'D',
    Modify = 'M',
};

enum class EventType : uint8_t {
    Activation            = 5,
    LastEligibleTradeDate = 7,
};

// ---------------------------------------------------------------------------
// 8-byte aligned group header (groupSize8Byte).
//
// Wire layout: blockLength(uint16) at offset 0, numInGroup(uint8) at offset 7.
// Total size = 8 bytes. Bytes 2-6 are padding.
// Used by NoOrderIDEntries groups in message 46 and 48.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) GroupHeader8Byte {
    uint16_t block_length;
    uint8_t  padding[5];
    uint8_t  num_in_group;

    void encode() { block_length = encode_le16(block_length); }
    void decode() { block_length = decode_le16(block_length); }

    char* encode_to(char* buf) const {
        GroupHeader8Byte h = *this;
        h.encode();
        std::memcpy(buf, &h, sizeof(h));
        return buf + sizeof(h);
    }

    static const char* decode_from(const char* buf, GroupHeader8Byte& out) {
        std::memcpy(&out, buf, sizeof(out));
        out.decode();
        return buf + sizeof(out);
    }
};

static_assert(sizeof(GroupHeader8Byte) == 8, "GroupHeader8Byte must be 8 bytes");

// ---------------------------------------------------------------------------
// SecurityStatus30 (id=30)
//
// blockLength = 30 bytes.  Schema: templates_FixBinary.xml line 315.
// Flat message — no repeating groups.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) SecurityStatus30 {
    static constexpr uint16_t TEMPLATE_ID  = SECURITY_STATUS_30_ID;
    static constexpr uint16_t BLOCK_LENGTH = 30;

    uint64_t transact_time;               // offset 0  — nanoseconds since epoch
    char     security_group[6];           // offset 8  — SecurityGroup
    char     asset[6];                    // offset 14 — Asset
    int32_t  security_id;                 // offset 20 — Int32NULL (null=2147483647)
    uint16_t trade_date;                  // offset 24 — LocalMktDate
    uint8_t  match_event_indicator;       // offset 26 — MatchEventIndicator bitmap
    uint8_t  security_trading_status;     // offset 27 — SecurityTradingStatus
    uint8_t  halt_reason;                 // offset 28 — HaltReason
    uint8_t  security_trading_event;      // offset 29 — SecurityTradingEvent
};

static_assert(sizeof(SecurityStatus30) == 30,
    "SecurityStatus30 must be 30 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// MDIncrementalRefreshBook46 (id=46)
//
// blockLength = 11 bytes.  Schema: templates_FixBinary.xml line 349.
// Repeating groups: NoMDEntries (groupSize, 32B/entry),
//                   NoOrderIDEntries (groupSize8Byte, 24B/entry).
// ---------------------------------------------------------------------------

struct __attribute__((packed)) MDIncrementalRefreshBook46 {
    static constexpr uint16_t TEMPLATE_ID  = MD_INCREMENTAL_REFRESH_BOOK_46_ID;
    static constexpr uint16_t BLOCK_LENGTH = 11;

    uint64_t transact_time;               // offset 0
    uint8_t  match_event_indicator;       // offset 8
    // 2 padding bytes to reach blockLength=11
    uint8_t  padding[2];                  // offset 9
};

static_assert(sizeof(MDIncrementalRefreshBook46) == 11,
    "MDIncrementalRefreshBook46 must be 11 bytes (schema blockLength)");

// NoMDEntries group entry — blockLength = 32 bytes.
struct __attribute__((packed)) RefreshBookEntry {
    static constexpr uint16_t BLOCK_LENGTH = 32;

    PRICE9   md_entry_px;                 // offset 0  — PRICENULL9
    int32_t  md_entry_size;               // offset 8  — Int32NULL
    int32_t  security_id;                 // offset 12
    uint32_t rpt_seq;                     // offset 16
    int32_t  number_of_orders;            // offset 20 — Int32NULL
    uint8_t  md_price_level;              // offset 24
    uint8_t  md_update_action;            // offset 25 — MDUpdateAction
    char     md_entry_type;               // offset 26 — MDEntryTypeBook
    int32_t  tradeable_size;              // offset 27 — Int32NULL (sinceVersion=10)
    uint8_t  padding;                     // offset 31
};

static_assert(sizeof(RefreshBookEntry) == 32,
    "RefreshBookEntry must be 32 bytes (schema blockLength)");

// NoOrderIDEntries group entry — blockLength = 24 bytes (groupSize8Byte).
struct __attribute__((packed)) RefreshBookOrderEntry {
    static constexpr uint16_t BLOCK_LENGTH = 24;

    uint64_t order_id;                    // offset 0
    uint64_t md_order_priority;           // offset 8  — uInt64NULL
    int32_t  md_display_qty;              // offset 16 — Int32NULL
    uint8_t  reference_id;                // offset 20 — uInt8NULL
    uint8_t  order_update_action;         // offset 21 — OrderUpdateAction
    uint8_t  padding[2];                  // offset 22
};

static_assert(sizeof(RefreshBookOrderEntry) == 24,
    "RefreshBookOrderEntry must be 24 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// MDIncrementalRefreshTradeSummary48 (id=48)
//
// blockLength = 11 bytes.  Schema: templates_FixBinary.xml line 384.
// Repeating groups: NoMDEntries (groupSize, 32B/entry),
//                   NoOrderIDEntries (groupSize8Byte, 16B/entry).
// ---------------------------------------------------------------------------

struct __attribute__((packed)) MDIncrementalRefreshTradeSummary48 {
    static constexpr uint16_t TEMPLATE_ID  = MD_INCREMENTAL_REFRESH_TRADE_SUMMARY_48_ID;
    static constexpr uint16_t BLOCK_LENGTH = 11;

    uint64_t transact_time;               // offset 0
    uint8_t  match_event_indicator;       // offset 8
    uint8_t  padding[2];                  // offset 9
};

static_assert(sizeof(MDIncrementalRefreshTradeSummary48) == 11,
    "MDIncrementalRefreshTradeSummary48 must be 11 bytes (schema blockLength)");

// NoMDEntries group entry — blockLength = 32 bytes.
struct __attribute__((packed)) TradeSummaryEntry {
    static constexpr uint16_t BLOCK_LENGTH = 32;

    PRICE9   md_entry_px;                 // offset 0  — PRICE9
    int32_t  md_entry_size;               // offset 8
    int32_t  security_id;                 // offset 12
    uint32_t rpt_seq;                     // offset 16
    int32_t  number_of_orders;            // offset 20
    uint8_t  aggressor_side;              // offset 24 — AggressorSide
    uint8_t  md_update_action;            // offset 25 — MDUpdateAction
    uint32_t md_trade_entry_id;           // offset 26 — uInt32NULL
    uint8_t  padding[2];                  // offset 30
};

static_assert(sizeof(TradeSummaryEntry) == 32,
    "TradeSummaryEntry must be 32 bytes (schema blockLength)");

// NoOrderIDEntries group entry — blockLength = 16 bytes (groupSize8Byte).
struct __attribute__((packed)) TradeSummaryOrderEntry {
    static constexpr uint16_t BLOCK_LENGTH = 16;

    uint64_t order_id;                    // offset 0
    int32_t  last_qty;                    // offset 8
    uint8_t  padding[4];                  // offset 12
};

static_assert(sizeof(TradeSummaryOrderEntry) == 16,
    "TradeSummaryOrderEntry must be 16 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// SnapshotFullRefreshOrderBook53 (id=53)
//
// blockLength = 28 bytes.  Schema: templates_FixBinary.xml line 466.
// Repeating groups: NoMDEntries (groupSize, 29B/entry).
// ---------------------------------------------------------------------------

struct __attribute__((packed)) SnapshotFullRefreshOrderBook53 {
    static constexpr uint16_t TEMPLATE_ID  = SNAPSHOT_FULL_REFRESH_ORDER_BOOK_53_ID;
    static constexpr uint16_t BLOCK_LENGTH = 28;

    uint32_t last_msg_seq_num_processed;  // offset 0
    uint32_t tot_num_reports;             // offset 4
    int32_t  security_id;                 // offset 8
    uint32_t no_chunks;                   // offset 12
    uint32_t current_chunk;               // offset 16
    uint64_t transact_time;               // offset 20
};

static_assert(sizeof(SnapshotFullRefreshOrderBook53) == 28,
    "SnapshotFullRefreshOrderBook53 must be 28 bytes (schema blockLength)");

// NoMDEntries group entry — blockLength = 29 bytes.
struct __attribute__((packed)) SnapshotOrderBookEntry {
    static constexpr uint16_t BLOCK_LENGTH = 29;

    uint64_t order_id;                    // offset 0
    uint64_t md_order_priority;           // offset 8  — uInt64NULL
    PRICE9   md_entry_px;                 // offset 16 — PRICE9
    int32_t  md_display_qty;              // offset 24
    char     md_entry_type;               // offset 28 — MDEntryTypeBook
};

static_assert(sizeof(SnapshotOrderBookEntry) == 29,
    "SnapshotOrderBookEntry must be 29 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// MDInstrumentDefinitionFuture54 (id=54)
//
// blockLength = 224 bytes.  Schema: templates_FixBinary.xml line 481.
// Repeating groups: NoEvents (9B), NoMDFeedTypes (4B),
//                   NoInstAttrib (4B), NoLotTypeRules (5B).
// All use standard groupSize (3-byte header).
// ---------------------------------------------------------------------------

struct __attribute__((packed)) MDInstrumentDefinitionFuture54 {
    static constexpr uint16_t TEMPLATE_ID  = MD_INSTRUMENT_DEFINITION_FUTURE_54_ID;
    static constexpr uint16_t BLOCK_LENGTH = 224;

    uint8_t  match_event_indicator;       // offset 0
    uint32_t tot_num_reports;             // offset 1  — uInt32NULL
    char     security_update_action;      // offset 5  — SecurityUpdateAction
    uint64_t last_update_time;            // offset 6
    uint8_t  md_security_trading_status;  // offset 14 — SecurityTradingStatus
    int16_t  appl_id;                     // offset 15
    uint8_t  market_segment_id;           // offset 17
    uint8_t  underlying_product;          // offset 18
    char     security_exchange[4];        // offset 19
    char     security_group[6];           // offset 23
    char     asset[6];                    // offset 29
    char     symbol[20];                  // offset 35
    int32_t  security_id;                 // offset 55
    // SecurityIDSource is constant '8', no wire field
    char     security_type[6];            // offset 59
    char     cfi_code[6];                 // offset 65
    MaturityMonthYear maturity_month_year; // offset 71 — 5 bytes
    char     currency[3];                 // offset 76
    char     settl_currency[3];           // offset 79
    char     match_algorithm;             // offset 82
    uint32_t min_trade_vol;               // offset 83
    uint32_t max_trade_vol;               // offset 87
    PRICE9   min_price_increment;         // offset 91  — PRICE9
    Decimal9 display_factor;              // offset 99  — Decimal9
    uint8_t  main_fraction;               // offset 107 — uInt8NULL
    uint8_t  sub_fraction;                // offset 108 — uInt8NULL
    uint8_t  price_display_format;        // offset 109 — uInt8NULL
    char     unit_of_measure[30];         // offset 110
    PRICE9   unit_of_measure_qty;         // offset 140 — Decimal9NULL (PRICENULL9)
    PRICE9   trading_reference_price;     // offset 148 — PRICENULL9
    uint8_t  settl_price_type;            // offset 156 — SettlPriceType bitmap
    int32_t  open_interest_qty;           // offset 157 — Int32NULL
    int32_t  cleared_volume;              // offset 161 — Int32NULL
    PRICE9   high_limit_price;            // offset 165 — PRICENULL9
    PRICE9   low_limit_price;             // offset 173 — PRICENULL9
    PRICE9   max_price_variation;         // offset 181 — PRICENULL9
    int32_t  decay_quantity;              // offset 189 — Int32NULL
    uint16_t decay_start_date;            // offset 193 — LocalMktDate
    int32_t  original_contract_size;      // offset 195 — Int32NULL
    int32_t  contract_multiplier;         // offset 199 — Int32NULL
    int8_t   contract_multiplier_unit;    // offset 203 — Int8NULL
    int8_t   flow_schedule_type;          // offset 204 — Int8NULL
    PRICE9   min_price_increment_amount;  // offset 205 — PRICENULL9
    char     user_defined_instrument;     // offset 213
    uint16_t trading_reference_date;      // offset 214 — LocalMktDate
    uint64_t instrument_guid;             // offset 216 — uInt64NULL (sinceVersion=10)
};

static_assert(sizeof(MDInstrumentDefinitionFuture54) == 224,
    "MDInstrumentDefinitionFuture54 must be 224 bytes (schema blockLength)");

// NoEvents group entry — blockLength = 9 bytes.
struct __attribute__((packed)) InstrDefEventEntry {
    static constexpr uint16_t BLOCK_LENGTH = 9;

    uint8_t  event_type;                  // offset 0 — EventType
    uint64_t event_time;                  // offset 1 — nanoseconds since epoch
};

static_assert(sizeof(InstrDefEventEntry) == 9,
    "InstrDefEventEntry must be 9 bytes (schema blockLength)");

// NoMDFeedTypes group entry — blockLength = 4 bytes.
struct __attribute__((packed)) InstrDefFeedTypeEntry {
    static constexpr uint16_t BLOCK_LENGTH = 4;

    char    md_feed_type[3];              // offset 0 — MDFeedType ("GBX","GBI",etc.)
    int8_t  market_depth;                 // offset 3
};

static_assert(sizeof(InstrDefFeedTypeEntry) == 4,
    "InstrDefFeedTypeEntry must be 4 bytes (schema blockLength)");

// NoInstAttrib group entry — blockLength = 4 bytes.
// InstAttribType is constant 24 (no wire field).
struct __attribute__((packed)) InstrDefAttribEntry {
    static constexpr uint16_t BLOCK_LENGTH = 4;

    uint32_t inst_attrib_value;           // offset 0 — InstAttribValue bitmap
};

static_assert(sizeof(InstrDefAttribEntry) == 4,
    "InstrDefAttribEntry must be 4 bytes (schema blockLength)");

// NoLotTypeRules group entry — blockLength = 5 bytes.
// DecimalQty = int32 mantissa with exponent -4.
struct __attribute__((packed)) InstrDefLotTypeEntry {
    static constexpr uint16_t BLOCK_LENGTH = 5;

    int8_t  lot_type;                     // offset 0
    int32_t min_lot_size;                 // offset 1 — DecimalQty mantissa (exp=-4)
};

static_assert(sizeof(InstrDefLotTypeEntry) == 5,
    "InstrDefLotTypeEntry must be 5 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Helper: build a MessageHeader for a given MDP3 message type.
// ---------------------------------------------------------------------------

template <typename MsgT>
constexpr MessageHeader make_header() {
    return MessageHeader{
        MsgT::BLOCK_LENGTH,
        MsgT::TEMPLATE_ID,
        MDP3_SCHEMA_ID,
        MDP3_VERSION
    };
}

}  // namespace exchange::cme::sbe::mdp3
