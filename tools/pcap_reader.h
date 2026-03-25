#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace exchange {

// Transport protocol extracted from the IP header.
enum class TransportProto : uint8_t {
    kTcp = 6,
    kUdp = 17,
    kOther = 0,
};

// A parsed packet with zero-copy pointers into the mmap'd pcap file.
// No heap allocation — payload is a view, not a copy.
struct PcapPacket {
    uint64_t ts_ns;          // Capture timestamp in nanoseconds since epoch.
    TransportProto proto;    // TCP or UDP.
    uint32_t src_ip;         // Source IP (network byte order).
    uint32_t dst_ip;         // Destination IP (network byte order).
    uint16_t src_port;       // Source port (host byte order).
    uint16_t dst_port;       // Destination port (host byte order).
    const uint8_t* payload;  // Pointer into mmap'd region (zero-copy).
    size_t payload_len;      // Payload length in bytes.
};

// Callback signature for forEachPacket.
using PcapCallback = std::function<void(const PcapPacket& pkt)>;

// Memory-mapped pcap/pcapng reader. Zero-copy: packet payloads are pointers
// directly into the mmap'd file — no per-packet allocation.
//
// Supports:
//   - Classic pcap: native (0xa1b2c3d4) and swapped (0xd4c3b2a1) byte order
//   - Classic pcap nanosecond variants (0xa1b23c4d / 0x4d3cb2a1)
//   - Pcapng: SHB + IDB + Enhanced Packet Blocks
//   - Ethernet (link type 1) + IPv4 + TCP/UDP
//
// Limitations:
//   - IPv6 not supported (packets skipped)
//   - VLAN tags not supported (packets skipped)
//   - Fragmented IP packets not reassembled (first fragment only)
class PcapReader {
public:
    enum class Format { kClassicPcap, kPcapng };

    explicit PcapReader(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("cannot open pcap file: " + path);

        struct stat st{};
        if (::fstat(fd_, &st) < 0) { ::close(fd_); throw std::runtime_error("fstat failed"); }
        file_size_ = static_cast<size_t>(st.st_size);

        if (file_size_ < 4) { ::close(fd_); throw std::runtime_error("pcap file too short"); }

        data_ = static_cast<const uint8_t*>(
            ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) {
            data_ = nullptr; ::close(fd_);
            throw std::runtime_error("mmap failed");
        }

        detect_format();
    }

    ~PcapReader() {
        if (data_) ::munmap(const_cast<uint8_t*>(data_), file_size_);
        if (fd_ >= 0) ::close(fd_);
    }

    PcapReader(const PcapReader&) = delete;
    PcapReader& operator=(const PcapReader&) = delete;

    // Iterate all TCP/UDP packets, invoking callback for each.
    // Zero-copy: PcapPacket::payload points into the mmap'd file.
    void forEachPacket(const PcapCallback& cb) {
        pos_ = header_end_;
        PcapPacket pkt;
        while (next_internal(pkt)) {
            cb(pkt);
        }
    }

    // Iterator-style: returns true if a packet was read, false on EOF.
    // Call repeatedly; skips non-IPv4 and non-TCP/UDP automatically.
    bool next(PcapPacket& pkt) {
        return next_internal(pkt);
    }

    // Reset iterator to the first packet.
    void reset() { pos_ = header_end_; }

    Format format() const { return format_; }
    uint32_t link_type() const { return link_type_; }

private:
    int fd_{-1};
    const uint8_t* data_{nullptr};
    size_t file_size_{0};
    size_t pos_{0};          // Current read position.
    size_t header_end_{0};   // Offset past the file/section headers.
    Format format_{};
    bool swapped_{false};
    bool nano_{false};       // Nanosecond-resolution pcap.
    uint32_t link_type_{0};

    // --- Byte-order helpers ---

    uint16_t rd16(const uint8_t* p) const {
        uint16_t v;
        std::memcpy(&v, p, 2);
        if (swapped_) v = static_cast<uint16_t>((v >> 8) | (v << 8));
        return v;
    }

