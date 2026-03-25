#pragma once

#include "cme/codec/mdp3_messages.h"
#include "cme/codec/sbe_header.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace exchange::cme::sbe::mdp3 {

// ---------------------------------------------------------------------------
// Decode result: holds decoded root block + group entries for one message.
//
// Groups use fixed-size arrays sized to practical maximums. The num_entries
// field tracks how many entries were actually decoded.
// ---------------------------------------------------------------------------

// Max entries per group in a single message. CME practical limit is ~20
// depth levels. 64 is generous headroom with no heap allocation.
constexpr size_t MAX_MD_ENTRIES = 64;
constexpr size_t MAX_ORDER_ENTRIES = 64;

struct DecodedRefreshBook46 {
    MDIncrementalRefreshBook46 root;
    RefreshBookEntry           md_entries[MAX_MD_ENTRIES];
    uint8_t                    num_md_entries;
    RefreshBookOrderEntry      order_entries[MAX_ORDER_ENTRIES];
    uint8_t                    num_order_entries;
};

struct DecodedTradeSummary48 {
    MDIncrementalRefreshTradeSummary48 root;
    TradeSummaryEntry                  md_entries[MAX_MD_ENTRIES];
    uint8_t                            num_md_entries;
    TradeSummaryOrderEntry             order_entries[MAX_ORDER_ENTRIES];
    uint8_t                            num_order_entries;
};

struct DecodedSecurityStatus30 {
    SecurityStatus30 root;
};

constexpr size_t MAX_SNAPSHOT_ENTRIES = 256;

struct DecodedSnapshot53 {
    SnapshotFullRefreshOrderBook53 root;
    SnapshotOrderBookEntry         md_entries[MAX_SNAPSHOT_ENTRIES];
    uint16_t                       num_md_entries;
};

constexpr size_t MAX_EVENT_ENTRIES = 16;
constexpr size_t MAX_FEED_TYPE_ENTRIES = 8;
constexpr size_t MAX_ATTRIB_ENTRIES = 8;
constexpr size_t MAX_LOT_TYPE_ENTRIES = 8;

struct DecodedInstrumentDef54 {
    MDInstrumentDefinitionFuture54 root;
    InstrDefEventEntry             events[MAX_EVENT_ENTRIES];
    uint8_t                        num_events;
    InstrDefFeedTypeEntry          feed_types[MAX_FEED_TYPE_ENTRIES];
    uint8_t                        num_feed_types;
    InstrDefAttribEntry            attribs[MAX_ATTRIB_ENTRIES];
    uint8_t                        num_attribs;
    InstrDefLotTypeEntry           lot_types[MAX_LOT_TYPE_ENTRIES];
    uint8_t                        num_lot_types;
};

// ---------------------------------------------------------------------------
// Decode error codes — no exceptions on the hot path.
// ---------------------------------------------------------------------------

enum class DecodeResult : uint8_t {
    kOk,
    kBufferTooShort,
    kUnknownTemplateId,
    kGroupOverflow,     // More entries than MAX_*_ENTRIES.
    kBadBlockLength,
    kBadSchemaId,       // schemaId != MDP3_SCHEMA_ID.
};

// ---------------------------------------------------------------------------
// Decoder functions.
//
// Each takes a pointer past the MessageHeader (i.e., start of the root block)
// plus the decoded header for block_length context.
//
// Returns DecodeResult::kOk on success; output struct is populated.
// No heap allocation. No exceptions.
// ---------------------------------------------------------------------------

inline DecodeResult decode_security_status_30(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedSecurityStatus30& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;
    std::memcpy(&out.root, buf, sizeof(out.root));
    return DecodeResult::kOk;
}

