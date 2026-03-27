#include "tools/tcp_client.h"
#include "tools/tcp_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace exchange {
namespace {

// Helper: spin up a TcpServer, run the test body, then shut down.
class TcpClientTest : public ::testing::Test {
protected:
    TcpServer::Config make_config() {
        TcpServer::Config cfg{};
        cfg.port = 0;  // ephemeral port
        cfg.on_connect = [this](int /*fd*/) { connected_.store(true); };
        cfg.on_message = [this](int fd, const char* data, size_t len) {
            last_msg_.assign(data, len);
            // Echo the message back.
            server_->send_message(fd, data, len);
        };
        cfg.on_disconnect = [this](int /*fd*/) { disconnected_.store(true); };
        return cfg;
    }

    void SetUp() override {
        server_ = std::make_unique<TcpServer>(make_config());
        // Run server poll loop in background thread.
        server_thread_ = std::thread([this] {
            while (!stop_server_.load()) {
                server_->poll(10);
            }
        });
    }

    void TearDown() override {
        stop_server_.store(true);
        if (server_thread_.joinable()) server_thread_.join();
        server_->shutdown();
    }

    std::unique_ptr<TcpServer> server_;
    std::thread server_thread_;
    std::atomic<bool> stop_server_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> disconnected_{false};
    std::string last_msg_;
};

TEST_F(TcpClientTest, ConnectSendRecv) {
    TcpClient client;
    ASSERT_TRUE(client.connect_to("127.0.0.1", server_->port()));
    EXPECT_TRUE(client.is_connected());

    // Wait for server to register the connection.
    for (int i = 0; i < 100 && !connected_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(connected_.load());

    // Send a message and receive the echo.
    const char payload[] = "hello exchange";
    ASSERT_TRUE(client.send_message(payload, sizeof(payload) - 1));

    auto response = client.recv_message();
    ASSERT_FALSE(response.empty());
    EXPECT_EQ(std::string(response.data(), response.size()), "hello exchange");
}

TEST_F(TcpClientTest, RecvMessageIntoBuffer) {
    TcpClient client;
    ASSERT_TRUE(client.connect_to("127.0.0.1", server_->port()));

    for (int i = 0; i < 100 && !connected_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const char payload[] = "buf test";
    ASSERT_TRUE(client.send_message(payload, sizeof(payload) - 1));

    char buf[256];
    ssize_t len = client.recv_message(buf, sizeof(buf));
    ASSERT_GT(len, 0);
    EXPECT_EQ(std::string(buf, static_cast<size_t>(len)), "buf test");
}

TEST_F(TcpClientTest, NonBlockingPollNoData) {
    TcpClient client;
    ASSERT_TRUE(client.connect_to("127.0.0.1", server_->port()));
    ASSERT_TRUE(client.set_nonblocking());

    // No data sent — poll should return false immediately.
    EXPECT_FALSE(client.poll_readable(0));
}

TEST_F(TcpClientTest, NonBlockingPollWithData) {
    TcpClient client;
    ASSERT_TRUE(client.connect_to("127.0.0.1", server_->port()));

    for (int i = 0; i < 100 && !connected_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Send a message so the echo comes back.
    const char payload[] = "poll test";
    ASSERT_TRUE(client.send_message(payload, sizeof(payload) - 1));

    // Wait for the echo to arrive.
    ASSERT_TRUE(client.set_nonblocking());
    bool readable = false;
    for (int i = 0; i < 100 && !readable; ++i) {
        readable = client.poll_readable(10);
    }
    EXPECT_TRUE(readable);
}

TEST_F(TcpClientTest, ConnectRefused) {
    TcpClient client;
    // Port 1 is almost certainly not listening.
    EXPECT_FALSE(client.connect_to("127.0.0.1", 1));
    EXPECT_FALSE(client.is_connected());
}

TEST_F(TcpClientTest, MoveSemantics) {
    TcpClient client;
    ASSERT_TRUE(client.connect_to("127.0.0.1", server_->port()));
    int original_fd = client.fd();
    EXPECT_GE(original_fd, 0);

    // Move construct.
    TcpClient moved(std::move(client));
    EXPECT_EQ(moved.fd(), original_fd);
    EXPECT_EQ(client.fd(), -1);

    // Move assign.
    TcpClient assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.fd(), original_fd);
    EXPECT_EQ(moved.fd(), -1);
}

TEST_F(TcpClientTest, MultipleMessages) {
    TcpClient client;
    ASSERT_TRUE(client.connect_to("127.0.0.1", server_->port()));

    for (int i = 0; i < 100 && !connected_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Send and receive multiple messages in sequence.
    for (int i = 0; i < 5; ++i) {
        std::string msg = "msg_" + std::to_string(i);
        ASSERT_TRUE(client.send_message(msg.data(), msg.size()));
        auto resp = client.recv_message();
        ASSERT_FALSE(resp.empty()) << "Failed on message " << i;
        EXPECT_EQ(std::string(resp.data(), resp.size()), msg);
    }
}

}  // namespace
}  // namespace exchange
