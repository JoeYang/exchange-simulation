#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace exchange::cme {

// SimConfig -- command-line configuration for the CME live simulator binary.
struct SimConfig {
    uint16_t    ilink3_port{9100};
    std::string mdp3_group{"239.0.31.1"};
    uint16_t    mdp3_port{14310};
    std::string secdef_group{"239.0.31.3"};    // secdef multicast group
    uint16_t    secdef_port{14312};            // secdef multicast port
    std::string snapshot_group{"239.0.31.2"};  // snapshot multicast group
    uint16_t    snapshot_port{14311};          // snapshot multicast port
    std::string shm_path;                      // empty = no shared memory transport
    std::vector<std::string> products;         // empty = load all CME products

    // Parse from argv.  Returns true on success.
    // Usage: cme-sim --ilink3-port 9100 --mdp3-group 239.0.31.1 --mdp3-port 14310
    //                [--shm-path /dev/shm/cme-sim] [--products ES,NQ,CL]
    static bool parse(int argc, char* argv[], SimConfig& out) {
        SimConfig cfg{};
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--ilink3-port") == 0 && i + 1 < argc) {
                cfg.ilink3_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (std::strcmp(argv[i], "--mdp3-group") == 0 && i + 1 < argc) {
                cfg.mdp3_group = argv[++i];
            } else if (std::strcmp(argv[i], "--mdp3-port") == 0 && i + 1 < argc) {
                cfg.mdp3_port = static_cast<uint16_t>(std::stoi(argv[++i]));
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
            "  --ilink3-port PORT    iLink3 TCP listen port   (default: 9100)\n"
            "  --mdp3-group  ADDR    MDP3 multicast group     (default: 239.0.31.1)\n"
            "  --mdp3-port   PORT    MDP3 multicast port      (default: 14310)\n"
            "  --secdef-group ADDR   Secdef multicast group   (default: 239.0.31.3)\n"
            "  --secdef-port PORT    Secdef multicast port    (default: 14312)\n"
            "  --snapshot-group ADDR Snapshot multicast group (default: 239.0.31.2)\n"
            "  --snapshot-port PORT  Snapshot multicast port  (default: 14311)\n"
            "  --shm-path    PATH    Shared memory path       (default: none)\n"
            "  --products    SYM,..  Comma-separated symbols  (default: all)\n"
            "  -h, --help            Show this message\n",
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

}  // namespace exchange::cme
