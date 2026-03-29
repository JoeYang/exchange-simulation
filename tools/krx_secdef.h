#pragma once

#include "tools/instrument_info.h"
#include "tools/secdef_consumer.h"
#include "tools/udp_multicast.h"
#include "krx/fast/fast_decoder.h"
#include "krx/fast/fast_encoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unordered_map>

namespace exchange {

// KrxSecdefConsumer -- discovers instruments from KRX FAST secdef multicast.
//
// Joins the secdef multicast group, decodes FastInstrumentDef (template 5)
// messages, and builds an InstrumentInfo map keyed by symbol.
//
// discover() blocks until total_instruments definitions are received or timeout.
class KrxSecdefConsumer : public SecdefConsumer {
    std::string secdef_group_;
    uint16_t    secdef_port_;

public:
    KrxSecdefConsumer(std::string group, uint16_t port)
        : secdef_group_(std::move(group)), secdef_port_(port) {}

    std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) override
    {
        using namespace krx::fast;

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

        // FAST secdef visitor: extracts InstrumentDef and populates result map.
        struct Visitor : public FastDecoderVisitorBase {
            std::unordered_map<std::string, InstrumentInfo>& result;
            uint32_t& expected_total;

            Visitor(std::unordered_map<std::string, InstrumentInfo>& r,
                    uint32_t& t)
                : result(r), expected_total(t) {}

            void on_instrument_def(const FastInstrumentDef& def) {
                if (expected_total == 0 && def.total_instruments > 0) {
                    expected_total = def.total_instruments;
                }

                InstrumentInfo info;
                info.security_id = def.instrument_id;

                // Extract null-padded symbol.
                size_t sym_len = strnlen(def.symbol, sizeof(def.symbol));
                info.symbol = std::string(def.symbol, sym_len);

                // Extract null-padded description.
                size_t desc_len = strnlen(def.description, sizeof(def.description));
                info.description = std::string(def.description, desc_len);

                // Map product_group uint8_t to string.
                switch (def.product_group) {
                    case 0: info.product_group = "Futures"; break;
                    case 1: info.product_group = "Options"; break;
                    case 2: info.product_group = "FX";      break;
                    case 3: info.product_group = "Bond";    break;
                    default: info.product_group = "Unknown"; break;
                }

                info.tick_size = def.tick_size;
                info.lot_size = def.lot_size;
                info.max_order_size = def.max_order_size;
                info.match_algorithm = 'F';  // KRX uses FIFO
                info.currency = "KRW";
                info.display_factor = 1.0 / 10000.0;  // PRICE_SCALE=10000

                result[info.symbol] = std::move(info);
            }
        };

        Visitor visitor(result, expected_total);

        while (true) {
            ssize_t n = receiver.receive(buf, BUF_SIZE);
            if (n <= 0) {
                if (result.empty()) {
                    std::fprintf(stderr,
                        "KrxSecdef: timeout -- no instruments discovered\n");
                }
                break;
            }

            // Strip 4-byte McastSeqHeader.
            if (static_cast<size_t>(n) <= sizeof(McastSeqHeader)) continue;
            const uint8_t* msg_buf =
                reinterpret_cast<const uint8_t*>(buf) + sizeof(McastSeqHeader);
            size_t msg_len = static_cast<size_t>(n) - sizeof(McastSeqHeader);

            decode_message(msg_buf, msg_len, visitor);

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
