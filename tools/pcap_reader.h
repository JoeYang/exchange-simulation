#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace exchange {

// Transport protocol extracted from the IP header.
enum class TransportProto : uint8_t {
    kTcp = 6,
    kUdp = 17,
    kOther = 0,
};

// A single extracted packet payload with metadata.
struct PcapPacket {
    uint32_t ts_sec;         // Capture timestamp (seconds since epoch).
    uint32_t ts_usec;        // Sub-second portion (microseconds or nanoseconds).
    TransportProto proto;    // TCP or UDP.
    uint32_t src_ip;         // Source IP (network byte order).
    uint32_t dst_ip;         // Destination IP (network byte order).
    uint16_t src_port;       // Source port (host byte order).
    uint16_t dst_port;       // Destination port (host byte order).
    std::vector<uint8_t> payload;  // Transport-layer payload (after UDP/TCP header).
};

// Reads pcap (classic) and pcapng files, extracting UDP and TCP payloads.
//
// Usage:
//   PcapReader reader("capture.pcap");
//   PcapPacket pkt;
//   while (reader.next(pkt)) {
//       // pkt.payload contains the transport payload
//   }
//
// Supports:
//   - Classic pcap: both native and swapped byte order
//   - Pcapng: Section Header Block + Interface Description Block + Enhanced
//     Packet Block (the standard tcpdump/wireshark output format)
//   - Ethernet (link type 1) + IPv4 only; other link types are skipped
//
// Limitations:
//   - IPv6 not supported (packets skipped)
//   - VLAN tags not supported (packets skipped)
//   - IP options are handled (IHL field), TCP data offset is handled
//   - Fragmented IP packets are not reassembled (first fragment only)
class PcapReader {
public:
    enum class Format { kClassicPcap, kPcapng };

    explicit PcapReader(const std::string& path) {
        file_.open(path, std::ios::binary);
        if (!file_) throw std::runtime_error("cannot open pcap file: " + path);
        detect_format();
    }

    // Returns true if a packet was read, false on EOF.
    // Skips non-IPv4 and non-TCP/UDP packets automatically.
    bool next(PcapPacket& pkt) {
        if (format_ == Format::kClassicPcap) return next_classic(pkt);
        return next_pcapng(pkt);
    }

    Format format() const { return format_; }
    uint32_t link_type() const { return link_type_; }

private:
    std::ifstream file_;
    Format format_{};
    bool swapped_{false};   // Classic pcap: byte-swapped magic.
    uint32_t link_type_{0}; // Data link type (1 = Ethernet).

    // --- I/O helpers ---

    bool read_bytes(void* dst, size_t n) {
        file_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        return file_.good();
    }

    bool skip_bytes(size_t n) {
        file_.seekg(static_cast<std::streamoff>(n), std::ios::cur);
        return file_.good();
    }

    uint16_t swap16(uint16_t v) const {
        return swapped_ ? static_cast<uint16_t>((v >> 8) | (v << 8)) : v;
    }

    uint32_t swap32(uint32_t v) const {
        if (!swapped_) return v;
        return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
               ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
    }

    // Read a little-endian uint32 (for pcapng, which is always LE in practice).
    static uint32_t le32(const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    static uint16_t be16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    // --- Format detection ---

    void detect_format() {
        uint32_t magic;
        if (!read_bytes(&magic, 4))
            throw std::runtime_error("pcap file too short");

        if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d) {
            // Classic pcap, native byte order.
            // 0xa1b23c4d = nanosecond-resolution variant.
            swapped_ = false;
            format_ = Format::kClassicPcap;
            parse_classic_header();
        } else if (magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1) {
            // Classic pcap, swapped byte order.
            swapped_ = true;
            format_ = Format::kClassicPcap;
            parse_classic_header();
        } else if (magic == 0x0a0d0d0a) {
            // Pcapng Section Header Block.
            format_ = Format::kPcapng;
            parse_pcapng_shb();
        } else {
            throw std::runtime_error("unrecognized pcap magic");
        }
    }

