// krx_sim_runner.cc -- KRX live simulator binary.
//
// Wires together:
//   - KrxSimulator with KrxFixExecPublisher + FastFeedPublisher
//   - TCP server for FIX 4.4 order entry
//   - UDP multicast publisher for FAST 1.1 market data
//   - KrxFixGateway to decode incoming FIX and route to the simulator
//   - (optional) SHM transport for dashboard visualization
//
// Event loop:
//   1. Poll TCP for incoming FIX messages
//   2. Gateway decodes FIX 4.4, routes to matching engine via simulator
//   3. Engine fires callbacks on exec publisher and FAST feed publisher
//   4. Pending execution reports sent back to client over TCP
//   5. Pending FAST packets multicast over UDP

#include "krx/krx_products.h"
#include "krx/krx_simulator.h"
#include "krx/krx_sim_config.h"
#include "krx/fix/krx_fix_gateway.h"
#include "krx/fix/krx_fix_exec_publisher.h"
#include "krx/fast/fast_encoder.h"
#include "krx/fast/fast_publisher.h"
#include "exchange-core/composite_listener.h"
#include "tools/shm_listener.h"
#include "tools/shm_transport.h"
#include "tools/tcp_server.h"
#include "tools/udp_multicast.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unordered_set>
#include <vector>

namespace exchange::krx {

// Publish FAST-encoded instrument definitions on the secdef multicast channel.
inline void publish_secdefs(
    UdpMulticastPublisher& secdef_mcast,
    const std::vector<KrxProductConfig>& products,
    Timestamp ts)
{
    using namespace fast;

    auto total = static_cast<uint32_t>(products.size());
    for (const auto& p : products) {
        FastInstrumentDef def{};
        def.instrument_id = p.instrument_id;
        std::memset(def.symbol, 0, sizeof(def.symbol));
        auto sym_len = std::min(p.symbol.size(), sizeof(def.symbol));
        std::memcpy(def.symbol, p.symbol.c_str(), sym_len);
        std::memset(def.description, 0, sizeof(def.description));
        auto desc_len = std::min(p.description.size(), sizeof(def.description));
        std::memcpy(def.description, p.description.c_str(), desc_len);
        def.product_group = static_cast<uint8_t>(p.product_group);
        def.tick_size = p.tick_size;
        def.lot_size = p.lot_size;
        def.max_order_size = p.max_order_size;
        def.total_instruments = total;
        def.timestamp = ts;

        uint8_t buf[kMaxFastEncodedSize]{};
        size_t n = encode_instrument_def(buf, sizeof(buf), def);
        if (n > 0) {
            secdef_mcast.send(reinterpret_cast<const char*>(buf), n);
        }
    }
}

}  // namespace exchange::krx

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// Filter products by the symbols listed in config. Empty list = keep all.
std::vector<exchange::krx::KrxProductConfig> filter_products(
    const std::vector<exchange::krx::KrxProductConfig>& all,
    const std::vector<std::string>& symbols)
{
    if (symbols.empty()) return all;

    std::unordered_set<std::string> wanted(symbols.begin(), symbols.end());
    std::vector<exchange::krx::KrxProductConfig> result;
    for (const auto& p : all) {
        if (wanted.count(p.symbol)) {
            result.push_back(p);
        }
    }
    return result;
}

auto make_clock() {
    return []() -> exchange::Timestamp {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<exchange::Timestamp>(ts.tv_sec) * 1'000'000'000LL
               + ts.tv_nsec;
    };
}

