#include "tools/tcp_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace exchange {
namespace {

// Helper: connect a raw TCP client to 127.0.0.1:port. Returns fd or -1.
int ConnectClient(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    int opt = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Helper: send a length-prefixed message on a raw client socket.
bool SendFramed(int fd, const char* data, size_t len) {
    uint32_t header = static_cast<uint32_t>(len);
    if (::send(fd, &header, sizeof(header), MSG_NOSIGNAL) != sizeof(header)) return false;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Helper: receive a length-prefixed message on a raw client socket.
// Returns the payload as a string, or empty on failure.
std::string RecvFramed(int fd) {
    uint32_t header = 0;
    size_t got = 0;
    while (got < sizeof(header)) {
        ssize_t n = ::recv(fd, reinterpret_cast<char*>(&header) + got,
                           sizeof(header) - got, 0);
        if (n <= 0) return {};
        got += static_cast<size_t>(n);
    }

    std::string payload(header, '\0');
    got = 0;
    while (got < header) {
        ssize_t n = ::recv(fd, payload.data() + got, header - got, 0);
        if (n <= 0) return {};
        got += static_cast<size_t>(n);
    }
    return payload;
}

// Poll the server until a condition is met, or timeout.
void PollUntil(TcpServer& server, const std::function<bool()>& pred,
               int timeout_ms = 1000) {
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        server.poll(1);
    }
}

// ---------------------------------------------------------------------------
// Basic connect and message
// ---------------------------------------------------------------------------

TEST(TcpServerTest, ConnectAndSendMessage) {
    int connected_fd = -1;
    std::string received_msg;
    bool disconnected = false;

    TcpServer::Config cfg;
    cfg.on_connect = [&](int fd) { connected_fd = fd; };
    cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        received_msg.assign(data, len);
    };
    cfg.on_disconnect = [&](int fd) {
        (void)fd;
        disconnected = true;
    };

    TcpServer server(cfg);
    ASSERT_GT(server.port(), 0);

    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    // Accept connection.
    PollUntil(server, [&] { return connected_fd != -1; });
    EXPECT_NE(connected_fd, -1);
    EXPECT_EQ(server.client_count(), 1u);

    // Send a message.
    std::string msg = "hello exchange";
    ASSERT_TRUE(SendFramed(client, msg.data(), msg.size()));

    PollUntil(server, [&] { return !received_msg.empty(); });
    EXPECT_EQ(received_msg, msg);

    // Close client.
    ::close(client);
    PollUntil(server, [&] { return disconnected; });
    EXPECT_TRUE(disconnected);
    EXPECT_EQ(server.client_count(), 0u);
}

// ---------------------------------------------------------------------------
// Server sends message to client
// ---------------------------------------------------------------------------

TEST(TcpServerTest, ServerSendsToClient) {
    int server_fd = -1;
    TcpServer::Config cfg;
    cfg.on_connect = [&](int fd) { server_fd = fd; };

    TcpServer server(cfg);

    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server_fd != -1; });

    std::string msg = "response from server";
    ASSERT_TRUE(server.send_message(server_fd, msg.data(), msg.size()));

    std::string received = RecvFramed(client);
    EXPECT_EQ(received, msg);

    ::close(client);
    PollUntil(server, [&] { return server.client_count() == 0; });
}

// ---------------------------------------------------------------------------
// Multiple clients
// ---------------------------------------------------------------------------

TEST(TcpServerTest, MultipleClients) {
    std::vector<int> connected_fds;
    std::vector<std::string> messages;

    TcpServer::Config cfg;
    cfg.on_connect = [&](int fd) { connected_fds.push_back(fd); };
    cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        messages.emplace_back(data, len);
    };

    TcpServer server(cfg);

    constexpr int kNumClients = 5;
    std::vector<int> clients;
    for (int i = 0; i < kNumClients; ++i) {
        int c = ConnectClient(server.port());
        ASSERT_NE(c, -1) << "client " << i;
        clients.push_back(c);
    }

    PollUntil(server, [&] {
        return connected_fds.size() == kNumClients;
    });
    EXPECT_EQ(server.client_count(), static_cast<size_t>(kNumClients));

    // Each client sends a unique message.
    for (int i = 0; i < kNumClients; ++i) {
        std::string msg = "client_" + std::to_string(i);
        ASSERT_TRUE(SendFramed(clients[i], msg.data(), msg.size()));
    }

    PollUntil(server, [&] {
        return messages.size() == kNumClients;
    });
    EXPECT_EQ(messages.size(), static_cast<size_t>(kNumClients));

    for (int c : clients) ::close(c);
    PollUntil(server, [&] { return server.client_count() == 0; });
}

