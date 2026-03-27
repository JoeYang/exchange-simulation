#pragma once

// RAII TCP client with length-prefixed framing (4-byte LE length + payload).
// Extracted from ilink3_send_order.cc for reuse across simulation binaries.

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace exchange {

class TcpClient {
    int fd_{-1};

public:
    TcpClient() = default;
    ~TcpClient() { close_conn(); }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    TcpClient(TcpClient&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }
    TcpClient& operator=(TcpClient&& other) noexcept {
        if (this != &other) {
            close_conn();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    bool connect_to(const char* host, uint16_t port) {
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
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            std::fprintf(stderr, "Invalid address: %s\n", host);
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
        char header[4];
        std::memcpy(header, &frame_len, 4);

        if (!send_all(header, 4)) return false;
        if (!send_all(data, len)) return false;
        return true;
    }

    // Receive a length-prefixed message (blocking). Returns payload as vector,
    // empty on error or disconnect.
    std::vector<char> recv_message() {
        char header[4];
        if (!recv_all(header, 4)) return {};

        uint32_t frame_len = 0;
        std::memcpy(&frame_len, header, 4);

        if (frame_len == 0 || frame_len > 16 * 1024 * 1024) return {};

        std::vector<char> buf(frame_len);
        if (!recv_all(buf.data(), frame_len)) return {};
        return buf;
    }

    // Receive a length-prefixed message into caller-supplied buffer.
    // Returns payload length, or -1 on error.
    ssize_t recv_message(char* buf, size_t max_len) {
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

    // Set socket to non-blocking mode for use in poll loops.
    bool set_nonblocking() {
        if (fd_ < 0) return false;
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags < 0) return false;
        return fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    // Poll for readable data. Returns true if data is available.
    // timeout_ms: -1 = block, 0 = non-blocking check.
    bool poll_readable(int timeout_ms) {
        if (fd_ < 0) return false;
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, timeout_ms);
        return ret > 0 && (pfd.revents & POLLIN);
    }

    void close_conn() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const { return fd_; }
    bool is_connected() const { return fd_ >= 0; }

private:
    bool send_all(const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
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
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // In non-blocking mode, wait for data.
                    if (!poll_readable(100)) return false;
                    continue;
                }
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

}  // namespace exchange
