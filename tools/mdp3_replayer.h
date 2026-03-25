#pragma once

#include "cme/codec/mdp3_decoder.h"
#include "tools/pcap_reader.h"
#include "tools/udp_multicast.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace exchange {

// Statistics collected during replay.
struct ReplayStats {
    uint32_t total_udp_packets{0};
    uint32_t decoded_ok{0};
    uint32_t decode_errors{0};
    uint32_t non_udp_skipped{0};
    uint32_t seq_gaps{0};         // Sequence number gaps detected.
    // Per-message-type counters.
    uint32_t security_status_30{0};
    uint32_t refresh_book_46{0};
    uint32_t trade_summary_48{0};
    uint32_t snapshot_53{0};
    uint32_t instrument_def_54{0};
    uint32_t unknown_template{0};
};

// Callback invoked for each successfully decoded MDP3 message.
// The visitor is a generic callable receiving any of the Decoded* types.
// This is the same pattern as decode_mdp3_message.

// Replay MDP3 packets from a pcap file through the decoder.
//
// Reads the pcap, filters for UDP packets on the specified destination port,
// strips the optional McastSeqHeader (if strip_seq_header is true), decodes
// the SBE payload, invokes the visitor for each successfully decoded message,
// and returns aggregate statistics.
//
// Template parameter Visitor must be callable with all Decoded* types
// (DecodedSecurityStatus30, DecodedRefreshBook46, DecodedTradeSummary48,
//  DecodedSnapshot53, DecodedInstrumentDef54).
template <typename Visitor>
ReplayStats replay_mdp3_pcap(
    const std::string& pcap_path,
    uint16_t dst_port,
    Visitor&& visitor,
    bool strip_seq_header = true)
{
    using namespace cme::sbe::mdp3;

    PcapReader reader(pcap_path);
    ReplayStats stats{};
    uint32_t expected_seq = 0;
    bool first_packet = true;

    PcapPacket pkt;
    while (reader.next(pkt)) {
        if (pkt.proto != TransportProto::kUdp) {
            ++stats.non_udp_skipped;
            continue;
        }
        if (dst_port != 0 && pkt.dst_port != dst_port) continue;

        ++stats.total_udp_packets;

        const uint8_t* payload = pkt.payload;
        size_t payload_len = pkt.payload_len;

        // Optionally strip the 4-byte McastSeqHeader.
        if (strip_seq_header && payload_len >= sizeof(McastSeqHeader)) {
            McastSeqHeader seq_hdr;
            std::memcpy(&seq_hdr, payload, sizeof(seq_hdr));

            if (first_packet) {
                expected_seq = seq_hdr.seq_num;
                first_packet = false;
            }
            if (seq_hdr.seq_num != expected_seq) {
                ++stats.seq_gaps;
            }
            expected_seq = seq_hdr.seq_num + 1;

            payload += sizeof(McastSeqHeader);
            payload_len -= sizeof(McastSeqHeader);
        }

        auto rc = decode_mdp3_message(
            reinterpret_cast<const char*>(payload), payload_len,
            [&](auto& msg) {
                using T = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same_v<T, DecodedSecurityStatus30>)
                    ++stats.security_status_30;
                else if constexpr (std::is_same_v<T, DecodedRefreshBook46>)
                    ++stats.refresh_book_46;
                else if constexpr (std::is_same_v<T, DecodedTradeSummary48>)
                    ++stats.trade_summary_48;
                else if constexpr (std::is_same_v<T, DecodedSnapshot53>)
                    ++stats.snapshot_53;
                else if constexpr (std::is_same_v<T, DecodedInstrumentDef54>)
                    ++stats.instrument_def_54;

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
inline ReplayStats replay_mdp3_pcap(
    const std::string& pcap_path,
    uint16_t dst_port = 0,
    bool strip_seq_header = true)
{
    return replay_mdp3_pcap(pcap_path, dst_port,
                            [](auto&) {}, strip_seq_header);
}

}  // namespace exchange
