#pragma once

#include "ice/impact/impact_messages.h"

#include <cstddef>
#include <cstring>

namespace exchange::ice::impact {

// ---------------------------------------------------------------------------
// iMpact stream decoder — visitor-based dispatch by message type.
//
// Walks a buffer of concatenated iMpact messages, reads each header,
// dispatches to the visitor's typed callback. Unknown message types are
// skipped (forward-compatible).
//
// Visitor concept — implement the callbacks you need:
//   void on_bundle_start(const BundleStart&);
//   void on_bundle_end(const BundleEnd&);
//   void on_add_modify_order(const AddModifyOrder&);
//   void on_order_withdrawal(const OrderWithdrawal&);
//   void on_deal_trade(const DealTrade&);
//   void on_market_status(const MarketStatus&);
//   void on_snapshot_order(const SnapshotOrder&);
//   void on_price_level(const PriceLevel&);
//
// Returns total bytes consumed. Stops at the first message that cannot be
// fully read (truncated buffer). This is not an error — the caller can
// buffer the remainder and retry after more data arrives.
// ---------------------------------------------------------------------------

namespace detail {

// Decode a single typed message from buf and dispatch to visitor.
// Returns true if the message was decoded, false if the buffer is too small.
template <typename MsgT, typename VisitorT, typename CallbackFn>
bool try_decode_and_dispatch(
    const char* buf, size_t remaining, CallbackFn callback, VisitorT& visitor)
{
    MsgT msg{};
    if (decode(buf, remaining, msg) == nullptr) return false;
    (visitor.*callback)(msg);
    return true;
}

}  // namespace detail

template <typename VisitorT>
size_t decode_messages(const char* buf, size_t len, VisitorT& visitor) {
    if (buf == nullptr || len == 0) return 0;

    size_t offset = 0;

    while (offset + sizeof(ImpactMessageHeader) <= len) {
        ImpactMessageHeader hdr{};
        std::memcpy(&hdr, buf + offset, sizeof(hdr));

        // Validate body_length covers at least the header
        if (hdr.body_length < sizeof(ImpactMessageHeader)) break;
        // Check we have enough data for the full message
        if (offset + hdr.body_length > len) break;

        const char* msg_buf = buf + offset;
        size_t remaining = hdr.body_length;

        switch (static_cast<MessageType>(hdr.msg_type)) {
            case MessageType::BundleStart:
                detail::try_decode_and_dispatch<BundleStart>(
                    msg_buf, remaining,
                    &VisitorT::on_bundle_start, visitor);
                break;
            case MessageType::BundleEnd:
                detail::try_decode_and_dispatch<BundleEnd>(
                    msg_buf, remaining,
                    &VisitorT::on_bundle_end, visitor);
                break;
            case MessageType::AddModifyOrder:
                detail::try_decode_and_dispatch<AddModifyOrder>(
                    msg_buf, remaining,
                    &VisitorT::on_add_modify_order, visitor);
                break;
            case MessageType::OrderWithdrawal:
                detail::try_decode_and_dispatch<OrderWithdrawal>(
                    msg_buf, remaining,
                    &VisitorT::on_order_withdrawal, visitor);
                break;
            case MessageType::DealTrade:
                detail::try_decode_and_dispatch<DealTrade>(
                    msg_buf, remaining,
                    &VisitorT::on_deal_trade, visitor);
                break;
            case MessageType::MarketStatus:
                detail::try_decode_and_dispatch<MarketStatus>(
                    msg_buf, remaining,
                    &VisitorT::on_market_status, visitor);
                break;
            case MessageType::SnapshotOrder:
                detail::try_decode_and_dispatch<SnapshotOrder>(
                    msg_buf, remaining,
                    &VisitorT::on_snapshot_order, visitor);
                break;
            case MessageType::PriceLevel:
                detail::try_decode_and_dispatch<PriceLevel>(
                    msg_buf, remaining,
                    &VisitorT::on_price_level, visitor);
                break;
            default:
                // Unknown type — skip forward using body_length
                break;
        }

        offset += hdr.body_length;
    }

    return offset;
}

}  // namespace exchange::ice::impact
