// exchange-trader -- Autonomous trading client for cme-sim or ice-sim.
//
// Connects via TCP, runs a trading strategy (random-walk or market-maker),
// writes a journal file of all actions and execution reports.
//
// Usage:
//   exchange-trader --exchange cme --host 127.0.0.1 --port 9100
//     --instrument ES --strategy random-walk --ref-price 5000.00
//     --spread 2.50 --rate 5 --max-position 10 --journal /tmp/client_1.journal

#include "tools/sim_client.h"
#include "tools/trading_strategy.h"
#include "cme/cme_products.h"
#include "ice/ice_products.h"
#include "exchange-core/types.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <memory>
#include <random>
#include <string>

namespace {

using namespace exchange;

// ---------------------------------------------------------------------------
// Global signal handler state
// ---------------------------------------------------------------------------

volatile sig_atomic_t g_running = 1;

void sigint_handler(int /*sig*/) { g_running = 0; }

// ---------------------------------------------------------------------------
// CLI arguments
// ---------------------------------------------------------------------------

struct Args {
    std::string exchange{"cme"};
    std::string host{"127.0.0.1"};
    uint16_t port{9100};
    std::string instrument{"ES"};
    std::string strategy{"random-walk"};
    std::string account{"FIRM_A"};
    double ref_price{5000.0};
    double spread{2.50};
    int rate{5};            // max actions per second
    int max_position{10};   // max contracts
    std::string journal;
    uint32_t seed{0};       // 0 = random seed
};

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "  --exchange STR       cme or ice            (default: cme)\n"
        "  --host HOST          TCP host              (default: 127.0.0.1)\n"
        "  --port PORT          TCP port              (default: 9100)\n"
        "  --instrument SYM     Instrument symbol     (default: ES)\n"
        "  --strategy STR       random-walk or market-maker (default: random-walk)\n"
        "  --account STR        Account identifier    (default: FIRM_A)\n"
        "  --ref-price PRICE    Reference price       (default: 5000.00)\n"
        "  --spread SPREAD      Spread in decimal     (default: 2.50)\n"
        "  --rate N             Max actions/sec       (default: 5)\n"
        "  --max-position N     Max position contracts (default: 10)\n"
        "  --journal PATH       Journal file path     (default: none)\n"
        "  --seed N             RNG seed (0=random)   (default: 0)\n"
        "  -h, --help           Show this message\n",
        prog);
}

bool parse_args(int argc, char* argv[], Args& out) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--exchange") == 0 && i + 1 < argc) {
            args.exchange = argv[++i];
        } else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            args.host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            args.instrument = argv[++i];
        } else if (std::strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            args.strategy = argv[++i];
        } else if (std::strcmp(argv[i], "--account") == 0 && i + 1 < argc) {
            args.account = argv[++i];
        } else if (std::strcmp(argv[i], "--ref-price") == 0 && i + 1 < argc) {
            args.ref_price = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--spread") == 0 && i + 1 < argc) {
            args.spread = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            args.rate = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--max-position") == 0 && i + 1 < argc) {
            args.max_position = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            args.journal = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            args.seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "-h") == 0 ||
                   std::strcmp(argv[i], "--help") == 0) {
            return false;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }
    out = std::move(args);
    return true;
}

// ---------------------------------------------------------------------------
// Instrument resolution
// ---------------------------------------------------------------------------

struct InstrumentInfo {
    int32_t security_id = -1;
    Price tick_size = 0;
    Quantity lot_size = 0;
};

InstrumentInfo resolve_cme(const std::string& symbol) {
    for (const auto& p : exchange::cme::get_cme_products()) {
        if (p.symbol == symbol) {
            InstrumentInfo info;
            info.security_id = static_cast<int32_t>(p.instrument_id);
            info.tick_size = p.tick_size;
            info.lot_size = p.lot_size;
            return info;
        }
    }
    return {};
}