inline DecodeResult decode_refresh_book_46(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedRefreshBook46& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;

    // Root block.
    std::memcpy(&out.root, buf, sizeof(out.root));
    const char* p = buf + hdr.block_length;
    size_t remaining = len - hdr.block_length;

    // NoMDEntries group (GroupHeader = 3 bytes).
    if (remaining < sizeof(GroupHeader)) return DecodeResult::kBufferTooShort;
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    remaining -= sizeof(GroupHeader);

    if (gh.num_in_group > MAX_MD_ENTRIES) return DecodeResult::kGroupOverflow;
    out.num_md_entries = gh.num_in_group;

    for (uint8_t i = 0; i < gh.num_in_group; ++i) {
        if (remaining < gh.block_length) return DecodeResult::kBufferTooShort;
        std::memcpy(&out.md_entries[i], p, sizeof(RefreshBookEntry));
        p += gh.block_length;
        remaining -= gh.block_length;
    }

    // NoOrderIDEntries group (GroupHeader8Byte = 8 bytes).
    if (remaining < sizeof(GroupHeader8Byte)) return DecodeResult::kBufferTooShort;
    GroupHeader8Byte gh8{};
    p = GroupHeader8Byte::decode_from(p, gh8);
    remaining -= sizeof(GroupHeader8Byte);

    if (gh8.num_in_group > MAX_ORDER_ENTRIES) return DecodeResult::kGroupOverflow;
    out.num_order_entries = gh8.num_in_group;

    for (uint8_t i = 0; i < gh8.num_in_group; ++i) {
        if (remaining < gh8.block_length) return DecodeResult::kBufferTooShort;
        std::memcpy(&out.order_entries[i], p, sizeof(RefreshBookOrderEntry));
        p += gh8.block_length;
        remaining -= gh8.block_length;
    }

    return DecodeResult::kOk;
}

inline DecodeResult decode_trade_summary_48(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedTradeSummary48& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;

    // Root block.
    std::memcpy(&out.root, buf, sizeof(out.root));
    const char* p = buf + hdr.block_length;
    size_t remaining = len - hdr.block_length;

    // NoMDEntries group (GroupHeader = 3 bytes).
    if (remaining < sizeof(GroupHeader)) return DecodeResult::kBufferTooShort;
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    remaining -= sizeof(GroupHeader);

    if (gh.num_in_group > MAX_MD_ENTRIES) return DecodeResult::kGroupOverflow;
    out.num_md_entries = gh.num_in_group;

    for (uint8_t i = 0; i < gh.num_in_group; ++i) {
        if (remaining < gh.block_length) return DecodeResult::kBufferTooShort;
        std::memcpy(&out.md_entries[i], p, sizeof(TradeSummaryEntry));
        p += gh.block_length;
        remaining -= gh.block_length;
    }

    // NoOrderIDEntries group (GroupHeader8Byte = 8 bytes).
    if (remaining < sizeof(GroupHeader8Byte)) return DecodeResult::kBufferTooShort;
    GroupHeader8Byte gh8{};
    p = GroupHeader8Byte::decode_from(p, gh8);
    remaining -= sizeof(GroupHeader8Byte);

    if (gh8.num_in_group > MAX_ORDER_ENTRIES) return DecodeResult::kGroupOverflow;
    out.num_order_entries = gh8.num_in_group;

    for (uint8_t i = 0; i < gh8.num_in_group; ++i) {
        if (remaining < gh8.block_length) return DecodeResult::kBufferTooShort;
        std::memcpy(&out.order_entries[i], p, sizeof(TradeSummaryOrderEntry));
        p += gh8.block_length;
        remaining -= gh8.block_length;
    }

    return DecodeResult::kOk;
}

inline DecodeResult decode_snapshot_53(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedSnapshot53& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;

    std::memcpy(&out.root, buf, sizeof(out.root));
    const char* p = buf + hdr.block_length;
    size_t remaining = len - hdr.block_length;

    // NoMDEntries group (GroupHeader = 3 bytes).
    if (remaining < sizeof(GroupHeader)) return DecodeResult::kBufferTooShort;
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    remaining -= sizeof(GroupHeader);

    if (gh.num_in_group > MAX_SNAPSHOT_ENTRIES) return DecodeResult::kGroupOverflow;
    out.num_md_entries = gh.num_in_group;

    for (uint16_t i = 0; i < gh.num_in_group; ++i) {
        if (remaining < gh.block_length) return DecodeResult::kBufferTooShort;
        std::memcpy(&out.md_entries[i], p, sizeof(SnapshotOrderBookEntry));
        p += gh.block_length;
        remaining -= gh.block_length;
    }

    return DecodeResult::kOk;
}

