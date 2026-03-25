#include "tools/pcap_reader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace exchange {
namespace {

// --- Helpers to build synthetic pcap files in memory ---

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

std::vector<uint8_t> MakePcapGlobalHeader(uint32_t link_type = 1) {
    std::vector<uint8_t> h;
    AppendU32LE(h, 0xa1b2c3d4);  // Magic (native, microsecond).
    AppendU16LE(h, 2); AppendU16LE(h, 4);  // Version.
    AppendU32LE(h, 0); AppendU32LE(h, 0);  // Thiszone, sigfigs.
    AppendU32LE(h, 65535);        // Snaplen.
    AppendU32LE(h, link_type);
    return h;
}

std::vector<uint8_t> MakeUdpFrame(uint16_t src_port, uint16_t dst_port,
                                   const std::vector<uint8_t>& payload,
                                   uint32_t src_ip = 0x0a000001,
                                   uint32_t dst_ip = 0xe0001f01) {
    std::vector<uint8_t> frame;
    uint8_t eth[14] = {};
    eth[12] = 0x08; eth[13] = 0x00;
    Append(frame, eth, 14);

    uint16_t total_len = static_cast<uint16_t>(20 + 8 + payload.size());
    uint8_t ip[20] = {};
    ip[0] = 0x45;
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xFF);
    ip[8] = 64; ip[9] = 17;
    std::memcpy(ip + 12, &src_ip, 4);
    std::memcpy(ip + 16, &dst_ip, 4);
    Append(frame, ip, 20);

    uint16_t udp_len = static_cast<uint16_t>(8 + payload.size());
    AppendU16BE(frame, src_port);
    AppendU16BE(frame, dst_port);
    AppendU16BE(frame, udp_len);
    AppendU16BE(frame, 0);
    Append(frame, payload.data(), payload.size());
    return frame;
}

std::vector<uint8_t> MakeTcpFrame(uint16_t src_port, uint16_t dst_port,
                                   const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    uint8_t eth[14] = {};
    eth[12] = 0x08; eth[13] = 0x00;
    Append(frame, eth, 14);

    uint16_t total_len = static_cast<uint16_t>(20 + 20 + payload.size());
    uint8_t ip[20] = {};
    ip[0] = 0x45;
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xFF);
    ip[8] = 64; ip[9] = 6;
    uint32_t src_ip = 0x0a000001, dst_ip = 0x0a000002;
    std::memcpy(ip + 12, &src_ip, 4);
    std::memcpy(ip + 16, &dst_ip, 4);
    Append(frame, ip, 20);

    AppendU16BE(frame, src_port);
    AppendU16BE(frame, dst_port);
    AppendU32LE(frame, 1);  // Seq.
    AppendU32LE(frame, 0);  // Ack.
    uint8_t tcp_misc[8] = {};
    tcp_misc[0] = 0x50;     // Data offset = 5.
    Append(frame, tcp_misc, 8);
    Append(frame, payload.data(), payload.size());
    return frame;
}

void AppendPcapPacket(std::vector<uint8_t>& file,
                      const std::vector<uint8_t>& frame,
                      uint32_t ts_sec = 1000, uint32_t ts_usec = 500) {
    uint32_t len = static_cast<uint32_t>(frame.size());
    AppendU32LE(file, ts_sec);
    AppendU32LE(file, ts_usec);
    AppendU32LE(file, len);
    AppendU32LE(file, len);
    Append(file, frame.data(), frame.size());
}

std::string WriteTempFile(const std::vector<uint8_t>& data,
                          const std::string& suffix) {
    std::string path = "/tmp/pcap_reader_test_" + suffix;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.close();
    return path;
}

std::vector<uint8_t> MakePcapngFile(
    const std::vector<std::vector<uint8_t>>& frames,
    uint32_t link_type = 1) {
    std::vector<uint8_t> file;

    // SHB (28 bytes).
    uint32_t shb_len = 28;
    AppendU32LE(file, 0x0a0d0d0a);
    AppendU32LE(file, shb_len);
    AppendU32LE(file, 0x1a2b3c4d);
    AppendU16LE(file, 1); AppendU16LE(file, 0);
    uint64_t section_len = 0xFFFFFFFFFFFFFFFF;
    Append(file, &section_len, 8);
    AppendU32LE(file, shb_len);

    // IDB (20 bytes).
    uint32_t idb_len = 20;
    AppendU32LE(file, 1);
    AppendU32LE(file, idb_len);
    AppendU16LE(file, static_cast<uint16_t>(link_type));
    AppendU16LE(file, 0);
    AppendU32LE(file, 65535);
    AppendU32LE(file, idb_len);

    // EPBs.
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        uint32_t cap_len = static_cast<uint32_t>(f.size());
        uint32_t padded = (cap_len + 3) & ~3u;
        uint32_t epb_len = 32 + padded;
        AppendU32LE(file, 6);
        AppendU32LE(file, epb_len);
        AppendU32LE(file, 0);
        // Timestamp: (i+1) seconds in microseconds.
        uint64_t ts = static_cast<uint64_t>(i + 1) * 1000000;
        AppendU32LE(file, static_cast<uint32_t>(ts >> 32));
        AppendU32LE(file, static_cast<uint32_t>(ts & 0xFFFFFFFF));
        AppendU32LE(file, cap_len);
        AppendU32LE(file, cap_len);
        Append(file, f.data(), f.size());
        uint8_t pad[4] = {};
        if (padded > cap_len) Append(file, pad, padded - cap_len);
        AppendU32LE(file, epb_len);
    }

    return file;
}

