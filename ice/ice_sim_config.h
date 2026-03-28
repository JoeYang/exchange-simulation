#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace exchange::ice {

// IceSimConfig -- command-line configuration for the ICE live simulator binary.
//
// ICE uses FIX 4.2 over TCP for order entry and iMpact (binary) over UDP
// multicast for market data — different from CME's SBE/iLink3 + MDP3 stack.
struct IceSimConfig {
    uint16_t    fix_port{9200};            // FIX 4.2 TCP listen port
    std::string impact_group{"239.0.32.1"};// iMpact multicast group
    uint16_t    impact_port{14400};        // iMpact multicast port
    uint16_t    snapshot_port{14401};      // TCP snapshot server port
    std::string shm_path;                  // empty = no shared memory transport
    std::vector<std::string> products;     // empty = load all ICE products

    // Parse from argv. Returns true on success.
    // Usage: ice-sim --fix-port 9200 --impact-group 239.0.32.1 --impact-port 14400
    //                [--shm-path /dev/shm/ice-sim] [--products B,G,I]
    static bool parse(int argc, char* argv[], IceSimConfig& out) {
        IceSimConfig cfg{};
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--fix-port") == 0 && i + 1 < argc) {
                cfg.fix_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--impact-group") == 0 && i + 1 < argc) {
                cfg.impact_group = argv[++i];
            } else if (std::strcmp(argv[i], "--impact-port") == 0 && i + 1 < argc) {
                cfg.impact_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--snapshot-port") == 0 && i + 1 < argc) {
                cfg.snapshot_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--shm-path") == 0 && i + 1 < argc) {
                cfg.shm_path = argv[++i];
            } else if (std::strcmp(argv[i], "--products") == 0 && i + 1 < argc) {
                cfg.products = split_csv(argv[++i]);
            } else if (std::strcmp(argv[i], "--help") == 0 ||
                       std::strcmp(argv[i], "-h") == 0) {
                return false;
            } else {
                return false;  // unknown argument
            }
        }
        out = std::move(cfg);
        return true;
    }

    static void print_usage(const char* prog) {
        std::fprintf(stderr,
            "Usage: %s [options]\n"
            "  --fix-port      PORT    FIX 4.2 TCP listen port    (default: 9200)\n"
            "  --impact-group  ADDR    iMpact multicast group     (default: 239.0.32.1)\n"
            "  --impact-port   PORT    iMpact multicast port      (default: 14400)\n"
            "  --snapshot-port PORT    TCP snapshot server port   (default: 14401)\n"
            "  --shm-path      PATH    Shared memory path         (default: none)\n"
            "  --products      SYM,..  Comma-separated symbols    (default: all)\n"
            "  -h, --help              Show this message\n",
            prog);
    }

private:
    static std::vector<std::string> split_csv(const char* s) {
        std::vector<std::string> result;
        std::string token;
        for (const char* p = s; *p; ++p) {
            if (*p == ',') {
                if (!token.empty()) result.push_back(std::move(token));
                token.clear();
            } else {
                token += *p;
            }
        }
        if (!token.empty()) result.push_back(std::move(token));
        return result;
    }
};

}  // namespace exchange::ice