InstrumentInfo resolve_ice(const std::string& symbol) {
    for (const auto& p : exchange::ice::get_ice_products()) {
        if (p.symbol == symbol) {
            InstrumentInfo info;
            info.security_id = static_cast<int32_t>(p.instrument_id);
            info.tick_size = p.tick_size;
            info.lot_size = p.lot_size;
            return info;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Timestamp helpers
// ---------------------------------------------------------------------------

Timestamp now_ns_local() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        print_usage(argv[0]);
        return 1;
    }

    // Resolve instrument.
    InstrumentInfo info;
    if (args.exchange == "cme") {
        info = resolve_cme(args.instrument);
    } else if (args.exchange == "ice") {
        info = resolve_ice(args.instrument);
    } else {
        std::fprintf(stderr, "Unknown exchange: %s (use cme or ice)\n",
                     args.exchange.c_str());
        return 1;
    }

    if (info.security_id < 0) {
        std::fprintf(stderr, "Unknown instrument: %s for exchange %s\n",
                     args.instrument.c_str(), args.exchange.c_str());
        return 1;
    }

    // Create protocol codec.
    std::unique_ptr<exchange::ProtocolCodec> codec;
    if (args.exchange == "cme") {
        codec = std::make_unique<exchange::CmeCodec>(info.security_id, args.account);
    } else {
        codec = std::make_unique<exchange::IceCodec>(
            args.instrument, args.account, "EXCHANGE");
    }

    // Create SimClient.
    exchange::SimClient client(std::move(codec), args.instrument);

    // Open journal if specified.
    if (!args.journal.empty()) {
        if (!client.open_journal(args.journal.c_str())) {
            std::fprintf(stderr, "Failed to open journal: %s\n",
                         args.journal.c_str());
            return 1;
        }
    }

    // Connect to exchange.
    std::fprintf(stderr, "Connecting to %s:%u (%s %s)...\n",
                 args.host.c_str(), args.port,
                 args.exchange.c_str(), args.instrument.c_str());
    if (!client.connect(args.host.c_str(), args.port)) {
        std::fprintf(stderr, "Connection failed\n");
        return 1;
    }
    std::fprintf(stderr, "Connected.\n");

    // Select strategy.
    exchange::StrategyTick strategy;
    if (args.strategy == "market-maker") {
        strategy = exchange::market_maker_strategy();
    } else {
        strategy = exchange::random_walk_strategy();
    }

    // Initialize client state.
    exchange::Price ref_price = static_cast<exchange::Price>(
        std::round(args.ref_price * PRICE_SCALE));
    exchange::Price spread = static_cast<exchange::Price>(
        std::round(args.spread * PRICE_SCALE));

    auto& state = client.state();
    state.ref_price = ref_price;
    state.spread = spread;
    state.max_position = static_cast<exchange::Quantity>(args.max_position) * PRICE_SCALE;
    state.lot_size = info.lot_size;
    state.tick_size = info.tick_size;
    state.next_cl_ord_id = 1;

    // RNG setup.
    uint32_t seed = args.seed;
    if (seed == 0) {
        seed = static_cast<uint32_t>(std::random_device{}());
    }
    std::mt19937 rng(seed);
    std::fprintf(stderr, "Strategy: %s, seed: %u\n",
                 args.strategy.c_str(), seed);

    // Install SIGINT handler.
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    // --- Main loop ---
    int actions_this_second = 0;
    Timestamp second_start = now_ns_local();
    Timestamp last_status = second_start;

    while (g_running) {
        // 1. Poll for responses (non-blocking).
        client.poll_responses();

        // Check if disconnected.
        if (!client.is_connected()) {
            std::fprintf(stderr, "Disconnected from exchange\n");
            break;
        }

        Timestamp now = now_ns_local();

        // 2. Rate limiter: reset counter each second.
        if (now - second_start >= 1'000'000'000LL) {
            actions_this_second = 0;
            second_start = now;
        }

        // 3. Strategy tick (if rate allows).
        if (actions_this_second < args.rate) {
            client.sync_state();
            auto actions = strategy(state, rng);

            for (const auto& action : actions) {
                if (actions_this_second >= args.rate) break;

                switch (action.type) {
                    case exchange::OrderAction::New:
                        if (client.send_new_order(
                                action.cl_ord_id, action.side,
                                action.price, action.qty)) {
                            ++actions_this_second;
                        }
                        break;
                    case exchange::OrderAction::Cancel:
                        if (client.send_cancel(action.cl_ord_id)) {
                            ++actions_this_second;
                        }
                        break;
                    case exchange::OrderAction::Modify:
                        if (client.send_replace(
                                action.orig_cl_ord_id, action.cl_ord_id,
                                action.price, action.qty)) {
                            ++actions_this_second;
                        }
                        break;
                }
            }
        }

        // 4. Print status every second.
        if (now - last_status >= 1'000'000'000LL) {
            std::fprintf(stderr, "\r%s", client.status_line().c_str());
            last_status = now;
        }

        // Brief sleep to avoid busy-spinning (1ms).
        struct timespec sleep_ts{0, 1'000'000};
        nanosleep(&sleep_ts, nullptr);
    }

    // --- Graceful shutdown ---
    std::fprintf(stderr, "\nShutting down: cancelling %zu open orders...\n",
                 client.open_orders().size());
    client.cancel_all_open();

    // Drain remaining responses for up to 2 seconds.
    Timestamp shutdown_start = now_ns_local();
    while (now_ns_local() - shutdown_start < 2'000'000'000LL) {
        if (client.poll_responses() == 0) {
            struct timespec sleep_ts{0, 10'000'000};  // 10ms
            nanosleep(&sleep_ts, nullptr);
        }
        if (client.open_orders().empty()) break;
    }

    // Print summary.
    std::fprintf(stderr,
        "\n=== Summary ===\n"
        "Instrument:   %s\n"
        "Strategy:     %s\n"
        "Position:     %ld contracts\n"
        "Realized P&L: %ld\n"
        "Fills:        %u\n"
        "Open orders:  %zu\n"
        "Decode errs:  %u\n",
        args.instrument.c_str(),
        args.strategy.c_str(),
        static_cast<long>(client.position() / PRICE_SCALE),
        static_cast<long>(client.realized_pnl()),
        client.fill_count(),
        client.open_orders().size(),
        client.decode_errors());

    return 0;
}
