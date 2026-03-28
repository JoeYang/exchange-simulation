#pragma once

#include "tools/instrument_info.h"
#include "tools/secdef_consumer.h"
#include "tools/udp_multicast.h"
#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_messages.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unordered_map>

namespace exchange {

// IceSecdefConsumer -- discovers instruments from ICE iMpact multicast.
//
// Joins the iMpact multicast group (same channel as incremental data),
// filters for InstrumentDefinition messages, and builds an InstrumentInfo
// map keyed by symbol.
//
// discover() blocks until no new instruments are received for 5 seconds
// or the global timeout expires.
class IceSecdefConsumer : public SecdefConsumer {
    std::string impact_group_;
    uint16_t    impact_port_;

public:
    IceSecdefConsumer(std::string group, uint16_t port)
        : impact_group_(std::move(group)), impact_port_(port) {}

    std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) override
    {
        using namespace ice::impact;

        std::unordered_map<std::string, InstrumentInfo> result;

        UdpMulticastReceiver receiver;
        receiver.join_group(impact_group_.c_str(), impact_port_);

        // Use a short receive timeout (1s) so we can check the idle timer.
        struct timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        ::setsockopt(receiver.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        auto start = std::chrono::steady_clock::now();
        auto last_new = start;
        constexpr int IDLE_SECS = 5;  // stop after 5s with no new instruments

        constexpr size_t BUF_SIZE = 2048;
        char buf[BUF_SIZE];

        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - start).count();
            if (elapsed >= timeout_secs) break;

            auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_new).count();
            if (!result.empty() && idle >= IDLE_SECS) break;

            ssize_t n = receiver.receive(buf, BUF_SIZE);
            if (n <= 0) continue;  // timeout or error, retry

            // Skip 4-byte McastSeqHeader.
            if (static_cast<size_t>(n) <= sizeof(McastSeqHeader)) continue;
            const char* msg_buf = buf + sizeof(McastSeqHeader);
            size_t msg_len = static_cast<size_t>(n) - sizeof(McastSeqHeader);

            // Decode and filter for InstrumentDefinition.
            struct Visitor {
                std::unordered_map<std::string, InstrumentInfo>& result;
                bool found_new{false};

                void on_bundle_start(const BundleStart&) {}
                void on_bundle_end(const BundleEnd&) {}
                void on_add_modify_order(const AddModifyOrder&) {}
                void on_order_withdrawal(const OrderWithdrawal&) {}
                void on_deal_trade(const DealTrade&) {}
                void on_market_status(const ice::impact::MarketStatus&) {}
                void on_snapshot_order(const SnapshotOrder&) {}
                void on_price_level(const ice::impact::PriceLevel&) {}

                void on_instrument_def(const InstrumentDefinition& msg) {
                    // Extract symbol: trim trailing nulls.
                    std::string sym(msg.symbol, sizeof(msg.symbol));
                    auto pos = sym.find('\0');
                    if (pos != std::string::npos) sym.erase(pos);
                    if (sym.empty()) return;

                    // Check for new instrument (not a duplicate).
                    if (result.count(sym)) return;
                    found_new = true;

                    InstrumentInfo info;
                    info.security_id =
                        static_cast<uint32_t>(msg.instrument_id);
                    info.symbol = sym;

                    // Trim description.
                    std::string desc(msg.description,
                                     sizeof(msg.description));
                    auto dp = desc.find('\0');
                    if (dp != std::string::npos) desc.erase(dp);
                    info.description = std::move(desc);

                    // Trim product_group.
                    std::string pg(msg.product_group,
                                   sizeof(msg.product_group));
                    auto pp = pg.find('\0');
                    if (pp != std::string::npos) pg.erase(pp);
                    info.product_group = std::move(pg);

                    // Pass-through: already engine fixed-point.
                    info.tick_size      = msg.tick_size;
                    info.lot_size       = msg.lot_size;
                    info.max_order_size = msg.max_order_size;

                    // Map match_algo: 0=FIFO -> 'F', 1=GTBPR -> 'P'
                    info.match_algorithm =
                        (msg.match_algo == 0) ? 'F' : 'P';

                    // Trim currency.
                    std::string ccy(msg.currency, sizeof(msg.currency));
                    auto cp = ccy.find('\0');
                    if (cp != std::string::npos) ccy.erase(cp);
                    info.currency = std::move(ccy);

                    // ICE display factor: 1/PRICE_SCALE = 0.0001
                    info.display_factor = 0.0001;

                    result[info.symbol] = std::move(info);
                }
            };

            Visitor visitor{result};
            decode_messages(msg_buf, msg_len, visitor);

            if (visitor.found_new) {
                last_new = std::chrono::steady_clock::now();
            }
        }

        if (result.empty()) {
            std::fprintf(stderr,
                "IceSecdef: timeout -- no instruments discovered\n");
        }

        receiver.leave_group();
        return result;
    }
};

}  // namespace exchange
