#pragma once

#include "ice/impact/impact_encoder.h"
#include "exchange-core/listeners.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace exchange::ice::impact {

// Encoded iMpact bundle with its wire bytes.
// Uses a fixed-size buffer to avoid per-message heap allocation.
struct ImpactPacket {
    static constexpr size_t kMaxSize = MAX_IMPACT_ENCODED_SIZE;

    std::array<char, kMaxSize> data{};
    size_t len{0};

    const char* bytes() const { return data.data(); }
};

// ImpactFeedPublisher implements MarketDataListenerBase.
// On each market data callback, encodes the event as an iMpact binary bundle
// (BundleStart + message + BundleEnd) and stores the packet for retrieval.
//
// Usage:
//   ImpactFeedPublisher pub(instrument_id);
//   // Wire as market data listener in engine template parameter...
//   for (const auto& pkt : pub.packets()) { /* send pkt.bytes() via UDP */ }
class ImpactFeedPublisher : public MarketDataListenerBase {
public:
    ImpactFeedPublisher() = default;

    explicit ImpactFeedPublisher(int32_t instrument_id) {
        ctx_.instrument_id = instrument_id;
    }

    // --- MarketDataListenerBase overrides (name-hiding, no vtable) ---

    void on_depth_update(const DepthUpdate& e) {
        ImpactPacket pkt{};
        pkt.len = encode_depth_update(pkt.data.data(), pkt.kMaxSize, e, ctx_);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    void on_trade(const Trade& e) {
        ImpactPacket pkt{};
        pkt.len = encode_trade(pkt.data.data(), pkt.kMaxSize, e, ctx_);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    void on_market_status(const exchange::MarketStatus& e) {
        ImpactPacket pkt{};
        pkt.len = encode_market_status(pkt.data.data(), pkt.kMaxSize, e, ctx_);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    // Passthrough — not encoded to iMpact incremental feed.
    void on_top_of_book(const TopOfBook&) {}
    void on_order_book_action(const OrderBookAction&) {}
    void on_indicative_price(const IndicativePrice&) {}

    // --- Accessors ---

    const std::vector<ImpactPacket>& packets() const { return packets_; }
    size_t packet_count() const { return packets_.size(); }
    void clear() { packets_.clear(); }

    const ImpactEncodeContext& context() const { return ctx_; }
    uint32_t seq_num() const { return ctx_.seq_num; }

private:
    ImpactEncodeContext ctx_{};
    std::vector<ImpactPacket> packets_;
};

}  // namespace exchange::ice::impact