// ---------------------------------------------------------------------------
// Large message (fragmented across TCP segments)
// ---------------------------------------------------------------------------

TEST(TcpServerTest, LargeMessageFragmented) {
    std::string received_msg;

    TcpServer::Config cfg;
    cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        received_msg.assign(data, len);
    };

    TcpServer server(cfg);
    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server.client_count() == 1; });

    // 64 KB message — will be fragmented across multiple TCP segments.
    constexpr size_t kSize = 64 * 1024;
    std::string large_msg(kSize, 'X');
    // Fill with a pattern so we can verify integrity.
    for (size_t i = 0; i < kSize; ++i) {
        large_msg[i] = static_cast<char>('A' + (i % 26));
    }

    ASSERT_TRUE(SendFramed(client, large_msg.data(), large_msg.size()));

    PollUntil(server, [&] { return !received_msg.empty(); }, 3000);
    EXPECT_EQ(received_msg.size(), kSize);
    EXPECT_EQ(received_msg, large_msg);

    ::close(client);
}

// ---------------------------------------------------------------------------
// Client disconnect detection
// ---------------------------------------------------------------------------

TEST(TcpServerTest, ClientDisconnectDetection) {
    int disconnect_fd = -1;

    TcpServer::Config cfg;
    cfg.on_disconnect = [&](int fd) { disconnect_fd = fd; };

    TcpServer server(cfg);
    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server.client_count() == 1; });

    // Just close the client and check.
    ::close(client);

    PollUntil(server, [&] { return disconnect_fd != -1; });
    EXPECT_NE(disconnect_fd, -1);
    EXPECT_EQ(server.client_count(), 0u);
}

// ---------------------------------------------------------------------------
// Server shutdown while clients connected
// ---------------------------------------------------------------------------

TEST(TcpServerTest, ShutdownWithConnectedClients) {
    std::vector<int> disconnected_fds;

    TcpServer::Config cfg;
    cfg.on_disconnect = [&](int fd) { disconnected_fds.push_back(fd); };

    TcpServer server(cfg);

    constexpr int kNumClients = 3;
    std::vector<int> clients;
    for (int i = 0; i < kNumClients; ++i) {
        int c = ConnectClient(server.port());
        ASSERT_NE(c, -1);
        clients.push_back(c);
    }

    PollUntil(server, [&] {
        return server.client_count() == kNumClients;
    });

    // Shutdown should disconnect all clients and fire callbacks.
    server.shutdown();
    EXPECT_EQ(server.client_count(), 0u);
    EXPECT_EQ(disconnected_fds.size(), static_cast<size_t>(kNumClients));

    // Further polls should be no-ops.
    EXPECT_EQ(server.poll(0), 0);

    // Client-side should see closed connection.
    for (int c : clients) {
        char buf[1];
        ssize_t n = ::recv(c, buf, 1, MSG_DONTWAIT);
        // Either 0 (FIN) or -1 (ECONNRESET/ENOTCONN).
        EXPECT_LE(n, 0);
        ::close(c);
    }
}

// ---------------------------------------------------------------------------
// Multiple messages in sequence from same client
// ---------------------------------------------------------------------------

TEST(TcpServerTest, MultipleMessagesFromSameClient) {
    std::vector<std::string> messages;

    TcpServer::Config cfg;
    cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        messages.emplace_back(data, len);
    };

    TcpServer server(cfg);
    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server.client_count() == 1; });

    constexpr int kMsgCount = 10;
    for (int i = 0; i < kMsgCount; ++i) {
        std::string msg = "msg_" + std::to_string(i);
        ASSERT_TRUE(SendFramed(client, msg.data(), msg.size()));
    }

    PollUntil(server, [&] {
        return messages.size() == kMsgCount;
    });

    for (int i = 0; i < kMsgCount; ++i) {
        EXPECT_EQ(messages[i], "msg_" + std::to_string(i));
    }

    ::close(client);
}

