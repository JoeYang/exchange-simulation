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

// Append bytes to a vector.
void Append(std::vector<uint8_t>& v, const void* data, size_t n) {
    auto* p = reinterpret_cast<const uint8_t*>(data);
    v.insert(v.end(), p, p + n);
}

void AppendU16BE(std::vector<uint8_t>& v, uint16_t val) {
    uint8_t buf[2] = {static_cast<uint8_t>(val >> 8),
                      static_cast<uint8_t>(val & 0xFF)};
    Append(v, buf, 2);
}

void AppendU32LE(std::vector<uint8_t>& v, uint32_t val) {
    Append(v, &val, 4);
}

void AppendU16LE(std::vector<uint8_t>& v, uint16_t val) {
    Append(v, &val, 2);
}

// Build a classic pcap global header (24 bytes).
std::vector<uint8_t> MakePcapGlobalHeader(uint32_t link_type = 1) {
    std::vector<uint8_t> h;
    AppendU32LE(h, 0xa1b2c3d4);  // Magic (native byte order, microsecond).
    AppendU16LE(h, 2);            // Version major.
    AppendU16LE(h, 4);            // Version minor.
    AppendU32LE(h, 0);            // Thiszone.
    AppendU32LE(h, 0);            // Sigfigs.
    AppendU32LE(h, 65535);        // Snaplen.
    AppendU32LE(h, link_type);    // Link type (1 = Ethernet).
    return h;
}

// Build an Ethernet + IPv4 + UDP frame.
// payload: the UDP payload bytes.
std::vector<uint8_t> MakeUdpFrame(uint16_t src_port, uint16_t dst_port,
                                   const std::vector<uint8_t>& payload,
                                   uint32_t src_ip = 0x0a000001,
                                   uint32_t dst_ip = 0xe0001f01) {
    std::vector<uint8_t> frame;

    // Ethernet header (14 bytes): dst_mac(6) + src_mac(6) + ethertype(2).
    uint8_t eth[14] = {};
    eth[12] = 0x08; eth[13] = 0x00;  // IPv4.
    Append(frame, eth, 14);

    // IPv4 header (20 bytes, no options).
    uint16_t total_len = static_cast<uint16_t>(20 + 8 + payload.size());
    uint8_t ip[20] = {};
    ip[0] = 0x45;  // Version 4, IHL 5.
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xFF);
    ip[8] = 64;    // TTL.
    ip[9] = 17;    // Protocol: UDP.
    std::memcpy(ip + 12, &src_ip, 4);
    std::memcpy(ip + 16, &dst_ip, 4);
    Append(frame, ip, 20);

    // UDP header (8 bytes).
    uint16_t udp_len = static_cast<uint16_t>(8 + payload.size());
    AppendU16BE(frame, src_port);
    AppendU16BE(frame, dst_port);
    AppendU16BE(frame, udp_len);
    AppendU16BE(frame, 0);  // Checksum (0 = not computed).

    // UDP payload.
    Append(frame, payload.data(), payload.size());
    return frame;
}

// Build an Ethernet + IPv4 + TCP frame.
std::vector<uint8_t> MakeTcpFrame(uint16_t src_port, uint16_t dst_port,
                                   const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;

    // Ethernet header.
    uint8_t eth[14] = {};
    eth[12] = 0x08; eth[13] = 0x00;
    Append(frame, eth, 14);

    // IPv4 header (20 bytes).
    uint16_t total_len = static_cast<uint16_t>(20 + 20 + payload.size());
    uint8_t ip[20] = {};
    ip[0] = 0x45;
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len & 0xFF);
    ip[8] = 64;
    ip[9] = 6;  // TCP.
    uint32_t src_ip = 0x0a000001, dst_ip = 0x0a000002;
    std::memcpy(ip + 12, &src_ip, 4);
    std::memcpy(ip + 16, &dst_ip, 4);
    Append(frame, ip, 20);

    // TCP header (20 bytes, no options). Data offset = 5 (20 bytes).
    AppendU16BE(frame, src_port);
    AppendU16BE(frame, dst_port);
    AppendU32LE(frame, 1);  // Seq num.
    AppendU32LE(frame, 0);  // Ack num.
    uint8_t tcp_misc[8] = {};
    tcp_misc[0] = 0x50;  // Data offset = 5 (5 * 4 = 20).
    Append(frame, tcp_misc, 8);

    Append(frame, payload.data(), payload.size());
    return frame;
}

// Wrap a frame in a classic pcap packet record.
void AppendPcapPacket(std::vector<uint8_t>& file,
                      const std::vector<uint8_t>& frame,
                      uint32_t ts_sec = 1000, uint32_t ts_usec = 500) {
    uint32_t len = static_cast<uint32_t>(frame.size());
    AppendU32LE(file, ts_sec);
    AppendU32LE(file, ts_usec);
    AppendU32LE(file, len);  // Included length.
    AppendU32LE(file, len);  // Original length.
    Append(file, frame.data(), frame.size());
}

