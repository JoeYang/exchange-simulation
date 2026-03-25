#pragma once

#include "cme/codec/ilink3_decoder.h"
#include "tools/pcap_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace exchange {

// Statistics collected during iLink3 replay.
struct Ilink3ReplayStats {
    uint32_t total_tcp_packets{0};
    uint32_t decoded_ok{0};
    uint32_t decode_errors{0};
    uint32_t non_tcp_skipped{0};
    // Per-message-type counters (client -> exchange).
    uint32_t new_order_514{0};
    uint32_t replace_515{0};
    uint32_t cancel_516{0};
    uint32_t mass_action_529{0};
    // Per-message-type counters (exchange -> client).
    uint32_t exec_new_522{0};
    uint32_t exec_reject_523{0};
    uint32_t exec_trade_525{0};
    uint32_t exec_cancel_534{0};
    uint32_t cancel_reject_535{0};
    uint32_t unknown_template{0};
};

// Replay iLink3 order flow from a pcap file through the decoder.
//
// Reads the pcap, filters for TCP packets on the specified destination port,
// decodes the SBE payload, invokes the visitor for each successfully decoded
// message, and returns aggregate statistics.
//
// iLink3 over TCP uses length-prefix framing (4-byte LE length + SBE payload).
// If strip_length_prefix is true (default), the first 4 bytes of each TCP
// payload are skipped. Set to false if the pcap already contains raw SBE.
//
// Template parameter Visitor must be callable with all iLink3 Decoded* types.
template <typename Visitor>
Ilink3ReplayStats replay_ilink3_pcap(
    const std::string& pcap_path,
    uint16_t dst_port,
    Visitor&& visitor,
    bool strip_length_prefix = true)
{
    using namespace cme::sbe::ilink3;

    PcapReader reader(pcap_path);
    Ilink3ReplayStats stats{};

    PcapPacket pkt;
    while (reader.next(pkt)) {
        if (pkt.proto != TransportProto::kTcp) {
            ++stats.non_tcp_skipped;
            continue;
        }
        if (dst_port != 0 && pkt.dst_port != dst_port) continue;
        if (pkt.payload_len == 0) continue;  // ACK-only, no data.

        ++stats.total_tcp_packets;

        const char* payload = reinterpret_cast<const char*>(pkt.payload);
        size_t payload_len = pkt.payload_len;

        // Optionally strip the 4-byte length prefix.
        if (strip_length_prefix) {
            if (payload_len < 4) { ++stats.decode_errors; continue; }
            payload += 4;
            payload_len -= 4;
        }

        auto rc = decode_ilink3_message(
            payload, payload_len,
            [&](auto& msg) {
                using T = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same_v<T, DecodedNewOrder514>)
                    ++stats.new_order_514;
                else if constexpr (std::is_same_v<T, DecodedReplaceRequest515>)
                    ++stats.replace_515;
                else if constexpr (std::is_same_v<T, DecodedCancelRequest516>)
                    ++stats.cancel_516;
                else if constexpr (std::is_same_v<T, DecodedMassAction529>)
                    ++stats.mass_action_529;
                else if constexpr (std::is_same_v<T, DecodedExecNew522>)
                    ++stats.exec_new_522;
                else if constexpr (std::is_same_v<T, DecodedExecReject523>)
                    ++stats.exec_reject_523;
                else if constexpr (std::is_same_v<T, DecodedExecTrade525>)
                    ++stats.exec_trade_525;
                else if constexpr (std::is_same_v<T, DecodedExecCancel534>)
                    ++stats.exec_cancel_534;
                else if constexpr (std::is_same_v<T, DecodedCancelReject535>)
                    ++stats.cancel_reject_535;

                visitor(msg);
            });

        if (rc == DecodeResult::kOk) {
            ++stats.decoded_ok;
        } else if (rc == DecodeResult::kUnknownTemplateId) {
            ++stats.unknown_template;
            ++stats.decode_errors;
        } else {
            ++stats.decode_errors;
        }
    }

    return stats;
}

// Convenience overload: replay without a visitor (stats only).
inline Ilink3ReplayStats replay_ilink3_pcap(
    const std::string& pcap_path,
    uint16_t dst_port = 0,
    bool strip_length_prefix = true)
{
    return replay_ilink3_pcap(pcap_path, dst_port,
                               [](auto&) {}, strip_length_prefix);
}

}  // namespace exchange
