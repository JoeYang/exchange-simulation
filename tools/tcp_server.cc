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

    // Register listener with epoll.
    epoll_event ev{};
    ev.events = EPOLLIN;
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
    while (true) {
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;  // Other error, stop accepting this round.
        }

        int opt = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            ::close(client_fd);
            continue;
        }

        clients_[client_fd] = ClientState{};

        if (config_.on_connect) {
            config_.on_connect(client_fd);
        }
    }
}

void TcpServer::handle_read(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    auto& state = it->second;
    char buf[4096];

    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            state.recv_buf.insert(state.recv_buf.end(), buf, buf + n);
        } else if (n == 0) {
            // Peer closed connection.
            remove_client(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // Real error — disconnect.
            remove_client(fd);
            return;
        }
    }

    // Process complete frames: [4-byte LE length][payload].
    while (state.recv_buf.size() >= kHeaderSize) {
        uint32_t payload_len = 0;
        std::memcpy(&payload_len, state.recv_buf.data(), kHeaderSize);

        if (payload_len > kMaxMessageSize) {
            // Protocol violation — disconnect.
            remove_client(fd);
            return;
        }

        size_t frame_size = kHeaderSize + payload_len;
        if (state.recv_buf.size() < frame_size) break;  // Incomplete frame.

        if (config_.on_message) {
            config_.on_message(fd, state.recv_buf.data() + kHeaderSize, payload_len);
        }

        state.recv_buf.erase(state.recv_buf.begin(),
                             state.recv_buf.begin() + static_cast<ptrdiff_t>(frame_size));
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

        if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            remove_client(fd);
            continue;
        }

        if (events[i].events & EPOLLIN) {
            handle_read(fd);
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