    uint32_t rd32(const uint8_t* p) const {
        uint32_t v;
        std::memcpy(&v, p, 4);
        if (swapped_) v = __builtin_bswap32(v);
        return v;
    }

    // Always little-endian (pcapng on LE hosts, which is the common case).
    static uint32_t le32(const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static uint16_t le16(const uint8_t* p) {
        uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }

    static uint16_t be16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    bool has(size_t n) const { return pos_ + n <= file_size_; }

    // --- Format detection ---

    void detect_format() {
        uint32_t magic;
        std::memcpy(&magic, data_, 4);

        if (magic == 0xa1b2c3d4) {
            swapped_ = false; nano_ = false;
            format_ = Format::kClassicPcap;
            parse_classic_header();
        } else if (magic == 0xa1b23c4d) {
            swapped_ = false; nano_ = true;
            format_ = Format::kClassicPcap;
            parse_classic_header();
        } else if (magic == 0xd4c3b2a1) {
            swapped_ = true; nano_ = false;
            format_ = Format::kClassicPcap;
            parse_classic_header();
        } else if (magic == 0x4d3cb2a1) {
            swapped_ = true; nano_ = true;
            format_ = Format::kClassicPcap;
            parse_classic_header();
        } else if (magic == 0x0a0d0d0a) {
            format_ = Format::kPcapng;
            parse_pcapng_headers();
        } else {
            throw std::runtime_error("unrecognized pcap magic");
        }
    }

    // --- Classic pcap ---

    void parse_classic_header() {
        // Global header: magic(4) + ver_major(2) + ver_minor(2) + thiszone(4) +
        // sigfigs(4) + snaplen(4) + link_type(4) = 24 bytes.
        if (file_size_ < 24) throw std::runtime_error("truncated pcap global header");
        link_type_ = rd32(data_ + 20);
        pos_ = 24;
        header_end_ = 24;
    }

    bool next_classic_packet(PcapPacket& pkt) {
        while (has(16)) {
            // Packet header: ts_sec(4) + ts_usec(4) + incl_len(4) + orig_len(4).
            uint32_t ts_sec = rd32(data_ + pos_);
            uint32_t ts_frac = rd32(data_ + pos_ + 4);
            uint32_t incl_len = rd32(data_ + pos_ + 8);
            pos_ += 16;

            if (!has(incl_len)) return false;  // Truncated file.

            const uint8_t* frame = data_ + pos_;
            pos_ += incl_len;

            if (parse_ethernet_ipv4(frame, incl_len, pkt)) {
                // Convert to nanoseconds.
                pkt.ts_ns = static_cast<uint64_t>(ts_sec) * 1000000000ULL +
                            (nano_ ? ts_frac
                                   : static_cast<uint64_t>(ts_frac) * 1000ULL);
                return true;
            }
            // Skip non-IPv4/non-TCP/UDP, continue to next packet.
        }
        return false;
    }

    // --- Pcapng ---

    void parse_pcapng_headers() {
        // SHB: type(4) + block_len(4) + bom(4) + ...
        if (file_size_ < 12) throw std::runtime_error("truncated pcapng SHB");
        uint32_t shb_len = le32(data_ + 4);
        uint32_t bom = le32(data_ + 8);
        if (bom != 0x1a2b3c4d) throw std::runtime_error("unsupported pcapng byte order");

        pos_ = shb_len;

        // IDB: type(4) + block_len(4) + link_type(2) + reserved(2) + snap_len(4) + ...
        if (!has(8)) throw std::runtime_error("truncated pcapng IDB");
        uint32_t idb_type = le32(data_ + pos_);
        uint32_t idb_len = le32(data_ + pos_ + 4);
        if (idb_type != 1) throw std::runtime_error("expected pcapng IDB");
        if (!has(12)) throw std::runtime_error("truncated pcapng IDB body");
        link_type_ = le16(data_ + pos_ + 8);

        pos_ += idb_len;
        header_end_ = pos_;
    }

    bool next_pcapng_packet(PcapPacket& pkt) {
        while (has(8)) {
            uint32_t btype = le32(data_ + pos_);
            uint32_t blen = le32(data_ + pos_ + 4);

            if (blen < 12 || !has(blen)) return false;

            if (btype == 6) {
                // Enhanced Packet Block.
                // body: iface_id(4) + ts_high(4) + ts_low(4) +
                //       cap_len(4) + orig_len(4) + data...
                if (blen < 32) { pos_ += blen; continue; }
                uint32_t ts_high = le32(data_ + pos_ + 12);
                uint32_t ts_low = le32(data_ + pos_ + 16);
                uint32_t cap_len = le32(data_ + pos_ + 20);

                const uint8_t* frame = data_ + pos_ + 28;
                size_t frame_avail = blen - 32;  // Exclude header + trailing len.
                if (cap_len > frame_avail) cap_len = static_cast<uint32_t>(frame_avail);

                size_t block_end = pos_ + blen;
                pos_ = block_end;

                if (parse_ethernet_ipv4(frame, cap_len, pkt)) {
                    // Default pcapng timestamp unit is microseconds.
                    uint64_t ts_us = (static_cast<uint64_t>(ts_high) << 32) | ts_low;
                    pkt.ts_ns = ts_us * 1000ULL;
                    return true;
                }
                continue;
            }

            // Skip non-EPB blocks.
            pos_ += blen;
        }
        return false;
    }

    // --- Dispatch ---

    bool next_internal(PcapPacket& pkt) {
        if (format_ == Format::kClassicPcap) return next_classic_packet(pkt);
        return next_pcapng_packet(pkt);
    }

    // --- Packet parsing ---

    static bool parse_ethernet_ipv4(const uint8_t* data, size_t len, PcapPacket& pkt) {
        if (len < 14) return false;
        uint16_t ethertype = be16(data + 12);
        if (ethertype != 0x0800) return false;  // IPv4 only.

        const uint8_t* ip = data + 14;
        size_t ip_avail = len - 14;

        if (ip_avail < 20) return false;
        if ((ip[0] >> 4) != 4) return false;

        uint8_t ihl = (ip[0] & 0x0F) * 4;
        if (ihl < 20 || ip_avail < ihl) return false;

        uint8_t protocol = ip[9];
        std::memcpy(&pkt.src_ip, ip + 12, 4);
        std::memcpy(&pkt.dst_ip, ip + 16, 4);

        const uint8_t* transport = ip + ihl;
        size_t transport_len = ip_avail - ihl;

        if (protocol == 17) {
            // UDP header: 8 bytes.
            if (transport_len < 8) return false;
            pkt.proto = TransportProto::kUdp;
            pkt.src_port = be16(transport);
            pkt.dst_port = be16(transport + 2);
            uint16_t udp_len = be16(transport + 4);
            if (udp_len < 8) return false;
            size_t payload_len = (transport_len < udp_len)
                                     ? transport_len - 8  // Truncated by snaplen.
                                     : udp_len - 8;
            pkt.payload = transport + 8;
            pkt.payload_len = payload_len;
            return true;
        }

        if (protocol == 6) {
            // TCP header: minimum 20 bytes.
            if (transport_len < 20) return false;
            pkt.proto = TransportProto::kTcp;
            pkt.src_port = be16(transport);
            pkt.dst_port = be16(transport + 2);
            uint8_t tcp_hdr_len = ((transport[12] >> 4) & 0x0F) * 4;
            if (tcp_hdr_len < 20 || transport_len < tcp_hdr_len) return false;
            pkt.payload = transport + tcp_hdr_len;
            pkt.payload_len = transport_len - tcp_hdr_len;
            return true;
        }

        return false;
    }
};

// Convenience: open a pcap/pcapng file and invoke callback for each packet.
inline void forEachPacket(const std::string& path, const PcapCallback& cb) {
    PcapReader reader(path);
    reader.forEachPacket(cb);
}

}  // namespace exchange
