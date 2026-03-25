#include "tools/mdp3_replayer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace exchange;
using namespace exchange::cme::sbe::mdp3;

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pcap <file> [--port <port>] [--no-seq-header]\n"
        "\n"
        "Replay MDP3 pcap captures through the SBE decoder.\n"
        "\n"
        "Options:\n"
        "  --pcap <file>      Path to pcap/pcapng capture file\n"
        "  --port <port>      Filter by UDP destination port (default: all)\n"
        "  --no-seq-header    Payload has no McastSeqHeader prefix\n",
        prog);
}

// Print a decoded message in human-readable format.
struct PrintVisitor {
    uint64_t ts_ns;

    void operator()(const DecodedSecurityStatus30& msg) {
        std::printf("[%lu] SecurityStatus30  secId=%d  status=%u  halt=%u\n",
                    ts_ns, msg.root.security_id,
                    msg.root.security_trading_status,
                    msg.root.halt_reason);
    }

    void operator()(const DecodedRefreshBook46& msg) {
        std::printf("[%lu] RefreshBook46  entries=%u  orders=%u  ts=%lu\n",
                    ts_ns, msg.num_md_entries, msg.num_order_entries,
                    msg.root.transact_time);
        for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
            const auto& e = msg.md_entries[i];
            std::printf("  [%u] secId=%d  px=%.4f  qty=%d  side=%c  action=%u  lvl=%u\n",
                        i, e.security_id, e.md_entry_px.to_double(),
                        e.md_entry_size, e.md_entry_type,
                        e.md_update_action, e.md_price_level);
        }
    }

    void operator()(const DecodedTradeSummary48& msg) {
        std::printf("[%lu] TradeSummary48  entries=%u  ts=%lu\n",
                    ts_ns, msg.num_md_entries, msg.root.transact_time);
        for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
            const auto& e = msg.md_entries[i];
            std::printf("  [%u] secId=%d  px=%.4f  qty=%d  aggr=%u\n",
                        i, e.security_id, e.md_entry_px.to_double(),
                        e.md_entry_size, e.aggressor_side);
        }
    }

    void operator()(const DecodedSnapshot53& msg) {
        std::printf("[%lu] Snapshot53  secId=%d  entries=%u  chunk=%u/%u\n",
                    ts_ns, msg.root.security_id, msg.num_md_entries,
                    msg.root.current_chunk, msg.root.no_chunks);
    }

    void operator()(const DecodedInstrumentDef54& msg) {
        std::printf("[%lu] InstrumentDef54  secId=%d  symbol=%.20s  events=%u\n",
                    ts_ns, msg.root.security_id, msg.root.symbol,
                    msg.num_events);
    }
};

int main(int argc, char* argv[]) {
    std::string pcap_path;
    uint16_t port = 0;
    bool strip_seq = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pcap") == 0 && i + 1 < argc) {
            pcap_path = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-seq-header") == 0) {
            strip_seq = false;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (pcap_path.empty()) {
        usage(argv[0]);
        return 1;
    }

    PrintVisitor printer{};
    auto stats = replay_mdp3_pcap(pcap_path, port,
        [&](auto& msg) { printer(msg); }, strip_seq);

    std::printf("\n--- Replay Summary ---\n");
    std::printf("UDP packets:      %u\n", stats.total_udp_packets);
    std::printf("Decoded OK:       %u\n", stats.decoded_ok);
    std::printf("Decode errors:    %u\n", stats.decode_errors);
    std::printf("Sequence gaps:    %u\n", stats.seq_gaps);
    std::printf("RefreshBook46:    %u\n", stats.refresh_book_46);
    std::printf("TradeSummary48:   %u\n", stats.trade_summary_48);
    std::printf("SecurityStatus30: %u\n", stats.security_status_30);
    std::printf("Snapshot53:       %u\n", stats.snapshot_53);
    std::printf("InstrumentDef54:  %u\n", stats.instrument_def_54);
    std::printf("Unknown template: %u\n", stats.unknown_template);

    return stats.decode_errors > 0 ? 1 : 0;
}
