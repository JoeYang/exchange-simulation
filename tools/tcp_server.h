#pragma once

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace exchange {

// Non-blocking TCP server using Linux epoll.
// Single-threaded event loop. Length-prefix framing: 4-byte LE length + payload.
// Designed for the iLink3 gateway to receive SBE-encoded order messages.
class TcpServer {
public:
    using ConnectCallback    = std::function<void(int fd)>;
    using MessageCallback    = std::function<void(int fd, const char* data, size_t len)>;
    using DisconnectCallback = std::function<void(int fd)>;

    struct Config {
        uint16_t port{0};             // 0 = OS picks an ephemeral port
        int backlog{128};
        int max_events{64};
        ConnectCallback on_connect;
        MessageCallback on_message;
        DisconnectCallback on_disconnect;
    };

    explicit TcpServer(Config config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Returns the port the server is listening on (useful when port=0).
    uint16_t port() const { return port_; }

    // Poll for events. Returns the number of events processed.
    // timeout_ms: epoll_wait timeout (-1 = block, 0 = non-blocking).
    int poll(int timeout_ms = 0);

    // Send a length-prefixed message to a connected client.
    // Returns true on success, false if the fd is invalid or send fails.
    bool send_message(int fd, const char* data, size_t len);

    // Disconnect a specific client.
    void disconnect(int fd);

    // Graceful shutdown: disconnect all clients and close the listener.
    void shutdown();

    // Number of currently connected clients.
    size_t client_count() const { return clients_.size(); }

private:
    static constexpr size_t kHeaderSize = 4;         // 4-byte LE length prefix
    static constexpr size_t kMaxMessageSize = 16 * 1024 * 1024;  // 16 MB
    static constexpr size_t kInitialBufSize = 64 * 1024;         // 64 KB pre-alloc

    // Pre-allocated receive buffer with read cursor to avoid O(N) erase.
    // compact() shifts unconsumed bytes to the front only when the cursor
    // passes the midpoint, amortising the memmove to O(1) per byte.
    struct ClientState {
        std::vector<char> recv_buf;
        size_t read_pos{0};   // start of unconsumed data
        size_t write_pos{0};  // end of received data

        ClientState() { recv_buf.resize(kInitialBufSize); }

        size_t readable() const { return write_pos - read_pos; }
        size_t writable() const { return recv_buf.size() - write_pos; }
        char* write_ptr() { return recv_buf.data() + write_pos; }
        const char* read_ptr() const { return recv_buf.data() + read_pos; }

        void advance_write(size_t n) { write_pos += n; }
        void advance_read(size_t n) { read_pos += n; }

        // Reclaim space when cursor passes midpoint.
        void compact() {
            if (read_pos > recv_buf.size() / 2) {
                size_t remaining = readable();
                std::memmove(recv_buf.data(), read_ptr(), remaining);
                read_pos = 0;
                write_pos = remaining;
            }
        }

        // Grow buffer if needed for large messages. Only called when a
        // frame header indicates more space is required — not on every recv.
        void ensure_capacity(size_t needed) {
            if (recv_buf.size() - write_pos < needed) {
                compact();
                if (recv_buf.size() < needed) {
                    recv_buf.resize(needed);
                }
            }
        }
    };

    void set_nonblocking(int fd);
    void accept_connections();
    void handle_read(int fd);
    void drain_and_remove_client(int fd);
    void remove_client(int fd);

    Config config_;
    int listen_fd_{-1};
    int epoll_fd_{-1};
    uint16_t port_{0};
    bool running_{false};
    std::unordered_map<int, ClientState> clients_;
};

}  // namespace exchange