inline DecodeResult decode_instrument_def_54(
    const char* buf, size_t len,
    const MessageHeader& hdr,
    DecodedInstrumentDef54& out)
{
    if (len < hdr.block_length) return DecodeResult::kBufferTooShort;

    std::memcpy(&out.root, buf, sizeof(out.root));
    const char* p = buf + hdr.block_length;
    size_t remaining = len - hdr.block_length;

    // Helper lambda to decode a standard 3-byte GroupHeader repeating group.
    auto decode_group = [&](auto* entries, uint8_t& count,
                            size_t entry_size, size_t max_entries) -> DecodeResult {
        if (remaining < sizeof(GroupHeader)) return DecodeResult::kBufferTooShort;
        GroupHeader gh{};
        p = GroupHeader::decode_from(p, gh);
        remaining -= sizeof(GroupHeader);

        if (gh.num_in_group > max_entries) return DecodeResult::kGroupOverflow;
        count = gh.num_in_group;

        for (uint8_t i = 0; i < gh.num_in_group; ++i) {
            if (remaining < gh.block_length) return DecodeResult::kBufferTooShort;
            std::memcpy(&entries[i], p, entry_size);
            p += gh.block_length;
            remaining -= gh.block_length;
        }
        return DecodeResult::kOk;
    };

    // NoEvents.
    auto rc = decode_group(out.events, out.num_events,
                           sizeof(InstrDefEventEntry), MAX_EVENT_ENTRIES);
    if (rc != DecodeResult::kOk) return rc;

    // NoMDFeedTypes.
    rc = decode_group(out.feed_types, out.num_feed_types,
                      sizeof(InstrDefFeedTypeEntry), MAX_FEED_TYPE_ENTRIES);
    if (rc != DecodeResult::kOk) return rc;

    // NoInstAttrib.
    rc = decode_group(out.attribs, out.num_attribs,
                      sizeof(InstrDefAttribEntry), MAX_ATTRIB_ENTRIES);
    if (rc != DecodeResult::kOk) return rc;

    // NoLotTypeRules.
    rc = decode_group(out.lot_types, out.num_lot_types,
                      sizeof(InstrDefLotTypeEntry), MAX_LOT_TYPE_ENTRIES);
    return rc;
}

// ---------------------------------------------------------------------------
// Top-level decode dispatch: decode MessageHeader, then dispatch by template_id.
//
// Validates schemaId == MDP3_SCHEMA_ID. Caller provides a visitor with
// overloads for each decoded message type. Returns DecodeResult.
// On kUnknownTemplateId or kBadSchemaId the visitor is not called.
// ---------------------------------------------------------------------------

template <typename Visitor>
DecodeResult decode_mdp3_message(
    const char* buf, size_t len,
    Visitor&& visitor)
{
    if (len < sizeof(MessageHeader)) return DecodeResult::kBufferTooShort;

    MessageHeader hdr{};
    const char* body = MessageHeader::decode_from(buf, hdr);
    size_t body_len = len - sizeof(MessageHeader);

    // Validate MDP3 schema identity.
    if (hdr.schema_id != MDP3_SCHEMA_ID) return DecodeResult::kBadSchemaId;

    switch (hdr.template_id) {
        case SECURITY_STATUS_30_ID: {
            DecodedSecurityStatus30 out{};
            auto rc = decode_security_status_30(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case MD_INCREMENTAL_REFRESH_BOOK_46_ID: {
            DecodedRefreshBook46 out{};
            auto rc = decode_refresh_book_46(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case MD_INCREMENTAL_REFRESH_TRADE_SUMMARY_48_ID: {
            DecodedTradeSummary48 out{};
            auto rc = decode_trade_summary_48(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case SNAPSHOT_FULL_REFRESH_ORDER_BOOK_53_ID: {
            DecodedSnapshot53 out{};
            auto rc = decode_snapshot_53(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        case MD_INSTRUMENT_DEFINITION_FUTURE_54_ID: {
            DecodedInstrumentDef54 out{};
            auto rc = decode_instrument_def_54(body, body_len, hdr, out);
            if (rc == DecodeResult::kOk) visitor(out);
            return rc;
        }
        default:
            return DecodeResult::kUnknownTemplateId;
    }
}

}  // namespace exchange::cme::sbe::mdp3