    // --- Classic pcap ---

    void parse_classic_header() {
        // We already read 4 bytes (magic). Read remaining 20 bytes of the
        // global header: version_major(2) + version_minor(2) + thiszone(4) +
        // sigfigs(4) + snaplen(4) + link_type(4) = 20 bytes.
        uint8_t hdr[20];
        if (!read_bytes(hdr, 20))
            throw std::runtime_error("truncated pcap global header");
        uint32_t lt;
        std::memcpy(&lt, hdr + 16, 4);
        link_type_ = swap32(lt);
    }

    bool next_classic(PcapPacket& pkt) {
        // Packet header: ts_sec(4) + ts_usec(4) + incl_len(4) + orig_len(4).
        uint8_t phdr[16];
        if (!read_bytes(phdr, 16)) return false;

        uint32_t ts_sec, ts_usec, incl_len;
        std::memcpy(&ts_sec, phdr, 4);
        std::memcpy(&ts_usec, phdr + 4, 4);
        std::memcpy(&incl_len, phdr + 8, 4);
        ts_sec = swap32(ts_sec);
        ts_usec = swap32(ts_usec);
        incl_len = swap32(incl_len);

        if (incl_len > 65535) return false;  // Sanity check.

        std::vector<uint8_t> raw(incl_len);
        if (!read_bytes(raw.data(), incl_len)) return false;

        if (parse_ethernet_ipv4(raw.data(), incl_len, pkt)) {
            pkt.ts_sec = ts_sec;
            pkt.ts_usec = ts_usec;
            return true;
        }
        // Skip non-IPv4/non-TCP/UDP and try next packet.
        return next_classic(pkt);
    }

    // --- Pcapng ---

    void parse_pcapng_shb() {
        // We already read 4 bytes (block type = 0x0a0d0d0a).
        // SHB: block_total_length(4) + byte_order_magic(4) + ...
        uint8_t buf[8];
        if (!read_bytes(buf, 8))
            throw std::runtime_error("truncated pcapng SHB");

        uint32_t block_len = le32(buf);
        uint32_t bom = le32(buf + 4);
        if (bom != 0x1a2b3c4d)
            throw std::runtime_error("unsupported pcapng byte order");

        // Skip rest of SHB (we already read 4+8=12 bytes, block_len includes
        // the type field too).
        if (block_len > 12) skip_bytes(block_len - 12);

        // Next block should be Interface Description Block (type 0x00000001).
        parse_pcapng_idb();
    }

    void parse_pcapng_idb() {
        uint8_t bhdr[8];
        if (!read_bytes(bhdr, 8))
            throw std::runtime_error("truncated pcapng IDB");

        uint32_t btype = le32(bhdr);
        uint32_t blen = le32(bhdr + 4);
        if (btype != 1)
            throw std::runtime_error("expected pcapng IDB, got block type " +
                                     std::to_string(btype));

        // IDB body: link_type(2) + reserved(2) + snap_len(4) + options...
        uint8_t body[4];
        if (!read_bytes(body, 4))
            throw std::runtime_error("truncated pcapng IDB body");

        uint16_t lt;
        std::memcpy(&lt, body, 2);
        link_type_ = lt;

        // Skip rest of IDB. Already read 8 (header) + 4 (body) = 12 bytes.
        if (blen > 12) skip_bytes(blen - 12);
    }