// Helper: compare payload pointer against expected bytes.
void ExpectPayload(const PcapPacket& pkt, const std::vector<uint8_t>& expected) {
    ASSERT_EQ(pkt.payload_len, expected.size());
    EXPECT_EQ(std::memcmp(pkt.payload, expected.data(), expected.size()), 0);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(PcapReaderTest, ClassicPcapSingleUdpPacket) {
    std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    auto frame = MakeUdpFrame(12345, 14310, payload);

    auto file = MakePcapGlobalHeader();
    AppendPcapPacket(file, frame, 1700000000, 42);

    auto path = WriteTempFile(file, "single_udp.pcap");
    PcapReader reader(path);
    EXPECT_EQ(reader.format(), PcapReader::Format::kClassicPcap);
    EXPECT_EQ(reader.link_type(), 1u);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    EXPECT_EQ(pkt.proto, TransportProto::kUdp);
    EXPECT_EQ(pkt.src_port, 12345u);
    EXPECT_EQ(pkt.dst_port, 14310u);
    // 1700000000 sec + 42 usec = 1700000000000042000 ns.
    EXPECT_EQ(pkt.ts_ns, 1700000000ULL * 1000000000 + 42 * 1000);
    ExpectPayload(pkt, payload);

    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, ClassicPcapTcpPacket) {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    auto frame = MakeTcpFrame(8080, 443, payload);

    auto file = MakePcapGlobalHeader();
    AppendPcapPacket(file, frame);

    auto path = WriteTempFile(file, "tcp.pcap");
    PcapReader reader(path);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    EXPECT_EQ(pkt.proto, TransportProto::kTcp);
    EXPECT_EQ(pkt.src_port, 8080u);
    EXPECT_EQ(pkt.dst_port, 443u);
    ExpectPayload(pkt, payload);

    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, ClassicPcapMultiplePackets) {
    auto file = MakePcapGlobalHeader();
    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> payload = {static_cast<uint8_t>(i)};
        auto frame = MakeUdpFrame(1000 + i, 2000 + i, payload);
        AppendPcapPacket(file, frame, static_cast<uint32_t>(i), 0);
    }

    auto path = WriteTempFile(file, "multi.pcap");
    PcapReader reader(path);

    for (int i = 0; i < kCount; ++i) {
        PcapPacket pkt;
        ASSERT_TRUE(reader.next(pkt)) << "failed at packet " << i;
        EXPECT_EQ(pkt.src_port, static_cast<uint16_t>(1000 + i));
        EXPECT_EQ(pkt.dst_port, static_cast<uint16_t>(2000 + i));
        ASSERT_EQ(pkt.payload_len, 1u);
        EXPECT_EQ(pkt.payload[0], static_cast<uint8_t>(i));
    }

    PcapPacket eof;
    EXPECT_FALSE(reader.next(eof));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, PcapngSingleUdpPacket) {
    std::vector<uint8_t> payload = {'p', 'c', 'a', 'p', 'n', 'g'};
    auto frame = MakeUdpFrame(5555, 6666, payload);
    auto file = MakePcapngFile({frame});
    auto path = WriteTempFile(file, "single.pcapng");

    PcapReader reader(path);
    EXPECT_EQ(reader.format(), PcapReader::Format::kPcapng);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    EXPECT_EQ(pkt.proto, TransportProto::kUdp);
    EXPECT_EQ(pkt.src_port, 5555u);
    EXPECT_EQ(pkt.dst_port, 6666u);
    ExpectPayload(pkt, payload);
    // 1 second = 1000000 us -> 1000000000 ns.
    EXPECT_EQ(pkt.ts_ns, 1000000000ULL);

    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, PcapngMultiplePackets) {
    std::vector<std::vector<uint8_t>> frames;
    constexpr int kCount = 3;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> payload(10, static_cast<uint8_t>(0x30 + i));
        frames.push_back(MakeUdpFrame(7000 + i, 8000 + i, payload));
    }

    auto file = MakePcapngFile(frames);
    auto path = WriteTempFile(file, "multi.pcapng");
    PcapReader reader(path);

    for (int i = 0; i < kCount; ++i) {
        PcapPacket pkt;
        ASSERT_TRUE(reader.next(pkt)) << "packet " << i;
        EXPECT_EQ(pkt.src_port, static_cast<uint16_t>(7000 + i));
        EXPECT_EQ(pkt.dst_port, static_cast<uint16_t>(8000 + i));
        ASSERT_EQ(pkt.payload_len, 10u);
        EXPECT_EQ(pkt.payload[0], static_cast<uint8_t>(0x30 + i));
    }

    PcapPacket eof;
    EXPECT_FALSE(reader.next(eof));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, PayloadIntegrity256Bytes) {
    std::vector<uint8_t> payload(256);
    for (int i = 0; i < 256; ++i) payload[i] = static_cast<uint8_t>(i);

    auto frame = MakeUdpFrame(9000, 9001, payload);
    auto file = MakePcapGlobalHeader();
    AppendPcapPacket(file, frame);

    auto path = WriteTempFile(file, "integrity.pcap");
    PcapReader reader(path);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    ExpectPayload(pkt, payload);
    std::remove(path.c_str());
}

