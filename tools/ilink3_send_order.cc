// ilink3_send_order.cc -- CLI tool to send iLink3 SBE-encoded orders to cme-sim.
//
// Connects via TCP, sends length-prefixed SBE frames matching the TcpServer
// protocol (4-byte LE length + payload), waits for execution report responses.
//
// Usage:
//   ilink3-send-order --host 127.0.0.1 --port 9100
//     --instrument ES --side BUY --price 5000.00 --qty 10 --type LIMIT --tif DAY
//
// Subcommands:
//   (default)  Send NewOrderSingle514
//   --cancel   Send OrderCancelRequest516 (requires --order-id)
//   --replace  Send OrderCancelReplaceRequest515 (requires --order-id)

#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"
#include "cme/codec/ilink3_messages.h"
#include "cme/codec/sbe_header.h"
#include "cme/cme_products.h"
#include "exchange-core/types.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace exchange;
using namespace exchange::cme;

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

enum class Command : uint8_t { NewOrder, Cancel, Replace };

struct Args {
    std::string host{"127.0.0.1"};
    uint16_t    port{9100};
    std::string instrument{"ES"};
    std::string side_str{"BUY"};
    double      price{0.0};
    uint32_t    qty{1};
    std::string type_str{"LIMIT"};
    std::string tif_str{"DAY"};
    std::string account{"FIRM_A"};
    uint64_t    order_id{0};       // for cancel/replace
    uint64_t    cl_ord_id{1};
    int         count{1};
    Command     command{Command::NewOrder};
};

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --host HOST          TCP host              (default: 127.0.0.1)\n"
        "  --port PORT          TCP port              (default: 9100)\n"
        "  --instrument SYM     Instrument symbol     (ES, NQ, CL, GC, ZN, ZB, MES, 6E)\n"
        "  --side SIDE          BUY or SELL           (default: BUY)\n"
        "  --price PRICE        Price in decimal      (e.g. 5000.25)\n"
        "  --qty QTY            Quantity in contracts  (default: 1)\n"
        "  --type TYPE          LIMIT, MARKET, STOP, STOPLIMIT (default: LIMIT)\n"
        "  --tif TIF            DAY, GTC, IOC, FOK, GTD        (default: DAY)\n"
        "  --account ACCT       Account identifier    (default: FIRM_A)\n"
        "  --order-id ID        Exchange order ID     (for cancel/replace)\n"
        "  --cl-ord-id ID       Client order ID       (default: 1)\n"
        "  --count N            Repeat N times        (default: 1)\n"
        "  --cancel             Send cancel request   (requires --order-id)\n"
        "  --replace            Send replace request  (requires --order-id)\n"
        "  -h, --help           Show this message\n",
        prog);
}

