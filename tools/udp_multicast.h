#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace exchange {

// 4-byte little-endian sequence header prepended to every datagram.
// Allows receivers to detect gaps without parsing the payload.
struct McastSeqHeader {
    uint32_t seq_num;
} __attribute__((packed));
static_assert(sizeof(McastSeqHeader) == 4);

enum class SendResult : uint8_t {
    kOk,          // Datagram sent successfully.
    kWouldBlock,  // Socket buffer full (EAGAIN/EWOULDBLOCK). Retry later.
    kError,       // Unrecoverable send error.
};

// UdpMulticastPublisher sends datagrams to a multicast group:port.
//
// Each send() prepends a 4-byte sequence header (zero-copy via sendmsg/iovec).
// The socket is non-blocking; send() returns SendResult::kWouldBlock when the
// kernel buffer is full rather than blocking the caller.
//
// RAII: socket is closed on destruction.
// Non-copyable, movable.
class UdpMulticastPublisher {
    int fd_{-1};
    struct sockaddr_in dest_{};
    uint32_t seq_{0};

public:
    // Opens a UDP socket configured for multicast sending.
    // group: multicast address (e.g. "224.0.0.1")
    // port:  destination port
    // ttl:   multicast TTL (default 1 = link-local)
    // loopback: if true, sender can receive its own messages (needed for tests)
    UdpMulticastPublisher(const char* group, uint16_t port,
                          int ttl = 1, bool loopback = true) {
        if (ttl < 0 || ttl > 255)
            throw std::runtime_error("TTL must be in range [0, 255]");

        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        // Set multicast TTL.
        unsigned char mc_ttl = static_cast<unsigned char>(ttl);
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                         &mc_ttl, sizeof(mc_ttl)) < 0) {
            ::close(fd_);
            throw std::runtime_error("setsockopt(IP_MULTICAST_TTL) failed");
        }

        // Set loopback.
        unsigned char mc_loop = loopback ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                         &mc_loop, sizeof(mc_loop)) < 0) {
            ::close(fd_);
            throw std::runtime_error("setsockopt(IP_MULTICAST_LOOP) failed");
        }

        dest_.sin_family = AF_INET;
        dest_.sin_port = htons(port);
        if (::inet_pton(AF_INET, group, &dest_.sin_addr) != 1) {
            ::close(fd_);
            throw std::runtime_error("inet_pton() failed for group address");
        }
    }

    ~UdpMulticastPublisher() {
        if (fd_ >= 0) ::close(fd_);
    }

    UdpMulticastPublisher(const UdpMulticastPublisher&) = delete;
    UdpMulticastPublisher& operator=(const UdpMulticastPublisher&) = delete;
    UdpMulticastPublisher(UdpMulticastPublisher&& o) noexcept
        : fd_(o.fd_), dest_(o.dest_), seq_(o.seq_) { o.fd_ = -1; }
    UdpMulticastPublisher& operator=(UdpMulticastPublisher&& o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = o.fd_; dest_ = o.dest_; seq_ = o.seq_; o.fd_ = -1;
        }
        return *this;
    }

    // Send a datagram with a 4-byte sequence header prepended.
    // Zero-copy: uses sendmsg with scatter/gather (iovec) so the payload
    // pointer is passed directly to the kernel — no memcpy.
    // Sequence number increments only on successful send.
    SendResult send(const char* data, size_t len) {
        McastSeqHeader hdr{seq_};
        struct iovec iov[2];
        iov[0].iov_base = &hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = const_cast<char*>(data);
        iov[1].iov_len = len;

        struct msghdr msg{};
        msg.msg_name = &dest_;
        msg.msg_namelen = sizeof(dest_);
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;

        ssize_t sent;
        do {
            sent = ::sendmsg(fd_, &msg, MSG_DONTWAIT);
        } while (sent < 0 && errno == EINTR);

        if (sent >= 0) {
            ++seq_;
            return SendResult::kOk;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SendResult::kWouldBlock;
        }
        return SendResult::kError;
    }

    uint32_t sequence_number() const { return seq_; }
    int fd() const { return fd_; }
};

// UdpMulticastReceiver joins a multicast group and receives datagrams.
// RAII: leaves group and closes socket on destruction.
// Non-copyable, non-movable (holds group membership state).
class UdpMulticastReceiver {
    int fd_{-1};
    struct ip_mreq mreq_{};
    bool joined_{false};

public:
    UdpMulticastReceiver() = default;

    ~UdpMulticastReceiver() {
        if (joined_) leave_group();
        if (fd_ >= 0) ::close(fd_);
    }

    UdpMulticastReceiver(const UdpMulticastReceiver&) = delete;
    UdpMulticastReceiver& operator=(const UdpMulticastReceiver&) = delete;

    // Bind to port, join multicast group.
    void join_group(const char* group, uint16_t port) {
        if (joined_) leave_group();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }

        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        // Allow multiple receivers on the same port (useful for tests).
        int reuse = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                         &reuse, sizeof(reuse)) < 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("bind() failed");
        }

        // Join the multicast group on the default interface.
        if (::inet_pton(AF_INET, group, &mreq_.imr_multiaddr) != 1) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("inet_pton() failed for group address");
        }
        mreq_.imr_interface.s_addr = htonl(INADDR_ANY);

        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &mreq_, sizeof(mreq_)) < 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("setsockopt(IP_ADD_MEMBERSHIP) failed");
        }
        joined_ = true;
    }

    // Leave the multicast group (idempotent).
    void leave_group() {
        if (!joined_ || fd_ < 0) return;
        ::setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                     &mreq_, sizeof(mreq_));
        joined_ = false;
    }

    // Receive a datagram. Returns bytes received, or -1 on error.
    // Blocks until data arrives (caller should set a receive timeout if needed).
    // Returns -1 with errno=EBADF if called before join_group().
    ssize_t receive(char* buffer, size_t max_len) {
        if (fd_ < 0) { errno = EBADF; return -1; }
        return ::recvfrom(fd_, buffer, max_len, 0, nullptr, nullptr);
    }

    bool is_joined() const { return joined_; }
    int fd() const { return fd_; }
};

}  // namespace exchange
