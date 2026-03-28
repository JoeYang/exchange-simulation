#pragma once

#include "krx/fast/fast_encoder.h"
#include "exchange-core/listeners.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace exchange::krx::fast {

// ---------------------------------------------------------------------------
// Encoded FAST packet with its wire bytes.
// Fixed-size buffer to avoid per-message heap allocation on the hot path.
// ---------------------------------------------------------------------------

struct FastPacket {
    static constexpr size_t kMaxSize = kMaxFastEncodedSize;

    std::array<uint8_t, kMaxSize> data{};
    size_t len{0};

    const uint8_t* bytes() const { return data.data(); }
};

// ---------------------------------------------------------------------------
// FastFeedPublisher — MarketDataListenerBase implementation.
//
// Converts engine market data callbacks into FAST binary messages and
// stores the packets for retrieval. Caller is responsible for sending
// packets over UDP multicast.
//
// Handles:
//   on_top_of_book  -> FastQuote (template 1)
//   on_trade         -> FastTrade (template 2)
//   on_market_status -> FastStatus (template 3)
//
// Snapshot is published explicitly via publish_snapshot().
//
// Usage:
//   FastFeedPublisher pub;
//   // Wire as market data listener in engine template parameter...
//   for (const auto& pkt : pub.packets()) { /* send via UDP */ }
//   pub.clear();
// ---------------------------------------------------------------------------

class FastFeedPublisher : public MarketDataListenerBase {
public:
    FastFeedPublisher() = default;

    // --- MarketDataListenerBase overrides (name-hiding, no vtable) ---

    void on_top_of_book(const TopOfBook& e) {
        auto msg = to_fast_quote(e);
        FastPacket pkt{};
        pkt.len = encode_quote(pkt.data.data(), pkt.kMaxSize, msg);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    void on_trade(const Trade& e) {
        auto msg = to_fast_trade(e);
        FastPacket pkt{};
        pkt.len = encode_trade(pkt.data.data(), pkt.kMaxSize, msg);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    void on_market_status(const exchange::MarketStatus& e) {
        auto msg = to_fast_status(e);
        FastPacket pkt{};
        pkt.len = encode_status(pkt.data.data(), pkt.kMaxSize, msg);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    // Passthrough — not encoded to FAST feed.
    void on_depth_update(const DepthUpdate&) {}
    void on_order_book_action(const OrderBookAction&) {}
    void on_indicative_price(const IndicativePrice&) {}
    void on_lock_limit_triggered(const LockLimitTriggered&) {}

    // --- Explicit snapshot publish ---

    void publish_snapshot(const TopOfBook& e) {
        auto msg = to_fast_snapshot(e);
        FastPacket pkt{};
        pkt.len = encode_snapshot(pkt.data.data(), pkt.kMaxSize, msg);
        if (pkt.len > 0) packets_.push_back(pkt);
    }

    // --- Accessors ---

    const std::vector<FastPacket>& packets() const { return packets_; }
    size_t packet_count() const { return packets_.size(); }
    void clear() { packets_.clear(); }

private:
    std::vector<FastPacket> packets_;
};

}  // namespace exchange::krx::fast