// Write bytes to a temp file and return the path.
std::string WriteTempFile(const std::vector<uint8_t>& data,
                          const std::string& suffix) {
    std::string path = "/tmp/pcap_reader_test_" + suffix;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.close();
    return path;
}

// --- Pcapng helpers ---

std::vector<uint8_t> MakePcapngFile(const std::vector<std::vector<uint8_t>>& frames,
                                     uint32_t link_type = 1) {
    std::vector<uint8_t> file;

    // Section Header Block (SHB).
    // Type(4) + Length(4) + BOM(4) + ver_major(2) + ver_minor(2) +
    // section_len(8) + trailing_len(4) = 28 bytes.
    uint32_t shb_len = 28;
    AppendU32LE(file, 0x0a0d0d0a);  // Block type.
    AppendU32LE(file, shb_len);
    AppendU32LE(file, 0x1a2b3c4d);  // Byte order magic.
    AppendU16LE(file, 1);            // Version major.
    AppendU16LE(file, 0);            // Version minor.
    uint64_t section_len = 0xFFFFFFFFFFFFFFFF;  // Unspecified.
    Append(file, &section_len, 8);
    AppendU32LE(file, shb_len);      // Trailing block length.

    // Interface Description Block (IDB).
    // Type(4) + Length(4) + link_type(2) + reserved(2) + snap_len(4) +
    // trailing_len(4) = 20 bytes.
    uint32_t idb_len = 20;
    AppendU32LE(file, 1);            // Block type.
    AppendU32LE(file, idb_len);
    AppendU16LE(file, static_cast<uint16_t>(link_type));
    AppendU16LE(file, 0);            // Reserved.
    AppendU32LE(file, 65535);        // Snap length.
    AppendU32LE(file, idb_len);      // Trailing block length.

    // Enhanced Packet Blocks (EPBs).
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        uint32_t cap_len = static_cast<uint32_t>(f.size());
        // Pad captured data to 4-byte boundary.
        uint32_t padded = (cap_len + 3) & ~3u;
        // EPB: type(4) + len(4) + iface_id(4) + ts_high(4) + ts_low(4) +
        //      cap_len(4) + orig_len(4) + data(padded) + trailing_len(4).
        uint32_t epb_len = 32 + padded;
        AppendU32LE(file, 6);         // Block type: EPB.
        AppendU32LE(file, epb_len);
        AppendU32LE(file, 0);         // Interface ID.
        // Timestamp: i * 1000000 microseconds (i seconds).
        uint64_t ts = static_cast<uint64_t>(i + 1) * 1000000;
        AppendU32LE(file, static_cast<uint32_t>(ts >> 32));
        AppendU32LE(file, static_cast<uint32_t>(ts & 0xFFFFFFFF));
        AppendU32LE(file, cap_len);
        AppendU32LE(file, cap_len);   // Original length.
        Append(file, f.data(), f.size());
        // Padding.
        uint8_t pad[4] = {};
        if (padded > cap_len) Append(file, pad, padded - cap_len);
        AppendU32LE(file, epb_len);   // Trailing block length.
    }

    return file;
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
    EXPECT_EQ(pkt.ts_sec, 1700000000u);
    EXPECT_EQ(pkt.ts_usec, 42u);
    EXPECT_EQ(pkt.payload, payload);

    // No more packets.
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
    EXPECT_EQ(pkt.payload, payload);

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
        ASSERT_EQ(pkt.payload.size(), 1u);
        EXPECT_EQ(pkt.payload[0], static_cast<uint8_t>(i));
        EXPECT_EQ(pkt.ts_sec, static_cast<uint32_t>(i));
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
    EXPECT_EQ(pkt.payload, payload);
    EXPECT_EQ(pkt.ts_sec, 1u);  // 1 second (first frame = 1*1000000 us).

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
        ASSERT_EQ(pkt.payload.size(), 10u);
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
    EXPECT_EQ(pkt.payload, payload);
    std::remove(path.c_str());
}

TEST(PcapReaderTest, InvalidFileThrows) {
    EXPECT_THROW(
        { PcapReader r("/tmp/nonexistent_file_xyz.pcap"); },
        std::runtime_error);

    // Write garbage bytes.
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
    EXPECT_EQ(pkt.payload, udp_payload);

    ASSERT_TRUE(reader.next(pkt));
    EXPECT_EQ(pkt.proto, TransportProto::kTcp);
    EXPECT_EQ(pkt.payload, tcp_payload);

    EXPECT_FALSE(reader.next(pkt));
    std::remove(path.c_str());
}

}  // namespace
}  // namespace exchange
