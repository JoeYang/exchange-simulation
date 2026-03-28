#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace exchange::krx {

// KrxSimConfig -- command-line configuration for the KRX live simulator binary.
//
// KRX uses FIX 4.4 over TCP for order entry and FAST 1.1 over UDP
// multicast for market data. Supports dual sessions (regular + after-hours).
struct KrxSimConfig {
    uint16_t    fix_port{9300};              // FIX 4.4 TCP listen port
    std::string fast_group{"224.0.33.1"};    // FAST multicast group
    uint16_t    fast_port{16000};            // FAST multicast port
    std::string secdef_group{"224.0.33.2"};  // secdef multicast group
    uint16_t    secdef_port{16001};          // secdef multicast port
    std::string snapshot_group{"224.0.33.3"};// snapshot multicast group
    uint16_t    snapshot_port{16002};        // snapshot multicast port
    std::string shm_path;                    // empty = no shared memory transport
    std::vector<std::string> products;       // empty = load all KRX products
    std::string session_mode{"regular"};     // "regular", "after-hours", or "both"

    // Parse from argv. Returns true on success.
    // Usage: krx-sim --fix-port 9300 --fast-group 224.0.33.1 --fast-port 16000
    //                [--shm-path /dev/shm/krx-sim] [--products KOSPI200,KTB3Y]
    //                [--session-mode both]
    static bool parse(int argc, char* argv[], KrxSimConfig& out) {
        KrxSimConfig cfg{};
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--fix-port") == 0 && i + 1 < argc) {
                cfg.fix_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--fast-group") == 0 && i + 1 < argc) {
                cfg.fast_group = argv[++i];
            } else if (std::strcmp(argv[i], "--fast-port") == 0 && i + 1 < argc) {
                cfg.fast_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--secdef-group") == 0 && i + 1 < argc) {
                cfg.secdef_group = argv[++i];
            } else if (std::strcmp(argv[i], "--secdef-port") == 0 && i + 1 < argc) {
                cfg.secdef_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--snapshot-group") == 0 && i + 1 < argc) {
                cfg.snapshot_group = argv[++i];
            } else if (std::strcmp(argv[i], "--snapshot-port") == 0 && i + 1 < argc) {
                cfg.snapshot_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--shm-path") == 0 && i + 1 < argc) {
                cfg.shm_path = argv[++i];
            } else if (std::strcmp(argv[i], "--products") == 0 && i + 1 < argc) {
                cfg.products = split_csv(argv[++i]);
            } else if (std::strcmp(argv[i], "--session-mode") == 0 && i + 1 < argc) {
                cfg.session_mode = argv[++i];
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
            "  --fix-port       PORT    FIX 4.4 TCP listen port    (default: 9300)\n"
            "  --fast-group     ADDR    FAST multicast group       (default: 224.0.33.1)\n"
            "  --fast-port      PORT    FAST multicast port        (default: 16000)\n"
            "  --secdef-group   ADDR    Secdef multicast group     (default: 224.0.33.2)\n"
            "  --secdef-port    PORT    Secdef multicast port      (default: 16001)\n"
            "  --snapshot-group ADDR    Snapshot multicast group   (default: 224.0.33.3)\n"
            "  --snapshot-port  PORT    Snapshot multicast port    (default: 16002)\n"
            "  --shm-path       PATH    Shared memory path         (default: none)\n"
            "  --products       SYM,..  Comma-separated symbols    (default: all)\n"
            "  --session-mode   MODE    regular|after-hours|both   (default: regular)\n"
            "  -h, --help               Show this message\n",
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

}  // namespace exchange::krx
