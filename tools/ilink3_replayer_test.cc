#include "tools/ilink3_replayer.h"

#include "cme/codec/ilink3_encoder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace exchange {
namespace {

using namespace cme::sbe;
using namespace cme::sbe::ilink3;

// --- Pcap construction helpers ---

void Append(std::vector<uint8_t>& v, const void* data, size_t n) {
    auto* p = reinterpret_cast<const uint8_t*>(data);
    v.insert(v.end(), p, p + n);
}

void AppendU32LE(std::vector<uint8_t>& v, uint32_t val) { Append(v, &val, 4); }
void AppendU16LE(std::vector<uint8_t>& v, uint16_t val) { Append(v, &val, 2); }

std::vector<uint8_t> MakePcapGlobalHeader() {
    std::vector<uint8_t> h;
    AppendU32LE(h, 0xa1b2c3d4);
    AppendU16LE(h, 2); AppendU16LE(h, 4);
    AppendU32LE(h, 0); AppendU32LE(h, 0);
    AppendU32LE(h, 65535);
    AppendU32LE(h, 1);  // Ethernet.
    return h;
}

// Build Ethernet + IPv4 + TCP frame wrapping the given payload.
std::vector<uint8_t> MakeTcpFrame(uint16_t src_port, uint16_t dst_port,
                                   const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> frame;

    // Ethernet header (14 bytes).
    uint8_t eth[14] = {};
    eth[12] = 0x08; eth[13] = 0x00;
    Append(frame, eth, 14);

    // IPv4 header (20 bytes).
    uint16_t total_len = static_cast<uint16_t>(20 + 20 + payload_len);
    uint8_t ip[20] = {};
    ip[0] = 0x45;
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xFF);
    ip[8] = 64; ip[9] = 6;  // TCP.
    uint32_t src_ip = 0x0a000001, dst_ip = 0x0a000002;
    std::memcpy(ip + 12, &src_ip, 4);
    std::memcpy(ip + 16, &dst_ip, 4);
    Append(frame, ip, 20);

    // TCP header (20 bytes, no options).
    uint8_t tcp[20] = {};
    tcp[0] = static_cast<uint8_t>(src_port >> 8);
    tcp[1] = static_cast<uint8_t>(src_port & 0xFF);
    tcp[2] = static_cast<uint8_t>(dst_port >> 8);
    tcp[3] = static_cast<uint8_t>(dst_port & 0xFF);
    tcp[12] = 0x50;  // Data offset = 5 words (20 bytes).
    Append(frame, tcp, 20);

    Append(frame, payload, payload_len);
    return frame;
}

void AppendPcapPacket(std::vector<uint8_t>& file,
                      const std::vector<uint8_t>& frame,
                      uint32_t ts_sec = 1000) {
    uint32_t len = static_cast<uint32_t>(frame.size());
    AppendU32LE(file, ts_sec);
    AppendU32LE(file, 0);
    AppendU32LE(file, len);
    AppendU32LE(file, len);
    Append(file, frame.data(), frame.size());
}

std::string WriteTempFile(const std::vector<uint8_t>& data,
                          const std::string& suffix) {
    std::string path = "/tmp/ilink3_replayer_test_" + suffix;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.close();
    return path;
}

