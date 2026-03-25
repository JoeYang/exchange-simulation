#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace exchange {

// UdpMulticastPublisher sends datagrams to a multicast group:port.
// RAII: socket is closed on destruction.
// Non-copyable, movable.
class UdpMulticastPublisher {
    int fd_{-1};
    struct sockaddr_in dest_{};

public:
    // Opens a UDP socket configured for multicast sending.
    // group: multicast address (e.g. "224.0.0.1")
    // port:  destination port
    // ttl:   multicast TTL (default 1 = link-local)
    // loopback: if true, sender can receive its own messages (needed for tests)
    UdpMulticastPublisher(const char* group, uint16_t port,
                          int ttl = 1, bool loopback = true) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
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
        : fd_(o.fd_), dest_(o.dest_) { o.fd_ = -1; }
    UdpMulticastPublisher& operator=(UdpMulticastPublisher&& o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = o.fd_; dest_ = o.dest_; o.fd_ = -1;
        }
        return *this;
    }

    // Send a datagram. Returns bytes sent, or -1 on error.
    ssize_t send(const char* data, size_t len) {
        return ::sendto(fd_, data, len, 0,
                        reinterpret_cast<const struct sockaddr*>(&dest_),
                        sizeof(dest_));
    }

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

        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
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
    ssize_t receive(char* buffer, size_t max_len) {
        return ::recvfrom(fd_, buffer, max_len, 0, nullptr, nullptr);
    }

    bool is_joined() const { return joined_; }
    int fd() const { return fd_; }
};

}  // namespace exchange
