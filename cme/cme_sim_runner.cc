// cme_sim_runner.cc -- CME Globex live simulator binary.
//
// Wires together:
//   - CmeSimulator with ILink3ReportPublisher + Mdp3FeedPublisher
//   - TCP server for iLink3 order entry (length-prefixed SBE frames)
//   - UDP multicast publisher for MDP3 market data
//   - ILink3Gateway to decode incoming SBE and route to the simulator
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
#include "cme/ilink3_gateway.h"
#include "cme/ilink3_report_publisher.h"
#include "cme/mdp3_feed_publisher.h"
#include "cme/sim_config.h"
#include "tools/tcp_server.h"
#include "tools/udp_multicast.h"

#include <atomic>
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

    // -- Create listeners --
    // Use the first product's instrument_id as the iLink3 encode context's
    // security_id. Individual fill reports override this per-order.
    sbe::ilink3::EncodeContext ilink_ctx{};
    ilink_ctx.uuid = 1;
    ilink_ctx.seq_num = 1;
    ilink_ctx.security_id = static_cast<int32_t>(products[0].instrument_id);
    std::memcpy(ilink_ctx.sender_id, "CME-SIM ", 8);
    std::memcpy(ilink_ctx.location, "US,IL   ", 8);
    ilink_ctx.party_details_list_req_id = 1;

    ILink3ReportPublisher report_pub(ilink_ctx);
    Mdp3FeedPublisher md_pub;

    // -- Create simulator + load products --
    CmeSimulator<ILink3ReportPublisher, Mdp3FeedPublisher> sim(report_pub, md_pub);
    sim.load_products(products);

    // -- Start trading session --
    auto now = []() -> Timestamp {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    };

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
    }

    // -- Shutdown --
    std::fprintf(stderr, "\nShutting down...\n");
    sim.close_market(now());
    sim.end_of_day(now());
    tcp.shutdown();
    std::fprintf(stderr, "Simulator stopped.\n");

    return 0;
}
