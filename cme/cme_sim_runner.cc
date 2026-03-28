// cme_sim_runner.cc -- CME Globex live simulator binary.
//
// Wires together:
//   - CmeSimulator with ILink3ReportPublisher + Mdp3FeedPublisher
//   - TCP server for iLink3 order entry (length-prefixed SBE frames)
//   - UDP multicast publisher for MDP3 market data
//   - ILink3Gateway to decode incoming SBE and route to the simulator
//   - (optional) SHM transport for dashboard visualization
//
// Event loop:
//   1. Poll TCP for incoming iLink3 messages
//   2. Gateway decodes SBE, routes to matching engine
//   3. Engine fires callbacks on report publisher and feed publisher
//   4. Pending execution reports are sent back to the client over TCP
//   5. Pending MDP3 packets are multicast over UDP

#include "cme/cme_products.h"
#include "cme/cme_simulator.h"
#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/mdp3_encoder.h"
#include "cme/ilink3_gateway.h"
#include "cme/ilink3_report_publisher.h"
#include "cme/mdp3_feed_publisher.h"
#include "cme/sim_config.h"
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

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// Filter products by the symbols listed in config. Empty list = keep all.
std::vector<exchange::cme::CmeProductConfig> filter_products(
    const std::vector<exchange::cme::CmeProductConfig>& all,
    const std::vector<std::string>& symbols)
{
    if (symbols.empty()) return all;

    std::unordered_set<std::string> wanted(symbols.begin(), symbols.end());
    std::vector<exchange::cme::CmeProductConfig> result;
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
        return static_cast<exchange::Timestamp>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    };
}

// Publish secdef messages for all products on the secdef multicast channel.
void publish_secdefs(
    exchange::UdpMulticastPublisher& secdef_pub,
    const std::vector<exchange::cme::CmeProductConfig>& products)
{
    using namespace exchange::cme::sbe::mdp3;
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto total = static_cast<uint32_t>(products.size());
    for (const auto& product : products) {
        size_t n = encode_instrument_definition(buf, product, total);
        secdef_pub.send(buf, n);
    }
}