TEST(PcapReaderTest, InvalidFileThrows) {
    EXPECT_THROW(
        { PcapReader r("/tmp/nonexistent_file_xyz.pcap"); },
        std::runtime_error);

    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF};
    auto path = WriteTempFile(garbage, "garbage.pcap");
    EXPECT_THROW(
        { PcapReader r(path); },
        std::runtime_error);
    std::remove(path.c_str());
}

TEST(PcapReaderTest, EmptyPcapReturnsEof) {
    auto file = MakePcapGlobalHeader();
    auto path = WriteTempFile(file, "empty.pcap");

    PcapReader reader(path);
    PcapPacket pkt;
    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, MixedTcpAndUdp) {
    auto file = MakePcapGlobalHeader();

    std::vector<uint8_t> udp_payload = {'U'};
    std::vector<uint8_t> tcp_payload = {'T'};
    auto udp_frame = MakeUdpFrame(1111, 2222, udp_payload);
    auto tcp_frame = MakeTcpFrame(3333, 4444, tcp_payload);

    AppendPcapPacket(file, udp_frame, 10, 0);
    AppendPcapPacket(file, tcp_frame, 20, 0);

    auto path = WriteTempFile(file, "mixed.pcap");
    PcapReader reader(path);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    EXPECT_EQ(pkt.proto, TransportProto::kUdp);
    ExpectPayload(pkt, udp_payload);

    ASSERT_TRUE(reader.next(pkt));
    EXPECT_EQ(pkt.proto, TransportProto::kTcp);
    ExpectPayload(pkt, tcp_payload);

    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, ForEachPacketCallback) {
    auto file = MakePcapGlobalHeader();
    constexpr int kCount = 4;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> payload = {static_cast<uint8_t>(0xA0 + i)};
        auto frame = MakeUdpFrame(3000 + i, 4000 + i, payload);
        AppendPcapPacket(file, frame, static_cast<uint32_t>(i), 0);
    }

    auto path = WriteTempFile(file, "foreach.pcap");
    int count = 0;
    forEachPacket(path, [&](const PcapPacket& pkt) {
        EXPECT_EQ(pkt.src_port, static_cast<uint16_t>(3000 + count));
        ASSERT_EQ(pkt.payload_len, 1u);
        EXPECT_EQ(pkt.payload[0], static_cast<uint8_t>(0xA0 + count));
        ++count;
    });
    EXPECT_EQ(count, kCount);
    std::remove(path.c_str());
}

TEST(PcapReaderTest, ResetRestartsIteration) {
    std::vector<uint8_t> payload = {'R'};
    auto frame = MakeUdpFrame(5000, 6000, payload);
    auto file = MakePcapGlobalHeader();
    AppendPcapPacket(file, frame);

    auto path = WriteTempFile(file, "reset.pcap");
    PcapReader reader(path);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    EXPECT_FALSE(reader.next(pkt));

    reader.reset();
    ASSERT_TRUE(reader.next(pkt));
    ExpectPayload(pkt, payload);
    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

TEST(PcapReaderTest, ZeroCopyPayloadPointsIntoMmap) {
    std::vector<uint8_t> payload = {0xDE, 0xAD};
    auto frame = MakeUdpFrame(1234, 5678, payload);
    auto file = MakePcapGlobalHeader();
    AppendPcapPacket(file, frame);

    auto path = WriteTempFile(file, "zerocopy.pcap");
    PcapReader reader(path);

    PcapPacket pkt;
    ASSERT_TRUE(reader.next(pkt));
    // Payload pointer should be non-null and point to valid data
    // without any heap allocation.
    ASSERT_NE(pkt.payload, nullptr);
    EXPECT_EQ(pkt.payload[0], 0xDE);
    EXPECT_EQ(pkt.payload[1], 0xAD);
    EXPECT_EQ(pkt.payload_len, 2u);
    std::remove(path.c_str());
}

}  // namespace
}  // namespace exchange