// Wrap SBE bytes in a 4-byte LE length prefix (TCP framing).
std::vector<uint8_t> WithLengthPrefix(const char* sbe_buf, size_t sbe_len) {
    std::vector<uint8_t> out;
    uint32_t len = static_cast<uint32_t>(sbe_len);
    Append(out, &len, 4);
    Append(out, sbe_buf, sbe_len);
    return out;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(Ilink3ReplayerTest, ReplayNewOrderAndCancel) {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.uuid = 12345;
    ctx.security_id = 100;
    std::memcpy(ctx.sender_id, "SENDER", 6);
    std::memcpy(ctx.location, "US,IL", 5);

    constexpr uint16_t kPort = 9100;
    auto file = MakePcapGlobalHeader();

    // Packet 0: NewOrderSingle514.
    {
        OrderRequest req{};
        req.client_order_id = 1;
        req.side = Side::Buy;
        req.type = OrderType::Limit;
        req.tif = exchange::TimeInForce::GTC;
        req.price = 45000000;
        req.quantity = 10000;
        req.timestamp = 1000;

        char sbe[MAX_ENCODED_SIZE];
        size_t n = encode_new_order(sbe, req, ctx);
        auto payload = WithLengthPrefix(sbe, n);
        auto frame = MakeTcpFrame(5000, kPort, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 1);
    }

    // Packet 1: OrderCancelRequest516.
    {
        ctx.seq_num = 2;
        char sbe[MAX_ENCODED_SIZE];
        size_t n = encode_cancel_order(sbe, 1, 2, Side::Buy, 2000, ctx);
        auto payload = WithLengthPrefix(sbe, n);
        auto frame = MakeTcpFrame(5000, kPort, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 2);
    }

    auto path = WriteTempFile(file, "new_cancel");
    auto stats = replay_ilink3_pcap(path, kPort, [](auto&) {});

    EXPECT_EQ(stats.total_tcp_packets, 2u);
    EXPECT_EQ(stats.decoded_ok, 2u);
    EXPECT_EQ(stats.decode_errors, 0u);
    EXPECT_EQ(stats.new_order_514, 1u);
    EXPECT_EQ(stats.cancel_516, 1u);

    std::remove(path.c_str());
}

TEST(Ilink3ReplayerTest, ReplayWithVisitor) {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.security_id = 100;

    constexpr uint16_t kPort = 9100;
    auto file = MakePcapGlobalHeader();

    OrderRequest req{};
    req.client_order_id = 42;
    req.side = Side::Sell;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::DAY;
    req.price = 50000000;
    req.quantity = 50000;
    req.timestamp = 3000;

    char sbe[MAX_ENCODED_SIZE];
    size_t n = encode_new_order(sbe, req, ctx);
    auto payload = WithLengthPrefix(sbe, n);
    auto frame = MakeTcpFrame(5000, kPort, payload.data(), payload.size());
    AppendPcapPacket(file, frame, 3);

    auto path = WriteTempFile(file, "visitor");

    Price decoded_price = 0;
    uint8_t decoded_side = 0;
    replay_ilink3_pcap(path, kPort, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedNewOrder514>) {
            decoded_price = price9_to_engine(msg.root.price);
            decoded_side = msg.root.side;
        }
    });

    EXPECT_EQ(decoded_price, 50000000);
    EXPECT_EQ(decoded_side, encode_side(Side::Sell));

    std::remove(path.c_str());
}

TEST(Ilink3ReplayerTest, ReplayExecReports) {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.uuid = 999;
    ctx.security_id = 100;

    constexpr uint16_t kPort = 9100;
    auto file = MakePcapGlobalHeader();

    // ExecutionReportNew522.
    {
        OrderAccepted evt{1, 10, 1000};
        Order order{};
        order.price = 45000000;
        order.quantity = 10000;
        order.side = Side::Buy;
        order.type = OrderType::Limit;
        order.tif = exchange::TimeInForce::GTC;

        char sbe[MAX_ENCODED_SIZE];
        size_t n = encode_exec_new(sbe, evt, order, ctx);
        auto payload = WithLengthPrefix(sbe, n);
        auto frame = MakeTcpFrame(kPort, 5000, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 1);
    }

    // ExecutionReportCancel534.
    {
        ctx.seq_num = 2;
        OrderCancelled evt{1, 2000, CancelReason::UserRequested};
        Order order{};
        order.client_order_id = 10;
        order.price = 45000000;
        order.quantity = 10000;
        order.side = Side::Buy;
        order.type = OrderType::Limit;
        order.tif = exchange::TimeInForce::GTC;

        char sbe[MAX_ENCODED_SIZE];
        size_t n = encode_exec_cancel(sbe, evt, order, ctx);
        auto payload = WithLengthPrefix(sbe, n);
        auto frame = MakeTcpFrame(kPort, 5000, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 2);
    }

    auto path = WriteTempFile(file, "exec_reports");

    // Use dst_port=5000 (the client port) to capture exchange->client messages.
    auto stats = replay_ilink3_pcap(path, 5000, [](auto&) {});

    EXPECT_EQ(stats.total_tcp_packets, 2u);
    EXPECT_EQ(stats.decoded_ok, 2u);
    EXPECT_EQ(stats.exec_new_522, 1u);
    EXPECT_EQ(stats.exec_cancel_534, 1u);

    std::remove(path.c_str());
}

