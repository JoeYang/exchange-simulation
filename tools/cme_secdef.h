#pragma once

#include "tools/instrument_info.h"
#include "tools/secdef_consumer.h"
#include "tools/udp_multicast.h"
#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_encoder.h"  // ENGINE_TO_PRICE9_FACTOR, PRICE_SCALE

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unordered_map>

namespace exchange {

// CmeSecdefConsumer -- discovers instruments from CME MDP3 secdef multicast.
//
// Joins the secdef multicast group, decodes MDInstrumentDefinitionFuture54
// messages, and builds an InstrumentInfo map keyed by symbol.
//
// discover() blocks until tot_num_reports definitions are received or timeout.
class CmeSecdefConsumer : public SecdefConsumer {
    std::string secdef_group_;
    uint16_t    secdef_port_;

public:
    CmeSecdefConsumer(std::string group, uint16_t port)
        : secdef_group_(std::move(group)), secdef_port_(port) {}

    std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) override
    {
        using namespace cme::sbe;
        using namespace cme::sbe::mdp3;

        std::unordered_map<std::string, InstrumentInfo> result;

        UdpMulticastReceiver receiver;
        receiver.join_group(secdef_group_.c_str(), secdef_port_);

        // Set receive timeout.
        struct timeval tv{};
        tv.tv_sec = timeout_secs;
        tv.tv_usec = 0;
        ::setsockopt(receiver.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint32_t expected_total = 0;
        constexpr size_t BUF_SIZE = 2048;
        char buf[BUF_SIZE];

        while (true) {
            ssize_t n = receiver.receive(buf, BUF_SIZE);
            if (n <= 0) {
                // Timeout or error.
                if (result.empty()) {
                    std::fprintf(stderr,
                        "CmeSecdef: timeout -- no instruments discovered\n");
                }
                break;
            }

            // Skip 4-byte McastSeqHeader.
            if (static_cast<size_t>(n) <= sizeof(McastSeqHeader)) continue;
            const char* msg_buf = buf + sizeof(McastSeqHeader);
            size_t msg_len = static_cast<size_t>(n) - sizeof(McastSeqHeader);

            // Decode with visitor.
            struct Visitor {
                std::unordered_map<std::string, InstrumentInfo>& result;
                uint32_t& expected_total;

                void operator()(const DecodedInstrumentDef54& def) {
                    if (expected_total == 0 && def.root.tot_num_reports > 0) {
                        expected_total = def.root.tot_num_reports;
                    }

                    InstrumentInfo info;
                    info.security_id =
                        static_cast<uint32_t>(def.root.security_id);

                    // Trim trailing spaces from symbol (20-char field).
                    std::string sym(def.root.symbol,
                                    sizeof(def.root.symbol));
                    auto end = sym.find_last_not_of(' ');
                    if (end != std::string::npos)
                        sym.erase(end + 1);
                    else
                        sym.clear();
                    info.symbol = sym;

                    // Trim security_group (6-char field).
                    std::string sg(def.root.security_group,
                                   sizeof(def.root.security_group));
                    auto sg_end = sg.find_last_not_of(' ');
                    if (sg_end != std::string::npos)
                        sg.erase(sg_end + 1);
                    else
                        sg.clear();
                    info.product_group = sg;

                    // Convert min_price_increment (PRICE9) back to engine
                    // fixed-point. PRICE9 mantissa / ENGINE_TO_PRICE9_FACTOR.
                    info.tick_size =
                        def.root.min_price_increment.mantissa /
                        ENGINE_TO_PRICE9_FACTOR;

                    // lot_size from NoLotTypeRules (lot_type=1).
                    info.lot_size = PRICE_SCALE;  // default 1 contract
                    for (uint8_t i = 0; i < def.num_lot_types; ++i) {
                        if (def.lot_types[i].lot_type == 1) {
                            info.lot_size =
                                static_cast<int64_t>(
                                    def.lot_types[i].min_lot_size) *
                                PRICE_SCALE;
                            break;
                        }
                    }

                    // max_order_size: max_trade_vol * PRICE_SCALE
                    info.max_order_size =
                        static_cast<int64_t>(def.root.max_trade_vol) *
                        PRICE_SCALE;

                    info.match_algorithm = def.root.match_algorithm;
                    info.currency = std::string(def.root.currency, 3);
                    info.display_factor = def.root.display_factor.to_double();

                    result[sym] = std::move(info);
                }

                // Ignore other message types during secdef discovery.
                void operator()(const DecodedRefreshBook46&) {}
                void operator()(const DecodedTradeSummary48&) {}
                void operator()(const DecodedSecurityStatus30&) {}
                void operator()(const DecodedSnapshot53&) {}
            };

            decode_mdp3_message(msg_buf, msg_len,
                                Visitor{result, expected_total});

            // Early exit: received all expected instruments.
            if (expected_total > 0 &&
                result.size() >= expected_total) {
                break;
            }
        }

        receiver.leave_group();
        return result;
    }
};

}  // namespace exchange