// run_sim -- shared event loop for both SHM and non-SHM modes.
//
// Templated on the simulator type so the gateway and engine dispatch are
// resolved at compile time regardless of which listener combination is used.
template <typename SimulatorT>
int run_sim(SimulatorT& sim,
            exchange::cme::ILink3ReportPublisher& report_pub,
            exchange::cme::Mdp3FeedPublisher& md_pub,
            const exchange::cme::SimConfig& cfg,
            const std::vector<exchange::cme::CmeProductConfig>& products)
{
    using namespace exchange;
    using namespace exchange::cme;

    auto now = make_clock();

    // -- Start secdef multicast publisher --
    UdpMulticastPublisher secdef_mcast(cfg.secdef_group.c_str(),
                                       cfg.secdef_port);
    std::fprintf(stderr, "Secdef multicast: %s:%u\n",
                 cfg.secdef_group.c_str(), cfg.secdef_port);

    // Publish instrument definitions before opening the market.
    publish_secdefs(secdef_mcast, products);
    std::fprintf(stderr, "Published %zu instrument definition(s)\n",
                 products.size());

    sim.start_trading_day(now());
    sim.open_market(now());
    std::fprintf(stderr, "Market open. Session state: Continuous\n");

    // -- Create ILink3 gateway --
    ILink3Gateway gateway(sim);

    // -- Start UDP multicast publisher --
    UdpMulticastPublisher mcast(cfg.mdp3_group.c_str(), cfg.mdp3_port);
    std::fprintf(stderr, "MDP3 multicast: %s:%u\n",
                 cfg.mdp3_group.c_str(), cfg.mdp3_port);

    // -- Track connected TCP clients for exec report broadcast --
    std::unordered_set<int> connected_fds;

    // -- Start TCP server for iLink3 --
    TcpServer::Config tcp_cfg{};
    tcp_cfg.port = cfg.ilink3_port;

    tcp_cfg.on_connect = [&](int fd) {
        connected_fds.insert(fd);
        std::fprintf(stderr, "iLink3 client connected: fd=%d (total=%zu)\n",
                     fd, connected_fds.size());
    };

    tcp_cfg.on_disconnect = [&](int fd) {
        connected_fds.erase(fd);
        std::fprintf(stderr, "iLink3 client disconnected: fd=%d (total=%zu)\n",
                     fd, connected_fds.size());
    };

    tcp_cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;

        // Register order context with the report publisher before gateway
        // processing, so accepted callbacks carry full order state.
        // Peek at the SBE template ID to decide if it's a new order.
        if (len >= 8) {
            // SBE header: blockLength(2) + templateId(2) + schemaId(2) + version(2)
            uint16_t template_id = 0;
            std::memcpy(&template_id, data + 2, sizeof(template_id));
            // NewOrderSingle514 — register order context
            if (template_id == 514) {
                sbe::ilink3::DecodedNewOrder514 decoded{};
                if (len >= sizeof(sbe::MessageHeader) + sizeof(sbe::ilink3::NewOrderSingle514)) {
                    std::memcpy(&decoded.root,
                                data + sizeof(sbe::MessageHeader),
                                sizeof(sbe::ilink3::NewOrderSingle514));
                    OrderRequest req{};
                    req.client_order_id =
                        sbe::ilink3::decode_cl_ord_id(decoded.root.cl_ord_id);
                    req.price = sbe::ilink3::price9_to_engine(decoded.root.price);
                    req.quantity = sbe::ilink3::wire_qty_to_engine(decoded.root.order_qty);
                    req.side = sbe::ilink3::decode_side(decoded.root.side);
                    req.type = sbe::ilink3::decode_ord_type(decoded.root.ord_type);
                    req.tif = sbe::ilink3::decode_tif(decoded.root.time_in_force);
                    req.display_qty =
                        sbe::ilink3::wire_qty_to_engine(decoded.root.display_qty);
                    report_pub.register_order(req);
                }
            }
        }

        auto result = gateway.process(data, len);
        if (result != GatewayResult::kOk) {
            std::fprintf(stderr, "Gateway error: %d for fd=%d\n",
                         static_cast<int>(result), fd);
        }
    };

    TcpServer tcp(tcp_cfg);
    std::fprintf(stderr, "iLink3 gateway listening on port %u\n", tcp.port());
    std::fprintf(stderr, "Simulator ready. Press Ctrl+C to stop.\n");

    // -- Secdef re-publish timer (every 30 seconds) --
    auto last_secdef_publish = std::chrono::steady_clock::now();
    constexpr auto secdef_interval = std::chrono::seconds(30);

    // -- Event loop --
    while (g_running.load(std::memory_order_relaxed)) {
        // 1. Poll TCP for incoming iLink3 messages (non-blocking).
        //    The on_message callback fires synchronously during poll(),
        //    which runs the gateway -> engine -> publishers inline.
        tcp.poll(/* timeout_ms= */ 10);

        // 2. Flush pending execution reports back to the active client.
        if (report_pub.report_count() > 0) {
            for (const auto& report : report_pub.reports()) {
                // Send to all connected clients (broadcast exec reports).
                for (int fd : connected_fds) {
                    tcp.send_message(fd, report.data.data(),
                                     report.length);
                }
            }
            report_pub.clear_reports();
        }

        // 3. Flush pending MDP3 packets to multicast.
        if (md_pub.packet_count() > 0) {
            for (const auto& pkt : md_pub.packets()) {
                auto rc = mcast.send(pkt.bytes(), pkt.len);
                if (rc == SendResult::kError) {
                    std::fprintf(stderr, "MDP3 multicast send error\n");
                }
            }
            md_pub.clear();
        }

        // 4. Re-publish secdef messages every 30 seconds.
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - last_secdef_publish >= secdef_interval) {
            publish_secdefs(secdef_mcast, products);
            last_secdef_publish = now_tp;
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
    using namespace exchange::cme;

    // -- Parse config --
    SimConfig cfg;
    if (!SimConfig::parse(argc, argv, cfg)) {
        SimConfig::print_usage(argv[0]);
        return 1;
    }

    // -- Install signal handlers for clean shutdown --
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -- Load products --
    auto all_products = get_cme_products();
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

    // -- Create iLink3 encode context --
    sbe::ilink3::EncodeContext ilink_ctx{};
    ilink_ctx.uuid = 1;
    ilink_ctx.seq_num = 1;
    ilink_ctx.security_id = static_cast<int32_t>(products[0].instrument_id);
    std::memcpy(ilink_ctx.sender_id, "CME-SIM", 7);
    std::memcpy(ilink_ctx.location, "US,IL", sizeof(ilink_ctx.location));
    ilink_ctx.party_details_list_req_id = 1;

    ILink3ReportPublisher report_pub(ilink_ctx);

    // Initialize the MDP3 feed publisher with the first product's security_id
    // so encoded packets carry the correct instrument identifier.  Without this,
    // security_id defaults to 0 and multicast consumers that filter by instrument
    // (e.g. exchange-observer) silently discard every book/trade/status message.
    char security_group[7]{};  // 6-char padded field
    char asset[7]{};
    std::snprintf(security_group, sizeof(security_group), "%-6s",
                  products[0].product_group.c_str());
    std::snprintf(asset, sizeof(asset), "%-6s",
                  products[0].symbol.c_str());
    Mdp3FeedPublisher md_pub(
        static_cast<int32_t>(products[0].instrument_id),
        security_group,
        asset);

    if (!cfg.shm_path.empty()) {
        // -- SHM mode: composite listeners fan out to protocol publishers + SHM --
        std::fprintf(stderr, "SHM transport: %s\n", cfg.shm_path.c_str());

        ShmProducer shm_producer(cfg.shm_path);
        SharedMemoryOrderListener shm_order_listener(shm_producer);
        SharedMemoryMdListener shm_md_listener(shm_producer);

        using CompositeOL = CompositeOrderListener<ILink3ReportPublisher,
                                                   SharedMemoryOrderListener>;
        using CompositeML = CompositeMdListener<Mdp3FeedPublisher,
                                                SharedMemoryMdListener>;

        CompositeOL composite_ol(&report_pub, &shm_order_listener);
        CompositeML composite_ml(&md_pub, &shm_md_listener);

        CmeSimulator<CompositeOL, CompositeML> sim(composite_ol, composite_ml);
        sim.load_products(products);

        return run_sim(sim, report_pub, md_pub, cfg, products);
    }

    // -- Standard mode: direct listeners, no SHM --
    CmeSimulator<ILink3ReportPublisher, Mdp3FeedPublisher> sim(report_pub, md_pub);
    sim.load_products(products);

    return run_sim(sim, report_pub, md_pub, cfg, products);
}