    bool next_pcapng(PcapPacket& pkt) {
        uint8_t bhdr[8];
        if (!read_bytes(bhdr, 8)) return false;

        uint32_t btype = le32(bhdr);
        uint32_t blen = le32(bhdr + 4);

        if (blen < 12) return false;  // Minimum block size.

        // Enhanced Packet Block (type 6).
        if (btype == 6) {
            // EPB body: interface_id(4) + ts_high(4) + ts_low(4) +
            //           captured_len(4) + original_len(4) + packet_data...
            uint8_t epb[20];
            if (!read_bytes(epb, 20)) return false;

            uint32_t ts_high = le32(epb + 4);
            uint32_t ts_low = le32(epb + 8);
            uint32_t cap_len = le32(epb + 12);

            if (cap_len > 65535) return false;

            std::vector<uint8_t> raw(cap_len);
            if (!read_bytes(raw.data(), cap_len)) return false;

            // Skip padding + trailing block length.
            size_t consumed = 8 + 20 + cap_len;
            if (blen > consumed) skip_bytes(blen - consumed);

            if (parse_ethernet_ipv4(raw.data(), cap_len, pkt)) {
                // pcapng timestamps are in interface-defined units
                // (default: microseconds since epoch as 64-bit value).
                uint64_t ts = (static_cast<uint64_t>(ts_high) << 32) | ts_low;
                pkt.ts_sec = static_cast<uint32_t>(ts / 1000000);
                pkt.ts_usec = static_cast<uint32_t>(ts % 1000000);
                return true;
            }
            // Not an IPv4 TCP/UDP packet, try next block.
            return next_pcapng(pkt);
        }

        // Skip unknown block types.
        if (blen > 8) skip_bytes(blen - 8);
        return next_pcapng(pkt);
    }

    // --- Packet parsing ---

    // Parse Ethernet + IPv4 + TCP/UDP. Returns false if not applicable.
    bool parse_ethernet_ipv4(const uint8_t* data, size_t len, PcapPacket& pkt) {
        if (link_type_ != 1) return false;  // Ethernet only.
        if (len < 14) return false;         // Ethernet header.

        // EtherType at offset 12.
        uint16_t ethertype = be16(data + 12);
        if (ethertype != 0x0800) return false;  // IPv4 only.

        const uint8_t* ip = data + 14;
        size_t ip_len = len - 14;

        if (ip_len < 20) return false;  // Minimum IPv4 header.
        uint8_t version_ihl = ip[0];
        if ((version_ihl >> 4) != 4) return false;  // IPv4.

        uint8_t ihl = (version_ihl & 0x0F) * 4;  // Header length in bytes.
        if (ihl < 20 || ip_len < ihl) return false;

        uint8_t protocol = ip[9];
        std::memcpy(&pkt.src_ip, ip + 12, 4);
        std::memcpy(&pkt.dst_ip, ip + 16, 4);

        const uint8_t* transport = ip + ihl;
        size_t transport_len = ip_len - ihl;

        if (protocol == 17) {
            // UDP: src_port(2) + dst_port(2) + length(2) + checksum(2) = 8.
            if (transport_len < 8) return false;
            pkt.proto = TransportProto::kUdp;
            pkt.src_port = be16(transport);
            pkt.dst_port = be16(transport + 2);
            uint16_t udp_len = be16(transport + 4);
            if (udp_len < 8 || transport_len < udp_len) return false;
            size_t payload_len = udp_len - 8;
            pkt.payload.assign(transport + 8, transport + 8 + payload_len);
            return true;
        }

        if (protocol == 6) {
            // TCP: need at least 20 bytes for header.
            if (transport_len < 20) return false;
            pkt.proto = TransportProto::kTcp;
            pkt.src_port = be16(transport);
            pkt.dst_port = be16(transport + 2);
            uint8_t tcp_hdr_len = ((transport[12] >> 4) & 0x0F) * 4;
            if (tcp_hdr_len < 20 || transport_len < tcp_hdr_len) return false;
            size_t payload_len = transport_len - tcp_hdr_len;
            pkt.payload.assign(transport + tcp_hdr_len,
                               transport + tcp_hdr_len + payload_len);
            return true;
        }

        return false;  // Not TCP or UDP.
    }
};

}  // namespace exchange