bool parse_args(int argc, char* argv[], Args& out) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            args.host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            args.instrument = argv[++i];
        } else if (std::strcmp(argv[i], "--side") == 0 && i + 1 < argc) {
            args.side_str = argv[++i];
        } else if (std::strcmp(argv[i], "--price") == 0 && i + 1 < argc) {
            args.price = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--qty") == 0 && i + 1 < argc) {
            args.qty = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            args.type_str = argv[++i];
        } else if (std::strcmp(argv[i], "--tif") == 0 && i + 1 < argc) {
            args.tif_str = argv[++i];
        } else if (std::strcmp(argv[i], "--account") == 0 && i + 1 < argc) {
            args.account = argv[++i];
        } else if (std::strcmp(argv[i], "--order-id") == 0 && i + 1 < argc) {
            args.order_id = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--cl-ord-id") == 0 && i + 1 < argc) {
            args.cl_ord_id = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            args.count = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cancel") == 0) {
            args.command = Command::Cancel;
        } else if (std::strcmp(argv[i], "--replace") == 0) {
            args.command = Command::Replace;
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
// Instrument symbol -> security_id mapping
// ---------------------------------------------------------------------------

int32_t resolve_security_id(const std::string& symbol) {
    auto products = get_cme_products();
    for (const auto& p : products) {
        if (p.symbol == symbol) {
            return static_cast<int32_t>(p.instrument_id);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Enum parsing
// ---------------------------------------------------------------------------

Side parse_side(const std::string& s) {
    if (s == "SELL" || s == "S") return Side::Sell;
    return Side::Buy;
}

OrderType parse_order_type(const std::string& s) {
    if (s == "MARKET")    return OrderType::Market;
    if (s == "STOP")      return OrderType::Stop;
    if (s == "STOPLIMIT") return OrderType::StopLimit;
    return OrderType::Limit;
}

TimeInForce parse_tif(const std::string& s) {
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTD") return TimeInForce::GTD;
    return TimeInForce::DAY;
}

// ---------------------------------------------------------------------------
// Price conversion: decimal -> engine fixed-point (PRICE_SCALE=10000)
//   5000.25 -> 50002500
// ---------------------------------------------------------------------------

Price decimal_to_engine_price(double d) {
    return static_cast<Price>(std::round(d * PRICE_SCALE));
}

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------

Timestamp now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// TCP client: connect, send length-prefixed message, recv length-prefixed
// ---------------------------------------------------------------------------

class TcpClient {
    int fd_{-1};

public:
    TcpClient() = default;
    ~TcpClient() { close_conn(); }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connect_to(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            std::fprintf(stderr, "socket() failed: %s\n", std::strerror(errno));
            return false;
        }

        // Disable Nagle for low-latency sends.
        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            std::fprintf(stderr, "Invalid address: %s\n", host.c_str());
            close_conn();
            return false;
        }

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                       sizeof(addr)) < 0) {
            std::fprintf(stderr, "connect() failed: %s\n", std::strerror(errno));
            close_conn();
            return false;
        }

        return true;
    }

    // Send a length-prefixed message (4-byte LE length + payload).
    bool send_message(const char* data, size_t len) {
        uint32_t frame_len = static_cast<uint32_t>(len);
        // Write 4-byte LE length prefix.
        char header[4];
        std::memcpy(header, &frame_len, 4);

        if (!send_all(header, 4)) return false;
        if (!send_all(data, len)) return false;
        return true;
    }

    // Receive a length-prefixed message. Returns payload length, or -1 on error.
    // Writes payload into buf (must be at least max_len bytes).
    ssize_t recv_message(char* buf, size_t max_len) {
        // Read 4-byte length prefix.
        char header[4];
        if (!recv_all(header, 4)) return -1;

        uint32_t frame_len = 0;
        std::memcpy(&frame_len, header, 4);

        if (frame_len > max_len) {
            std::fprintf(stderr, "Response too large: %u bytes\n", frame_len);
            return -1;
        }

        if (!recv_all(buf, frame_len)) return -1;
        return static_cast<ssize_t>(frame_len);
    }

    void close_conn() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const { return fd_; }

private:
    bool send_all(const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                std::fprintf(stderr, "send() failed: %s\n", std::strerror(errno));
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recv_all(char* buf, size_t len) {
        size_t received = 0;
        while (received < len) {
            ssize_t n = ::recv(fd_, buf + received, len - received, 0);
            if (n <= 0) {
                if (n == 0) {
                    std::fprintf(stderr, "Connection closed by server\n");
                } else {
                    std::fprintf(stderr, "recv() failed: %s\n", std::strerror(errno));
                }
                return false;
            }
            received += static_cast<size_t>(n);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Response printer — decode and display execution reports
// ---------------------------------------------------------------------------

const char* side_str(uint8_t wire_side) {
    return (wire_side == 2) ? "SELL" : "BUY";
}

const char* ord_status_str(uint8_t status) {
    switch (status) {
        case static_cast<uint8_t>('0'): return "New";
        case static_cast<uint8_t>('1'): return "PartiallyFilled";
        case static_cast<uint8_t>('2'): return "Filled";
        case static_cast<uint8_t>('4'): return "Canceled";
        case static_cast<uint8_t>('8'): return "Rejected";
        case static_cast<uint8_t>('C'): return "Expired";
        default: return "Unknown";
    }
}

void print_response(const char* buf, size_t len) {
    using namespace sbe;
    using namespace sbe::ilink3;

    auto rc = decode_ilink3_message(buf, len, [](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;

        if constexpr (std::is_same_v<T, DecodedExecNew522>) {
            const auto& m = decoded.root;
            std::printf("  ExecutionReportNew522:\n");
            std::printf("    OrderId:   %lu\n", static_cast<unsigned long>(m.order_id));
            std::printf("    ClOrdId:   %.20s\n", m.cl_ord_id);
            std::printf("    Side:      %s\n", side_str(m.side));
            std::printf("    Price:     %.4f\n", m.price.to_double());
            std::printf("    OrderQty:  %u\n", m.order_qty);
            std::printf("    OrdType:   %u\n", m.ord_type);
            std::printf("    TIF:       %u\n", m.time_in_force);
            std::printf("    SeqNum:    %u\n", m.seq_num);

        } else if constexpr (std::is_same_v<T, DecodedExecTrade525>) {
            const auto& m = decoded.root;
            std::printf("  ExecutionReportTrade525:\n");
            std::printf("    OrderId:   %lu\n", static_cast<unsigned long>(m.order_id));
            std::printf("    ClOrdId:   %.20s\n", m.cl_ord_id);
            std::printf("    Side:      %s\n", side_str(m.side));
            std::printf("    LastPx:    %.4f\n", m.last_px.to_double());
            std::printf("    LastQty:   %u\n", m.last_qty);
            std::printf("    CumQty:    %u\n", m.cum_qty);
            std::printf("    LeavesQty: %u\n", m.leaves_qty);
            std::printf("    OrdStatus: %s\n", ord_status_str(m.ord_status));
            std::printf("    Aggressor: %s\n", m.aggressor_indicator ? "Yes" : "No");

        } else if constexpr (std::is_same_v<T, DecodedExecCancel534>) {
            const auto& m = decoded.root;
            std::printf("  ExecutionReportCancel534:\n");
            std::printf("    OrderId:   %lu\n", static_cast<unsigned long>(m.order_id));
            std::printf("    ClOrdId:   %.20s\n", m.cl_ord_id);
            std::printf("    CumQty:    %u\n", m.cum_qty);
            std::printf("    SeqNum:    %u\n", m.seq_num);

        } else if constexpr (std::is_same_v<T, DecodedExecReject523>) {
            const auto& m = decoded.root;
            std::printf("  ExecutionReportReject523:\n");
            std::printf("    ClOrdId:   %.20s\n", m.cl_ord_id);
            std::printf("    Reason:    %u\n", m.ord_rej_reason);
            std::printf("    SeqNum:    %u\n", m.seq_num);

        } else if constexpr (std::is_same_v<T, DecodedCancelReject535>) {
            const auto& m = decoded.root;
            std::printf("  OrderCancelReject535:\n");
            std::printf("    OrderId:   %lu\n", static_cast<unsigned long>(m.order_id));
            std::printf("    ClOrdId:   %.20s\n", m.cl_ord_id);
            std::printf("    Reason:    %u\n", m.cxl_rej_reason);
            std::printf("    SeqNum:    %u\n", m.seq_num);

        } else {
            std::printf("  (Unexpected response template)\n");
        }
    });

    if (rc != DecodeResult::kOk) {
        std::fprintf(stderr, "  Failed to decode response (error=%d)\n",
                     static_cast<int>(rc));
    }
}

// ---------------------------------------------------------------------------
// Encode context setup
// ---------------------------------------------------------------------------

sbe::ilink3::EncodeContext make_encode_context(
    int32_t security_id, const std::string& account)
{
    sbe::ilink3::EncodeContext ctx{};
    ctx.uuid = 1;
    ctx.seq_num = 1;
    ctx.security_id = security_id;
    size_t copy_len = std::min(account.size(), sizeof(ctx.sender_id) - 1);
    std::memcpy(ctx.sender_id, account.c_str(), copy_len);
    std::memcpy(ctx.location, "US,IL", 5);
    ctx.party_details_list_req_id = 1;
    return ctx;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        print_usage(argv[0]);
        return 1;
    }

    // Resolve instrument symbol to security_id.
    int32_t security_id = resolve_security_id(args.instrument);
    if (security_id < 0) {
        std::fprintf(stderr, "Unknown instrument: %s\n"
                     "Valid symbols: ES, NQ, CL, GC, ZN, ZB, MES, 6E\n",
                     args.instrument.c_str());
        return 1;
    }

    // Validate cancel/replace require --order-id.
    if ((args.command == Command::Cancel || args.command == Command::Replace) &&
        args.order_id == 0) {
        std::fprintf(stderr, "--cancel and --replace require --order-id\n");
        return 1;
    }

    // Connect to cme-sim.
    TcpClient client;
    std::fprintf(stderr, "Connecting to %s:%u...\n",
                 args.host.c_str(), args.port);
    if (!client.connect_to(args.host, args.port)) {
        return 1;
    }
    std::fprintf(stderr, "Connected.\n");

    auto ctx = make_encode_context(security_id, args.account);

    // Encode buffer — large enough for any iLink3 message.
    char send_buf[1024];
    char recv_buf[2048];

    for (int i = 0; i < args.count; ++i) {
        size_t msg_len = 0;
        Timestamp ts = now_ns();
        uint64_t cl_ord_id = args.cl_ord_id + static_cast<uint64_t>(i);

        switch (args.command) {
            case Command::NewOrder: {
                OrderRequest req{};
                req.client_order_id = cl_ord_id;
                req.side = parse_side(args.side_str);
                req.type = parse_order_type(args.type_str);
                req.tif = parse_tif(args.tif_str);
                req.price = decimal_to_engine_price(args.price);
                req.quantity = static_cast<Quantity>(args.qty) * PRICE_SCALE;
                req.timestamp = ts;

                msg_len = sbe::ilink3::encode_new_order(send_buf, req, ctx);

                std::printf("[%d] NewOrderSingle -> %s %s %u@%.4f %s %s\n",
                            i + 1,
                            args.instrument.c_str(),
                            args.side_str.c_str(),
                            args.qty,
                            args.price,
                            args.type_str.c_str(),
                            args.tif_str.c_str());
                break;
            }

            case Command::Cancel: {
                msg_len = sbe::ilink3::encode_cancel_order(
                    send_buf,
                    args.order_id,
                    cl_ord_id,
                    parse_side(args.side_str),
                    ts,
                    ctx);

                std::printf("[%d] CancelRequest -> OrderId=%lu\n",
                            i + 1,
                            static_cast<unsigned long>(args.order_id));
                break;
            }

            case Command::Replace: {
                ModifyRequest req{};
                req.order_id = args.order_id;
                req.client_order_id = cl_ord_id;
                req.new_price = decimal_to_engine_price(args.price);
                req.new_quantity = static_cast<Quantity>(args.qty) * PRICE_SCALE;
                req.timestamp = ts;

                msg_len = sbe::ilink3::encode_modify_order(
                    send_buf, req, args.order_id,
                    parse_side(args.side_str),
                    parse_order_type(args.type_str),
                    parse_tif(args.tif_str),
                    ctx);

                std::printf("[%d] ReplaceRequest -> OrderId=%lu %u@%.4f\n",
                            i + 1,
                            static_cast<unsigned long>(args.order_id),
                            args.qty, args.price);
                break;
            }
        }

        if (msg_len == 0) {
            std::fprintf(stderr, "Encoding failed\n");
            return 1;
        }

        // Send length-prefixed message.
        if (!client.send_message(send_buf, msg_len)) {
            std::fprintf(stderr, "Failed to send message\n");
            return 1;
        }

        // Increment seq_num for the next message.
        ctx.seq_num++;

        // Wait for response(s). An order may generate multiple responses
        // (e.g. Accepted + Fill, or Accepted + multiple PartialFills).
        // Read with a timeout to handle the case where no more responses come.
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        bool got_response = false;
        while (true) {
            ssize_t resp_len = client.recv_message(recv_buf, sizeof(recv_buf));
            if (resp_len <= 0) {
                if (!got_response) {
                    std::fprintf(stderr, "  No response received\n");
                }
                break;
            }
            got_response = true;
            print_response(recv_buf, static_cast<size_t>(resp_len));

            // Check if this is a terminal response (not a partial fill).
            // Peek at template ID to decide.
            if (static_cast<size_t>(resp_len) >= sizeof(sbe::MessageHeader)) {
                sbe::MessageHeader hdr{};
                sbe::MessageHeader::decode_from(recv_buf, hdr);
                // ExecutionReportNew522 may be followed by fills, keep reading.
                // Fill/Cancel/Reject are terminal for the request.
                if (hdr.template_id != sbe::ilink3::EXEC_REPORT_NEW_ID) {
                    break;
                }
                // After New, check if there might be a fill following.
                // Set a short timeout for subsequent reads.
                tv.tv_sec = 0;
                tv.tv_usec = 200000;  // 200ms
                setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO,
                           &tv, sizeof(tv));
            }
        }
    }

    std::fprintf(stderr, "Done. %d message(s) sent.\n", args.count);
    return 0;
}
