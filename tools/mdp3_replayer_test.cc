#include "tools/mdp3_replayer.h"

#include "cme/codec/mdp3_encoder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace exchange {
namespace {

using namespace cme::sbe;
using namespace cme::sbe::mdp3;

// --- Pcap construction helpers (matching pcap_reader_test.cc conventions) ---

void Append(std::vector<uint8_t>& v, const void* data, size_t n) {
    auto* p = reinterpret_cast<const uint8_t*>(data);
    v.insert(v.end(), p, p + n);
}

void AppendU16BE(std::vector<uint8_t>& v, uint16_t val) {
    uint8_t buf[2] = {static_cast<uint8_t>(val >> 8),
                      static_cast<uint8_t>(val & 0xFF)};
    Append(v, buf, 2);
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

// Build Ethernet + IPv4 + UDP frame wrapping the given payload.
std::vector<uint8_t> MakeUdpFrame(uint16_t src_port, uint16_t dst_port,
                                   const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> frame;

    // Ethernet header (14 bytes).
    uint8_t eth[14] = {};
    eth[12] = 0x08; eth[13] = 0x00;
    Append(frame, eth, 14);

    // IPv4 header (20 bytes).
    uint16_t total_len = static_cast<uint16_t>(20 + 8 + payload_len);
    uint8_t ip[20] = {};
    ip[0] = 0x45;
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xFF);
    ip[8] = 64; ip[9] = 17;  // UDP.
    uint32_t src_ip = 0x0a000001, dst_ip = 0xe0001f01;
    std::memcpy(ip + 12, &src_ip, 4);
    std::memcpy(ip + 16, &dst_ip, 4);
    Append(frame, ip, 20);

    // UDP header (8 bytes).
    uint16_t udp_len = static_cast<uint16_t>(8 + payload_len);
    AppendU16BE(frame, src_port);
    AppendU16BE(frame, dst_port);
    AppendU16BE(frame, udp_len);
    AppendU16BE(frame, 0);

    Append(frame, payload, payload_len);
    return frame;
}

void AppendPcapPacket(std::vector<uint8_t>& file,
                      const std::vector<uint8_t>& frame,
                      uint32_t ts_sec = 1000) {
    uint32_t len = static_cast<uint32_t>(frame.size());
    AppendU32LE(file, ts_sec);
    AppendU32LE(file, 0);  // ts_usec.
    AppendU32LE(file, len);
    AppendU32LE(file, len);
    Append(file, frame.data(), frame.size());
}

std::string WriteTempFile(const std::vector<uint8_t>& data,
                          const std::string& suffix) {
    std::string path = "/tmp/mdp3_replayer_test_" + suffix;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.close();
    return path;
}

// Encode an MDP3 message with a McastSeqHeader prepended.
std::vector<uint8_t> EncodeWithSeqHeader(const char* sbe_buf, size_t sbe_len,
                                          uint32_t seq) {
    std::vector<uint8_t> out;
    McastSeqHeader hdr{seq};
    Append(out, &hdr, sizeof(hdr));
    Append(out, sbe_buf, sbe_len);
    return out;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(Mdp3ReplayerTest, ReplayMixedMessages) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 12345;
    std::memcpy(ctx.security_group, "ES    ", 6);
    std::memcpy(ctx.asset, "ES    ", 6);

    constexpr uint16_t kPort = 14310;
    auto file = MakePcapGlobalHeader();

    // Packet 0: DepthUpdate -> RefreshBook46.
    {
        char sbe[MAX_MDP3_ENCODED_SIZE];
        DepthUpdate evt{};
        evt.side = Side::Buy;
        evt.price = 45000000;
        evt.total_qty = 100000;
        evt.order_count = 5;
        evt.action = DepthUpdate::Add;
        evt.ts = 1000;
        size_t n = encode_depth_update(sbe, evt, ctx);
        auto payload = EncodeWithSeqHeader(sbe, n, 0);
        auto frame = MakeUdpFrame(5000, kPort, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 1);
    }

    // Packet 1: Trade -> TradeSummary48.
    {
        char sbe[MAX_MDP3_ENCODED_SIZE];
        Trade evt{};
        evt.price = 45005000;
        evt.quantity = 30000;
        evt.aggressor_side = Side::Sell;
        evt.ts = 2000;
        size_t n = encode_trade(sbe, evt, ctx);
        auto payload = EncodeWithSeqHeader(sbe, n, 1);
        auto frame = MakeUdpFrame(5000, kPort, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 2);
    }

    // Packet 2: MarketStatus -> SecurityStatus30.
    {
        char sbe[MAX_MDP3_ENCODED_SIZE];
        MarketStatus evt{};
        evt.state = SessionState::Continuous;
        evt.ts = 3000;
        size_t n = encode_market_status(sbe, evt, ctx);
        auto payload = EncodeWithSeqHeader(sbe, n, 2);
        auto frame = MakeUdpFrame(5000, kPort, payload.data(), payload.size());
        AppendPcapPacket(file, frame, 3);
    }

    auto path = WriteTempFile(file, "mixed.pcap");

    // Replay and verify.
    int book_count = 0, trade_count = 0, status_count = 0;
    auto stats = replay_mdp3_pcap(path, kPort, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedRefreshBook46>) {
            ++book_count;
            EXPECT_EQ(msg.num_md_entries, 1u);
            EXPECT_DOUBLE_EQ(msg.md_entries[0].md_entry_px.to_double(), 4500.0);
        } else if constexpr (std::is_same_v<T, DecodedTradeSummary48>) {
            ++trade_count;
            EXPECT_DOUBLE_EQ(msg.md_entries[0].md_entry_px.to_double(), 4500.5);
        } else if constexpr (std::is_same_v<T, DecodedSecurityStatus30>) {
            ++status_count;
            EXPECT_EQ(msg.root.security_trading_status,
                      static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade));
        }
    });

    EXPECT_EQ(stats.total_udp_packets, 3u);
    EXPECT_EQ(stats.decoded_ok, 3u);
    EXPECT_EQ(stats.decode_errors, 0u);
    EXPECT_EQ(stats.seq_gaps, 0u);
    EXPECT_EQ(stats.refresh_book_46, 1u);
    EXPECT_EQ(stats.trade_summary_48, 1u);
    EXPECT_EQ(stats.security_status_30, 1u);
    EXPECT_EQ(book_count, 1);
    EXPECT_EQ(trade_count, 1);
    EXPECT_EQ(status_count, 1);
    std::remove(path.c_str());
}