// ---------------------------------------------------------------------------
// Server disconnect (server-initiated)
// ---------------------------------------------------------------------------

TEST(TcpServerTest, ServerInitiatedDisconnect) {
    int server_fd = -1;
    bool disconnect_called = false;

    TcpServer::Config cfg;
    cfg.on_connect = [&](int fd) { server_fd = fd; };
    cfg.on_disconnect = [&](int fd) {
        (void)fd;
        disconnect_called = true;
    };

    TcpServer server(cfg);
    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server_fd != -1; });

    server.disconnect(server_fd);
    EXPECT_TRUE(disconnect_called);
    EXPECT_EQ(server.client_count(), 0u);

    // Client should see connection closed.
    char buf[1];
    ssize_t n = ::recv(client, buf, 1, 0);
    EXPECT_LE(n, 0);

    ::close(client);
}

// ---------------------------------------------------------------------------
// send_message to invalid fd returns false
// ---------------------------------------------------------------------------

TEST(TcpServerTest, SendToInvalidFdReturnsFalse) {
    TcpServer::Config cfg;
    TcpServer server(cfg);

    EXPECT_FALSE(server.send_message(9999, "hello", 5));
}

// ---------------------------------------------------------------------------
// Double shutdown is safe
// ---------------------------------------------------------------------------

TEST(TcpServerTest, DoubleShutdownIsSafe) {
    TcpServer::Config cfg;
    TcpServer server(cfg);

    server.shutdown();
    server.shutdown();  // Must not crash.
    EXPECT_EQ(server.poll(0), 0);
}

// ---------------------------------------------------------------------------
// Multiple frames coalesced into one TCP segment
// ---------------------------------------------------------------------------

TEST(TcpServerTest, CoalescedFrames) {
    std::vector<std::string> messages;

    TcpServer::Config cfg;
    cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        messages.emplace_back(data, len);
    };

    TcpServer server(cfg);
    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server.client_count() == 1; });

    // Build two framed messages into a single buffer and send at once.
    std::string msg1 = "AAA";
    std::string msg2 = "BBBBB";

    std::vector<char> combined;
    auto append_frame = [&](const std::string& m) {
        uint32_t len = static_cast<uint32_t>(m.size());
        combined.insert(combined.end(),
                        reinterpret_cast<char*>(&len),
                        reinterpret_cast<char*>(&len) + 4);
        combined.insert(combined.end(), m.begin(), m.end());
    };
    append_frame(msg1);
    append_frame(msg2);

    ssize_t sent = ::send(client, combined.data(), combined.size(), MSG_NOSIGNAL);
    ASSERT_EQ(sent, static_cast<ssize_t>(combined.size()));

    PollUntil(server, [&] { return messages.size() == 2; });
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], msg1);
    EXPECT_EQ(messages[1], msg2);

    ::close(client);
}

// ---------------------------------------------------------------------------
// Data received before peer close is processed before disconnect callback
// ---------------------------------------------------------------------------

TEST(TcpServerTest, DataBeforeDisconnectIsProcessed) {
    std::vector<std::string> messages;
    bool disconnected = false;
    size_t msgs_at_disconnect = 0;

    TcpServer::Config cfg;
    cfg.on_message = [&](int fd, const char* data, size_t len) {
        (void)fd;
        messages.emplace_back(data, len);
    };
    cfg.on_disconnect = [&](int fd) {
        (void)fd;
        msgs_at_disconnect = messages.size();
        disconnected = true;
    };

    TcpServer server(cfg);
    int client = ConnectClient(server.port());
    ASSERT_NE(client, -1);

    PollUntil(server, [&] { return server.client_count() == 1; });

    // Send a message then immediately close — server must see the message
    // before the disconnect callback fires.
    std::string msg = "final_message";
    ASSERT_TRUE(SendFramed(client, msg.data(), msg.size()));
    ::close(client);

    PollUntil(server, [&] { return disconnected; });
    EXPECT_TRUE(disconnected);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0], msg);
    // The message callback must have fired before the disconnect callback.
    EXPECT_EQ(msgs_at_disconnect, 1u);
}

}  // namespace
}  // namespace exchange
