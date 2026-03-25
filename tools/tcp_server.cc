#include "tools/tcp_server.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace exchange {

TcpServer::TcpServer(Config config) : config_(std::move(config)) {
    // Create epoll instance.
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));
    }

    // Create listening socket.
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ == -1) {
        ::close(epoll_fd_);
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(config_.port);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(listen_fd_);
        ::close(epoll_fd_);
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }

    // Retrieve the actual port (important when port=0).
    sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound_addr), &addr_len) == -1) {
        ::close(listen_fd_);
        ::close(epoll_fd_);
        throw std::runtime_error(std::string("getsockname: ") + std::strerror(errno));
    }
    port_ = ntohs(bound_addr.sin_port);

    if (::listen(listen_fd_, config_.backlog) == -1) {
        ::close(listen_fd_);
        ::close(epoll_fd_);
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    }

    // Register listener with epoll — edge-triggered.
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) == -1) {
        ::close(listen_fd_);
        ::close(epoll_fd_);
        throw std::runtime_error(std::string("epoll_ctl: ") + std::strerror(errno));
    }

    running_ = true;
}

TcpServer::~TcpServer() {
    shutdown();
}

void TcpServer::set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return;
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TcpServer::accept_connections() {
    // EPOLLET: must drain all pending connections until EAGAIN.
    while (true) {
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        int opt = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Edge-triggered for client sockets; EPOLLRDHUP to detect peer close.
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            ::close(client_fd);
            continue;
        }

        clients_.emplace(client_fd, ClientState{});

        if (config_.on_connect) {
            config_.on_connect(client_fd);
        }
    }
}

void TcpServer::handle_read(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    auto& state = it->second;

    // EPOLLET: drain all available data until EAGAIN.
    while (true) {
        // Ensure we have space to recv into.
        if (state.writable() == 0) {
            state.compact();
            if (state.writable() == 0) {
                // Buffer full after compact — double it.
                state.recv_buf.resize(state.recv_buf.size() * 2);
            }
        }

        ssize_t n = ::recv(fd, state.write_ptr(), state.writable(), 0);
        if (n > 0) {
            state.advance_write(static_cast<size_t>(n));
        } else if (n == 0) {
            // Peer closed — process remaining data first, then disconnect.
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // Real error — process remaining data first, then disconnect.
            break;
        }
    }

    // Process complete frames: [4-byte LE length][payload].
    // Uses read cursor — no memmove per frame.
    while (state.readable() >= kHeaderSize) {
        uint32_t payload_len = 0;
        std::memcpy(&payload_len, state.read_ptr(), kHeaderSize);

        if (payload_len > kMaxMessageSize) {
            remove_client(fd);
            return;
        }

        size_t frame_size = kHeaderSize + payload_len;
        if (state.readable() < frame_size) {
            // Incomplete frame — ensure buffer can hold the full frame.
            state.ensure_capacity(frame_size);
            break;
        }

        if (config_.on_message) {
            config_.on_message(fd, state.read_ptr() + kHeaderSize, payload_len);
        }

        state.advance_read(frame_size);
    }

    // Compact if cursor is past midpoint to reclaim space.
    state.compact();
}

// Drain any remaining data in the socket, process frames, then disconnect.
void TcpServer::drain_and_remove_client(int fd) {
    // Read any final data the peer sent before closing.
    handle_read(fd);
    // Now remove if still present (handle_read may have removed on protocol error).
    if (clients_.find(fd) != clients_.end()) {
        remove_client(fd);
    }
}

void TcpServer::remove_client(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);

    if (config_.on_disconnect) {
        config_.on_disconnect(fd);
    }

    clients_.erase(fd);
}

int TcpServer::poll(int timeout_ms) {
    if (!running_) return 0;

    epoll_event events[64];
    int max_ev = std::min(config_.max_events, 64);
    int n = ::epoll_wait(epoll_fd_, events, max_ev, timeout_ms);
    if (n == -1) {
        if (errno == EINTR) return 0;
        return 0;
    }

    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;

        if (fd == listen_fd_) {
            accept_connections();
            continue;
        }

        // Always attempt to read data first — even on EPOLLRDHUP/EPOLLHUP.
        // The peer may have sent final data before closing.
        if (events[i].events & EPOLLIN) {
            handle_read(fd);
        }

        // After processing data, handle disconnect indicators.
        if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            drain_and_remove_client(fd);
            continue;
        }
    }

    return n;
}

bool TcpServer::send_message(int fd, const char* data, size_t len) {
    if (clients_.find(fd) == clients_.end()) return false;

    uint32_t header = static_cast<uint32_t>(len);
    // Send header.
    size_t sent = 0;
    const char* hdr_ptr = reinterpret_cast<const char*>(&header);
    while (sent < kHeaderSize) {
        ssize_t n = ::send(fd, hdr_ptr + sent, kHeaderSize - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }

    // Send payload.
    sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }

    return true;
}

void TcpServer::disconnect(int fd) {
    if (clients_.find(fd) != clients_.end()) {
        remove_client(fd);
    }
}

void TcpServer::shutdown() {
    if (!running_) return;
    running_ = false;

    // Disconnect all clients.
    std::vector<int> fds;
    fds.reserve(clients_.size());
    for (const auto& [fd, _] : clients_) {
        fds.push_back(fd);
    }
    for (int fd : fds) {
        remove_client(fd);
    }

    // Close listener.
    if (listen_fd_ != -1) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listen_fd_, nullptr);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    // Close epoll.
    if (epoll_fd_ != -1) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

}  // namespace exchange