TEST(Ilink3ReplayerTest, PortFilterSkipsOtherPorts) {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.security_id = 100;

    auto file = MakePcapGlobalHeader();

    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::GTC;
    req.price = 45000000;
    req.quantity = 10000;
    req.timestamp = 1000;

    char sbe[MAX_ENCODED_SIZE];
    size_t n = encode_new_order(sbe, req, ctx);
    auto payload = WithLengthPrefix(sbe, n);
    auto frame = MakeTcpFrame(5000, 9100, payload.data(), payload.size());
    AppendPcapPacket(file, frame, 1);

    auto path = WriteTempFile(file, "port_filter");

    // Filter on different port — should see 0 decoded messages.
    auto stats = replay_ilink3_pcap(path, 8888, [](auto&) {});
    EXPECT_EQ(stats.total_tcp_packets, 0u);
    EXPECT_EQ(stats.decoded_ok, 0u);

    // Filter on correct port — should see 1.
    stats = replay_ilink3_pcap(path, 9100, [](auto&) {});
    EXPECT_EQ(stats.total_tcp_packets, 1u);
    EXPECT_EQ(stats.decoded_ok, 1u);

    // Port 0 = accept all.
    stats = replay_ilink3_pcap(path, 0, [](auto&) {});
    EXPECT_EQ(stats.decoded_ok, 1u);

    std::remove(path.c_str());
}

TEST(Ilink3ReplayerTest, StatsOnlyOverload) {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.security_id = 100;

    auto file = MakePcapGlobalHeader();

    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::GTC;
    req.price = 45000000;
    req.quantity = 10000;
    req.timestamp = 1000;

    char sbe[MAX_ENCODED_SIZE];
    size_t n = encode_new_order(sbe, req, ctx);
    auto payload = WithLengthPrefix(sbe, n);
    auto frame = MakeTcpFrame(5000, 9100, payload.data(), payload.size());
    AppendPcapPacket(file, frame, 1);

    auto path = WriteTempFile(file, "stats_only");
    auto stats = replay_ilink3_pcap(path, 9100);
    EXPECT_EQ(stats.decoded_ok, 1u);
    EXPECT_EQ(stats.new_order_514, 1u);

    std::remove(path.c_str());
}

TEST(Ilink3ReplayerTest, NoLengthPrefixMode) {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.security_id = 100;

    auto file = MakePcapGlobalHeader();

    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::GTC;
    req.price = 45000000;
    req.quantity = 10000;
    req.timestamp = 1000;

    char sbe[MAX_ENCODED_SIZE];
    size_t n = encode_new_order(sbe, req, ctx);
    // No length prefix — raw SBE payload.
    std::vector<uint8_t> payload;
    Append(payload, sbe, n);
    auto frame = MakeTcpFrame(5000, 9100, payload.data(), payload.size());
    AppendPcapPacket(file, frame, 1);

    auto path = WriteTempFile(file, "no_prefix");
    auto stats = replay_ilink3_pcap(path, 9100, [](auto&) {},
                                     /*strip_length_prefix=*/false);
    EXPECT_EQ(stats.decoded_ok, 1u);
    EXPECT_EQ(stats.new_order_514, 1u);

    std::remove(path.c_str());
}

}  // namespace
}  // namespace exchange