TEST(Mdp3ReplayerTest, SequenceGapDetection) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 1;
    constexpr uint16_t kPort = 14310;
    auto file = MakePcapGlobalHeader();

    // Send seq 0, then seq 5 (gap of 4).
    for (uint32_t seq : {0u, 5u}) {
        char sbe[MAX_MDP3_ENCODED_SIZE];
        MarketStatus evt{};
        evt.state = SessionState::Closed;
        evt.ts = seq;
        size_t n = encode_market_status(sbe, evt, ctx);
        auto payload = EncodeWithSeqHeader(sbe, n, seq);
        auto frame = MakeUdpFrame(5000, kPort, payload.data(), payload.size());
        AppendPcapPacket(file, frame);
    }

    auto path = WriteTempFile(file, "gap.pcap");
    auto stats = replay_mdp3_pcap(path, kPort);

    EXPECT_EQ(stats.total_udp_packets, 2u);
    EXPECT_EQ(stats.decoded_ok, 2u);
    EXPECT_EQ(stats.seq_gaps, 1u);  // Gap between seq 0 and seq 5.
    std::remove(path.c_str());
}

TEST(Mdp3ReplayerTest, PortFiltering) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 1;
    auto file = MakePcapGlobalHeader();

    // Two packets: one on port 14310, one on port 9999.
    for (uint16_t port : {14310, 9999}) {
        char sbe[MAX_MDP3_ENCODED_SIZE];
        MarketStatus evt{};
        evt.state = SessionState::Closed;
        evt.ts = 1;
        size_t n = encode_market_status(sbe, evt, ctx);
        auto payload = EncodeWithSeqHeader(sbe, n, 0);
        auto frame = MakeUdpFrame(5000, port, payload.data(), payload.size());
        AppendPcapPacket(file, frame);
    }

    auto path = WriteTempFile(file, "filter.pcap");
    auto stats = replay_mdp3_pcap(path, 14310);

    EXPECT_EQ(stats.total_udp_packets, 1u);
    EXPECT_EQ(stats.decoded_ok, 1u);
    std::remove(path.c_str());
}

TEST(Mdp3ReplayerTest, StatsOnlyOverload) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 1;
    auto file = MakePcapGlobalHeader();

    char sbe[MAX_MDP3_ENCODED_SIZE];
    DepthUpdate evt{};
    evt.ts = 1;
    size_t n = encode_depth_update(sbe, evt, ctx);
    auto payload = EncodeWithSeqHeader(sbe, n, 0);
    auto frame = MakeUdpFrame(5000, 14310, payload.data(), payload.size());
    AppendPcapPacket(file, frame);

    auto path = WriteTempFile(file, "stats.pcap");
    auto stats = replay_mdp3_pcap(path);

    EXPECT_EQ(stats.total_udp_packets, 1u);
    EXPECT_EQ(stats.decoded_ok, 1u);
    EXPECT_EQ(stats.refresh_book_46, 1u);
    std::remove(path.c_str());
}

TEST(Mdp3ReplayerTest, EmptyPcap) {
    auto file = MakePcapGlobalHeader();
    auto path = WriteTempFile(file, "empty.pcap");
    auto stats = replay_mdp3_pcap(path);

    EXPECT_EQ(stats.total_udp_packets, 0u);
    EXPECT_EQ(stats.decoded_ok, 0u);
    EXPECT_EQ(stats.decode_errors, 0u);
    std::remove(path.c_str());
}

TEST(Mdp3ReplayerTest, NoSeqHeaderMode) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 1;
    auto file = MakePcapGlobalHeader();

    // Send raw SBE without McastSeqHeader.
    char sbe[MAX_MDP3_ENCODED_SIZE];
    MarketStatus evt{};
    evt.state = SessionState::Continuous;
    evt.ts = 42;
    size_t n = encode_market_status(sbe, evt, ctx);
    auto frame = MakeUdpFrame(5000, 14310,
                               reinterpret_cast<const uint8_t*>(sbe), n);
    AppendPcapPacket(file, frame);

    auto path = WriteTempFile(file, "noseq.pcap");
    auto stats = replay_mdp3_pcap(path, 14310, false);  // strip_seq_header=false

    EXPECT_EQ(stats.total_udp_packets, 1u);
    EXPECT_EQ(stats.decoded_ok, 1u);
    EXPECT_EQ(stats.security_status_30, 1u);
    std::remove(path.c_str());
}

}  // namespace
}  // namespace exchange
