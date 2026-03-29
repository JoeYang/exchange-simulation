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
#include "ice/impact/impact_encoder.h"
#include "ice/impact/impact_publisher.h"
#include "tools/shm_listener.h"
#include "tools/shm_transport.h"
#include "tools/tcp_server.h"
#include "tools/udp_multicast.h"

#include <atomic>
#include <chrono>
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

// Publish InstrumentDefinition for all products on the given multicast socket.
void publish_secdefs(
    const std::vector<exchange::ice::IceProductConfig>& products,
    exchange::UdpMulticastPublisher& mcast)
{
    using namespace exchange::ice::impact;
    ImpactEncodeContext ctx{};
    char buf[MAX_IMPACT_ENCODED_SIZE];

    for (const auto& p : products) {
        size_t len = encode_instrument_definition(buf, sizeof(buf), p, ctx);
        if (len > 0) {
            auto rc = mcast.send(buf, len);
            if (rc == exchange::SendResult::kError) {
                std::fprintf(stderr, "secdef multicast send error: %s\n",
                             p.symbol.c_str());
            }
        }
    }
}

// run_sim -- shared event loop for both SHM and non-SHM modes.
template <typename SimulatorT>
int run_sim(SimulatorT& sim,
            exchange::ice::fix::IceFixExecPublisher& exec_pub,
            exchange::ice::impact::ImpactFeedPublisher& md_pub,
            const std::unordered_map<std::string, uint32_t>& symbol_map,
            const std::vector<exchange::ice::IceProductConfig>& products,
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

            // Store in pending_ keyed by client_order_id; on_order_accepted()
            // will remap to the engine-assigned OrderId.
            exec_pub.register_order(
                cl_ord_id,
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

    // -- Start TCP snapshot server --
    TcpServer* snap_server_ptr = nullptr;
    TcpServer::Config snap_cfg{};
    snap_cfg.port = cfg.snapshot_port;

    snap_cfg.on_message = [&](int fd, const char* data, size_t len) {
        // Parse "SNAP <symbol>\n" request.
        std::string req(data, len);
        if (req.size() < 6 || req.substr(0, 5) != "SNAP ") {
            std::fprintf(stderr, "Snapshot: invalid request: %s\n", req.c_str());
            return;
        }
        // Strip trailing newline.
        std::string symbol = req.substr(5);
        if (!symbol.empty() && symbol.back() == '\n') symbol.pop_back();

        auto it = symbol_map.find(symbol);
        if (it == symbol_map.end()) {
            std::fprintf(stderr, "Snapshot: unknown symbol: %s\n", symbol.c_str());
            return;
        }
        uint32_t iid = it->second;

        // Encode snapshot: BundleStart + SnapshotOrders + BundleEnd(seq=0).
        // Max buffer: BundleStart(17) + 256*SnapshotOrder(32) + BundleEnd(7) = 8216
        constexpr size_t SNAP_BUF_SIZE = 16384;
        char snap_buf[SNAP_BUF_SIZE];
        ImpactEncodeContext snap_ctx{};
        snap_ctx.instrument_id = static_cast<int32_t>(iid);

        // Get the engine and iterate its book.
        const exchange::PriceLevel* first_bid = nullptr;
        const exchange::PriceLevel* first_ask = nullptr;

        // Build linked PriceLevel lists from the engine's book.
        // for_each_level gives us (price, qty, order_count) — we need to
        // build temporary PriceLevel nodes for encode_snapshot_orders().
        struct SnapLevel {
            exchange::PriceLevel lv{};
        };
        std::vector<SnapLevel> bid_levels, ask_levels;

        auto collect = [](auto* engine, std::vector<SnapLevel>& bids,
                          std::vector<SnapLevel>& asks) {
            if (!engine) return;
            engine->for_each_level(exchange::Side::Buy,
                [&](exchange::Price p, exchange::Quantity q, uint32_t oc) {
                    SnapLevel sl{};
                    sl.lv.price = p;
                    sl.lv.total_quantity = q;
                    sl.lv.order_count = oc;
                    bids.push_back(sl);
                });
            engine->for_each_level(exchange::Side::Sell,
                [&](exchange::Price p, exchange::Quantity q, uint32_t oc) {
                    SnapLevel sl{};
                    sl.lv.price = p;
                    sl.lv.total_quantity = q;
                    sl.lv.order_count = oc;
                    asks.push_back(sl);
                });
        };

        if (sim.is_gtbpr_instrument(iid)) {
            collect(sim.get_gtbpr_engine(iid), bid_levels, ask_levels);
        } else {
            collect(sim.get_fifo_engine(iid), bid_levels, ask_levels);
        }

        // Link the temporary PriceLevels into a list.
        for (size_t i = 0; i < bid_levels.size(); ++i) {
            bid_levels[i].lv.next = (i + 1 < bid_levels.size())
                ? &bid_levels[i + 1].lv : nullptr;
        }
        for (size_t i = 0; i < ask_levels.size(); ++i) {
            ask_levels[i].lv.next = (i + 1 < ask_levels.size())
                ? &ask_levels[i + 1].lv : nullptr;
        }

        first_bid = bid_levels.empty() ? nullptr : &bid_levels[0].lv;
        first_ask = ask_levels.empty() ? nullptr : &ask_levels[0].lv;

        size_t snap_len = encode_snapshot_orders(
            snap_buf, SNAP_BUF_SIZE,
            static_cast<int32_t>(iid),
            first_bid, first_ask, snap_ctx);

        if (snap_len > 0) {
            snap_server_ptr->send_message(fd, snap_buf, snap_len);
            std::fprintf(stderr, "Snapshot: sent %zu bytes for %s (%zu bid + %zu ask levels)\n",
                         snap_len, symbol.c_str(), bid_levels.size(), ask_levels.size());
        }
    };

    TcpServer snap_server(snap_cfg);
    snap_server_ptr = &snap_server;
    std::fprintf(stderr, "Snapshot server listening on port %u\n", snap_server.port());

    // -- Publish initial security definitions on iMpact channel --
    publish_secdefs(products, mcast);
    std::fprintf(stderr, "Published %zu secdef(s) on iMpact channel\n",
                 products.size());

    std::fprintf(stderr, "Simulator ready. Press Ctrl+C to stop.\n");

    // -- Secdef re-publish timer (every 30 seconds) --
    auto last_secdef = std::chrono::steady_clock::now();
    constexpr auto SECDEF_INTERVAL = std::chrono::seconds(30);

    // -- Event loop --
    while (g_running.load(std::memory_order_relaxed)) {
        // 1. Poll TCP for incoming FIX messages (non-blocking).
        //    The on_message callback fires synchronously during poll(),
        //    which runs the gateway -> engine -> publishers inline.
        tcp.poll(/* timeout_ms= */ 10);

        // 1b. Poll TCP snapshot server for snapshot requests.
        snap_server.poll(/* timeout_ms= */ 0);

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

        // 4. Periodic secdef re-publish (every 30 seconds).
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - last_secdef >= SECDEF_INTERVAL) {
            publish_secdefs(products, mcast);
            last_secdef = now_tp;
        }
    }

    // -- Shutdown --
    std::fprintf(stderr, "\nShutting down...\n");
    sim.close_market(now());
    sim.end_of_day(now());
    tcp.shutdown();
    snap_server.shutdown();
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

        return run_sim(sim, exec_pub, md_pub, symbol_map, products, cfg);
    }

    // -- Standard mode: direct listeners, no SHM --
    IceSimulator<IceFixExecPublisher, ImpactFeedPublisher> sim(exec_pub, md_pub);
    sim.load_products(products);

    return run_sim(sim, exec_pub, md_pub, symbol_map, products, cfg);
}