// run_sim -- shared event loop for both SHM and non-SHM modes.
template <typename SimulatorT>
int run_sim(SimulatorT& sim,
            exchange::krx::fix::KrxFixExecPublisher& exec_pub,
            exchange::krx::fast::FastFeedPublisher& fast_pub,
            const exchange::krx::KrxSimConfig& cfg,
            const std::vector<exchange::krx::KrxProductConfig>& products)
{
    using namespace exchange;
    using namespace exchange::krx;

    auto now = make_clock();

    // -- Set reference prices (use 0 for now; real system gets from feed) --
    // In a production system these would come from the previous close feed.
    // For the simulator, we start without daily limits until set externally.

    // -- Open regular session --
    sim.start_regular_session(now());
    sim.open_regular_market(now());
    std::fprintf(stderr, "Market open. Session: Regular Continuous\n");

    // -- Create FIX gateway (routes to first instrument by default) --
    // The gateway is templated on the simulator which exposes new_order etc.
    // For multi-instrument routing, the gateway extracts board_id from FIX.
    // For simplicity, the runner routes all orders to the first product.
    auto first_id = products[0].instrument_id;

    // Wrapper that routes FIX gateway calls to a specific instrument.
    struct SimRouter {
        SimulatorT& sim;
        uint32_t instrument_id;

        void new_order(const OrderRequest& req) {
            sim.new_order(instrument_id, req);
        }
        void cancel_order(OrderId id, Timestamp ts) {
            sim.cancel_order(instrument_id, id, ts);
        }
        void modify_order(const ModifyRequest& req) {
            sim.modify_order(instrument_id, req);
        }
    };

    SimRouter router{sim, first_id};
    fix::KrxFixGateway<SimRouter> gateway(router);

    // -- Start UDP multicast publisher for FAST --
    UdpMulticastPublisher fast_mcast(cfg.fast_group.c_str(), cfg.fast_port);
    std::fprintf(stderr, "FAST multicast: %s:%u\n",
                 cfg.fast_group.c_str(), cfg.fast_port);

    // -- Start UDP multicast publisher for secdef channel --
    UdpMulticastPublisher secdef_mcast(cfg.secdef_group.c_str(), cfg.secdef_port);
    std::fprintf(stderr, "Secdef multicast: %s:%u\n",
                 cfg.secdef_group.c_str(), cfg.secdef_port);

    // Publish initial secdef definitions at startup.
    publish_secdefs(secdef_mcast, products, now());
    std::fprintf(stderr, "Published %zu secdef(s)\n", products.size());

    // -- Start UDP multicast publisher for snapshot channel --
    UdpMulticastPublisher snapshot_mcast(cfg.snapshot_group.c_str(), cfg.snapshot_port);
    std::fprintf(stderr, "Snapshot multicast: %s:%u\n",
                 cfg.snapshot_group.c_str(), cfg.snapshot_port);

    // -- Track connected TCP clients --
    std::unordered_set<int> connected_fds;

    // -- Start TCP server for FIX --
    TcpServer::Config tcp_cfg{};
    tcp_cfg.port = cfg.fix_port;

    tcp_cfg.on_connect = [&](int fd) {
        connected_fds.insert(fd);
        std::fprintf(stderr, "FIX client connected: fd=%d (total=%zu)\n",
                     fd, connected_fds.size());
    };

    tcp_cfg.on_disconnect = [&](int fd) {
        connected_fds.erase(fd);
        std::fprintf(stderr, "FIX client disconnected: fd=%d (total=%zu)\n",
                     fd, connected_fds.size());
    };

    tcp_cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        auto ts = now();
        auto result = gateway.on_message(data, len, ts);
        if (!result.ok) {
            std::fprintf(stderr, "FIX gateway error: %s (fd=%d)\n",
                         result.error.c_str(), fd);
        }
    };

    TcpServer tcp(tcp_cfg);
    std::fprintf(stderr, "FIX gateway listening on port %u\n", tcp.port());
    std::fprintf(stderr, "Simulator ready. Press Ctrl+C to stop.\n");

    // -- Secdef re-publish timer (30s cycle like CME) --
    auto last_secdef = std::chrono::steady_clock::now();
    constexpr auto secdef_interval = std::chrono::seconds(30);

    // -- Snapshot publish timer --
    auto last_snapshot = std::chrono::steady_clock::now();
    constexpr auto snapshot_interval = std::chrono::seconds(5);
    uint32_t snapshot_seq = 0;

    // -- Event loop --
    while (g_running.load(std::memory_order_relaxed)) {
        // 1. Poll TCP for incoming FIX messages (non-blocking).
        tcp.poll(/* timeout_ms= */ 10);

        // 2. Flush pending execution reports to connected clients.
        if (!exec_pub.messages().empty()) {
            for (const auto& msg : exec_pub.messages()) {
                for (int fd : connected_fds) {
                    tcp.send_message(fd, msg.data(), msg.size());
                }
            }
            exec_pub.clear_messages();
        }

        // 3. Flush pending FAST packets to multicast.
        if (fast_pub.packet_count() > 0) {
            for (const auto& pkt : fast_pub.packets()) {
                auto rc = fast_mcast.send(
                    reinterpret_cast<const char*>(pkt.bytes()), pkt.len);
                if (rc == SendResult::kError) {
                    std::fprintf(stderr, "FAST multicast send error\n");
                }
            }
            fast_pub.clear();
        }

        // 4. Re-publish secdef periodically for late joiners.
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - last_secdef >= secdef_interval) {
            publish_secdefs(secdef_mcast, products, now());
            last_secdef = now_tp;
        }

        // 5. Publish full-depth snapshots on snapshot channel for recovery.
        if (now_tp - last_snapshot >= snapshot_interval) {
            using namespace exchange::krx::fast;
            auto ts = now();

            for (const auto& product : products) {
                auto* engine = sim.get_regular_engine(product.instrument_id);
                if (!engine) continue;

                FastFullSnapshot snap{};
                snap.instrument_id = product.instrument_id;
                snap.seq_num = ++snapshot_seq;
                snap.timestamp = ts;

                // Collect bid levels (up to kSnapshotBookDepth).
                uint8_t bid_idx = 0;
                engine->for_each_level(Side::Buy,
                    [&](Price p, Quantity q, uint32_t oc) {
                        if (bid_idx < kSnapshotBookDepth) {
                            snap.bids[bid_idx] = {p, q, oc};
                            ++bid_idx;
                        }
                    });
                snap.num_bid_levels = bid_idx;

                // Collect ask levels (up to kSnapshotBookDepth).
                uint8_t ask_idx = 0;
                engine->for_each_level(Side::Sell,
                    [&](Price p, Quantity q, uint32_t oc) {
                        if (ask_idx < kSnapshotBookDepth) {
                            snap.asks[ask_idx] = {p, q, oc};
                            ++ask_idx;
                        }
                    });
                snap.num_ask_levels = ask_idx;

                uint8_t buf[kMaxFastEncodedSize]{};
                size_t n = encode_full_snapshot(buf, sizeof(buf), snap);
                if (n > 0) {
                    snapshot_mcast.send(
                        reinterpret_cast<const char*>(buf), n);
                }
            }
            last_snapshot = now_tp;
        }
    }

    // -- Shutdown --
    std::fprintf(stderr, "\nShutting down...\n");
    sim.pre_close_regular(now());
    sim.close_regular_session(now());
    sim.end_of_day(now());
    tcp.shutdown();
    std::fprintf(stderr, "Simulator stopped.\n");

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace exchange;
    using namespace exchange::krx;

    // -- Parse config --
    KrxSimConfig cfg;
    if (!KrxSimConfig::parse(argc, argv, cfg)) {
        KrxSimConfig::print_usage(argv[0]);
        return 1;
    }

    // -- Install signal handlers --
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -- Load products --
    auto all_products = get_krx_products();
    auto products = filter_products(all_products, cfg.products);
    if (products.empty()) {
        std::fprintf(stderr, "Error: no matching products found.\n");
        return 1;
    }

    std::fprintf(stderr, "Loading %zu product(s):", products.size());
    for (const auto& p : products) {
        std::fprintf(stderr, " %s", p.symbol.c_str());
    }
    std::fprintf(stderr, "\n");

    // -- Create publishers --
    fix::KrxFixExecPublisher exec_pub("KRX-SIM", "CLIENT",
                                       products[0].symbol);
    fast::FastFeedPublisher fast_pub;

    if (!cfg.shm_path.empty()) {
        // SHM mode: composite listeners fan out to publishers + SHM.
        std::fprintf(stderr, "SHM transport: %s\n", cfg.shm_path.c_str());

        ShmProducer shm_producer(cfg.shm_path);
        SharedMemoryOrderListener shm_ol(shm_producer);
        SharedMemoryMdListener shm_ml(shm_producer);

        using CompositeOL = CompositeOrderListener<
            fix::KrxFixExecPublisher, SharedMemoryOrderListener>;
        using CompositeML = CompositeMdListener<
            fast::FastFeedPublisher, SharedMemoryMdListener>;

        CompositeOL composite_ol(&exec_pub, &shm_ol);
        CompositeML composite_ml(&fast_pub, &shm_ml);

        KrxSimulator<CompositeOL, CompositeML> sim(composite_ol, composite_ml);
        sim.load_products(products);

        return run_sim(sim, exec_pub, fast_pub, cfg, products);
    }

    // Standard mode: direct listeners, no SHM.
    KrxSimulator<fix::KrxFixExecPublisher, fast::FastFeedPublisher>
        sim(exec_pub, fast_pub);
    sim.load_products(products);

    return run_sim(sim, exec_pub, fast_pub, cfg, products);
}
