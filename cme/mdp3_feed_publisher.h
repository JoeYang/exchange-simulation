#pragma once

#include "cme/codec/mdp3_encoder.h"
#include "exchange-core/listeners.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace exchange {
namespace cme {

// Encoded MDP3 message with its wire bytes.
// Uses a fixed-size buffer to avoid per-message heap allocation.
struct Mdp3Packet {
    static constexpr size_t kMaxSize = sbe::mdp3::MAX_MDP3_ENCODED_SIZE;

    std::array<char, kMaxSize> data{};
    size_t len{0};

    const char* bytes() const { return data.data(); }
};

// Mdp3FeedPublisher implements MarketDataListenerBase.
// On each market data callback, encodes the event as MDP3 SBE bytes
// and stores the encoded packet for retrieval.
//
// Usage:
//   Mdp3FeedPublisher pub(security_id, "ES", "ES");
//   CmeSimulator<RecordingOrderListener, Mdp3FeedPublisher> sim(ol, pub);
//   // ... submit orders ...
//   for (const auto& pkt : pub.packets()) { /* send pkt.bytes() */ }
class Mdp3FeedPublisher : public MarketDataListenerBase {
public:
    Mdp3FeedPublisher() = default;

    explicit Mdp3FeedPublisher(int32_t security_id,
                               const char* security_group = "      ",
                               const char* asset = "      ") {
        ctx_.security_id = security_id;
        std::memcpy(ctx_.security_group, security_group, 6);
        std::memcpy(ctx_.asset, asset, 6);
    }

    // --- MarketDataListenerBase overrides (name-hiding, no vtable) ---

    void on_depth_update(const DepthUpdate& e) {
        Mdp3Packet pkt{};
        pkt.len = sbe::mdp3::encode_depth_update(pkt.data.data(), e, ctx_);
        packets_.push_back(pkt);
    }

    void on_top_of_book(const TopOfBook& e) {
        Mdp3Packet pkt{};
        pkt.len = sbe::mdp3::encode_top_of_book(pkt.data.data(), e, ctx_);
        packets_.push_back(pkt);
    }

    void on_trade(const Trade& e) {
        Mdp3Packet pkt{};
        pkt.len = sbe::mdp3::encode_trade(pkt.data.data(), e, ctx_);
        packets_.push_back(pkt);
    }

    void on_market_status(const MarketStatus& e) {
        Mdp3Packet pkt{};
        pkt.len = sbe::mdp3::encode_market_status(pkt.data.data(), e, ctx_);
        packets_.push_back(pkt);
    }

    // Passthrough — not encoded to MDP3.
    void on_order_book_action(const OrderBookAction&) {}
    void on_indicative_price(const IndicativePrice&) {}

    // --- Accessors ---

    const std::vector<Mdp3Packet>& packets() const { return packets_; }
    size_t packet_count() const { return packets_.size(); }
    void clear() { packets_.clear(); }

    const sbe::mdp3::Mdp3EncodeContext& context() const { return ctx_; }
    uint32_t rpt_seq() const { return ctx_.rpt_seq; }

private:
    sbe::mdp3::Mdp3EncodeContext ctx_{};
    std::vector<Mdp3Packet> packets_;
};

}  // namespace cme
}  // namespace exchange
