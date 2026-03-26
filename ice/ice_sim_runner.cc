// ice_sim_runner.cc -- ICE Futures live simulator binary.
//
// Wires together:
//   - IceSimulator with IceFixExecPublisher + ImpactFeedPublisher
//   - TCP server for FIX 4.2 order entry (length-prefixed text frames)
//   - UDP multicast publisher for iMpact market data
//   - (optional) SHM transport for dashboard visualization
//
// Event loop:
//   1. Poll TCP for incoming FIX messages
//   2. Parse FIX, resolve symbol to instrument, route to matching engine
//   3. Engine fires callbacks on exec publisher and feed publisher
//   4. Pending FIX execution reports are sent back to the client over TCP
//   5. Pending iMpact packets are multicast over UDP

#include "exchange-core/composite_listener.h"
#include "ice/fix/fix_parser.h"
#include "ice/fix/ice_fix_exec_publisher.h"
#include "ice/fix/ice_fix_gateway.h"
#include "ice/fix/ice_fix_messages.h"
#include "ice/ice_products.h"
#include "ice/ice_sim_config.h"
#include "ice/ice_simulator.h"
#include "ice/impact/impact_publisher.h"
#include "tools/shm_listener.h"
#include "tools/shm_transport.h"
#include "tools/tcp_server.h"
#include "tools/udp_multicast.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// Filter products by the symbols listed in config. Empty list = keep all.
std::vector<exchange::ice::IceProductConfig> filter_products(
    const std::vector<exchange::ice::IceProductConfig>& all,
    const std::vector<std::string>& symbols)
{
    if (symbols.empty()) return all;

    std::unordered_set<std::string> wanted(symbols.begin(), symbols.end());
    std::vector<exchange::ice::IceProductConfig> result;
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

// ---------------------------------------------------------------------------
// SymbolRouter -- adapts IceSimulator's multi-instrument API to the
// single-engine interface that IceFixGateway expects.
//
// Before each gateway dispatch, the sim runner calls set_active_instrument()
// with the instrument_id resolved from the FIX Symbol tag. The gateway then
// calls new_order/cancel_order/modify_order on this router, which forwards
// to the IceSimulator with the active instrument_id.
// ---------------------------------------------------------------------------

template <typename SimulatorT>
struct SymbolRouter {
    SimulatorT& sim;
    uint32_t active_instrument{0};

    explicit SymbolRouter(SimulatorT& s) : sim(s) {}

    void new_order(const exchange::OrderRequest& req) {
        sim.new_order(active_instrument, req);
    }

    void cancel_order(exchange::OrderId id, exchange::Timestamp ts) {
        sim.cancel_order(active_instrument, id, ts);
    }

    void modify_order(const exchange::ModifyRequest& req) {
        sim.modify_order(active_instrument, req);
    }

    size_t mass_cancel(uint64_t account_id, exchange::Timestamp ts) {
        (void)account_id;
        (void)ts;
        // Mass cancel not routed per-instrument in sim runner.
        return 0;
    }
};

// run_sim -- shared event loop for both SHM and non-SHM modes.
template <typename SimulatorT>
int run_sim(SimulatorT& sim,
            exchange::ice::fix::IceFixExecPublisher& exec_pub,
            exchange::ice::impact::ImpactFeedPublisher& md_pub,
            const std::unordered_map<std::string, uint32_t>& symbol_map,
            const exchange::ice::IceSimConfig& cfg)
{
    using namespace exchange;
    using namespace exchange::ice;
    using namespace exchange::ice::fix;
    using namespace exchange::ice::impact;

    auto now = make_clock();

    sim.start_trading_day(now());
    sim.open_market(now());
    std::fprintf(stderr, "Market open. Session state: Continuous\n");

    // -- Create symbol router + FIX gateway --
    SymbolRouter<SimulatorT> router(sim);
    IceFixGateway<SymbolRouter<SimulatorT>> gateway(router);

    // -- Start UDP multicast publisher --
    UdpMulticastPublisher mcast(cfg.impact_group.c_str(), cfg.impact_port);
    std::fprintf(stderr, "iMpact multicast: %s:%u\n",
                 cfg.impact_group.c_str(), cfg.impact_port);

    // -- Track connected TCP clients for exec report broadcast --
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
        Timestamp ts = now();

        // Pre-parse the FIX message to extract the Symbol (tag 55) for
        // instrument routing. FIX text parsing is off-hot-path.
        auto pre_parse = ::ice::fix::parse_fix_message(data, len);
        if (!pre_parse.has_value()) {
            std::fprintf(stderr, "FIX parse error: %s\n",
                         pre_parse.error().c_str());
            return;
        }

        const auto& msg = pre_parse.value();
        std::string symbol = msg.get_string(tags::Symbol);
        auto it = symbol_map.find(symbol);
        if (it == symbol_map.end()) {
            std::fprintf(stderr, "Unknown symbol: %s\n", symbol.c_str());
            return;
        }

        // Set active instrument for the router before gateway dispatch.
        router.active_instrument = it->second;

        // Register order context with exec publisher for new orders (35=D).
        if (msg.msg_type == "D") {
            auto nos = FixNewOrderSingle::from_fix(msg);
            uint64_t cl_ord_id = static_cast<uint64_t>(
                std::strtoll(nos.cl_ord_id.c_str(), nullptr, 10));

            exchange::Side side;
            fix_to_side(nos.side, side);

            // Register with a placeholder OrderId; the engine will assign the
            // real one. We use cl_ord_id as a temporary key and the publisher
            // will match on the engine-assigned id via on_order_accepted.
            exec_pub.register_order(
                static_cast<OrderId>(cl_ord_id), cl_ord_id,
                fix_price_to_engine(msg.get_string(tags::Price)),
                fix_qty_to_engine(msg.get_string(tags::OrderQty)),
                side);
        }

        auto result = gateway.on_message(data, len, ts);
        if (!result.ok) {
            std::fprintf(stderr, "Gateway error: %s for fd=%d\n",
                         result.error.c_str(), fd);
        }
    };

    TcpServer tcp(tcp_cfg);
    std::fprintf(stderr, "FIX gateway listening on port %u\n", tcp.port());
    std::fprintf(stderr, "Simulator ready. Press Ctrl+C to stop.\n");

    // -- Event loop --
    while (g_running.load(std::memory_order_relaxed)) {
        // 1. Poll TCP for incoming FIX messages (non-blocking).
        //    The on_message callback fires synchronously during poll(),
        //    which runs the gateway -> engine -> publishers inline.
        tcp.poll(/* timeout_ms= */ 10);

        // 2. Flush pending execution reports back to connected clients.
        const auto& messages = exec_pub.messages();
        if (!messages.empty()) {
            for (const auto& msg : messages) {
                for (int fd : connected_fds) {
                    tcp.send_message(fd, msg.data(), msg.size());
                }
            }
            exec_pub.clear_messages();
        }

        // 3. Flush pending iMpact packets to multicast.
        if (md_pub.packet_count() > 0) {
            for (const auto& pkt : md_pub.packets()) {
                auto rc = mcast.send(pkt.bytes(), pkt.len);
                if (rc == SendResult::kError) {
                    std::fprintf(stderr, "iMpact multicast send error\n");
                }
            }
            md_pub.clear();
        }
    }

    // -- Shutdown --
    std::fprintf(stderr, "\nShutting down...\n");
    sim.close_market(now());
    sim.end_of_day(now());
    tcp.shutdown();
    std::fprintf(stderr, "Simulator stopped.\n");

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace exchange;
    using namespace exchange::ice;
    using namespace exchange::ice::fix;
    using namespace exchange::ice::impact;

    // -- Parse config --
    IceSimConfig cfg;
    if (!IceSimConfig::parse(argc, argv, cfg)) {
        IceSimConfig::print_usage(argv[0]);
        return 1;
    }

    // -- Install signal handlers for clean shutdown --
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -- Load products --
    auto all_products = get_ice_products();
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

    // -- Build symbol -> instrument_id map for FIX routing --
    std::unordered_map<std::string, uint32_t> symbol_map;
    for (const auto& p : products) {
        symbol_map[p.symbol] = p.instrument_id;
    }

    // -- Create FIX exec publisher (uses first product symbol as default) --
    IceFixExecPublisher exec_pub("ICE-SIM", "CLIENT", products[0].symbol);

    // -- Create iMpact feed publisher --
    ImpactFeedPublisher md_pub(
        static_cast<int32_t>(products[0].instrument_id));

    if (!cfg.shm_path.empty()) {
        // -- SHM mode: composite listeners fan out to protocol publishers + SHM --
        std::fprintf(stderr, "SHM transport: %s\n", cfg.shm_path.c_str());

        ShmProducer shm_producer(cfg.shm_path);
        SharedMemoryOrderListener shm_order_listener(shm_producer);
        SharedMemoryMdListener shm_md_listener(shm_producer);

        using CompositeOL = CompositeOrderListener<IceFixExecPublisher,
                                                   SharedMemoryOrderListener>;
        using CompositeML = CompositeMdListener<ImpactFeedPublisher,
                                                SharedMemoryMdListener>;

        CompositeOL composite_ol(&exec_pub, &shm_order_listener);
        CompositeML composite_ml(&md_pub, &shm_md_listener);

        IceSimulator<CompositeOL, CompositeML> sim(composite_ol, composite_ml);
        sim.load_products(products);

        return run_sim(sim, exec_pub, md_pub, symbol_map, cfg);
    }

    // -- Standard mode: direct listeners, no SHM --
    IceSimulator<IceFixExecPublisher, ImpactFeedPublisher> sim(exec_pub, md_pub);
    sim.load_products(products);

    return run_sim(sim, exec_pub, md_pub, symbol_map, cfg);
}
