#include "tools/ice_recovery.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "ice/impact/impact_encoder.h"
#include "ice/impact/impact_messages.h"
#include "tools/display_state.h"
#include "tools/tcp_server.h"
#include "gtest/gtest.h"

namespace {

using namespace exchange::ice::impact;

// ---------------------------------------------------------------------------
// Helper: encode a canned snapshot into buf. Returns total bytes.
// Encodes BundleStart + N SnapshotOrders + BundleEnd(seq=0).
// ---------------------------------------------------------------------------

size_t encode_canned_snapshot(char* buf, size_t buf_len,
                               int32_t instrument_id,
                               const int64_t* bid_prices, const int32_t* bid_qtys,
                               int nbids,
                               const int64_t* ask_prices, const int32_t* ask_qtys,
                               int nasks) {
    uint16_t msg_count = static_cast<uint16_t>(nbids + nasks);
    char* p = write_bundle_start(buf, buf_len, 0, msg_count, 0);
    if (!p) return 0;

    int64_t next_oid = 1;
    for (int i = 0; i < nbids; ++i) {
        SnapshotOrder msg{};
        std::memset(&msg, 0, sizeof(msg));
        msg.instrument_id = instrument_id;
        msg.order_id = next_oid++;
        msg.side = static_cast<uint8_t>(Side::Buy);
        msg.price = bid_prices[i];
        msg.quantity = static_cast<uint32_t>(bid_qtys[i]);
        size_t rem = buf_len - static_cast<size_t>(p - buf);
        p = encode(p, rem, msg);
        if (!p) return 0;
    }
    for (int i = 0; i < nasks; ++i) {
        SnapshotOrder msg{};
        std::memset(&msg, 0, sizeof(msg));
        msg.instrument_id = instrument_id;
        msg.order_id = next_oid++;
        msg.side = static_cast<uint8_t>(Side::Sell);
        msg.price = ask_prices[i];
        msg.quantity = static_cast<uint32_t>(ask_qtys[i]);
        size_t rem = buf_len - static_cast<size_t>(p - buf);
        p = encode(p, rem, msg);
        if (!p) return 0;
    }
    size_t rem = buf_len - static_cast<size_t>(p - buf);
    p = write_bundle_end(p, rem, 0);
    if (!p) return 0;
    return static_cast<size_t>(p - buf);
}

// RAII helper: runs a TcpServer poll loop in a background thread.
struct ServerRunner {
    exchange::TcpServer& server;
    std::atomic<bool> running{true};
    std::thread thread;

    explicit ServerRunner(exchange::TcpServer& s)
        : server(s), thread([this] {
            while (running.load(std::memory_order_relaxed)) {
                server.poll(10);
            }
        }) {}

    ~ServerRunner() {
        running.store(false);
        thread.join();
        server.shutdown();
    }
};

// ---------------------------------------------------------------------------
// Test: IceRecovery populates DisplayState from snapshot server.
// ---------------------------------------------------------------------------

TEST(IceRecoveryTest, PopulatesDisplayStateFromSnapshot) {
    // Prepare canned snapshot: 2 bids + 1 ask.
    char snap_buf[512];
    int64_t bid_prices[] = {45010000, 45000000};
    int32_t bid_qtys[] = {10, 5};
    int64_t ask_prices[] = {45020000};
    int32_t ask_qtys[] = {8};

    size_t snap_len = encode_canned_snapshot(
        snap_buf, sizeof(snap_buf), 12345,
        bid_prices, bid_qtys, 2,
        ask_prices, ask_qtys, 1);
    ASSERT_GT(snap_len, 0u);

    // Start TCP snapshot server.
    exchange::TcpServer::Config cfg{};
    cfg.port = 0;  // ephemeral
    cfg.on_message = [](int, const char*, size_t) {};

    // We need to capture the server pointer to call send_message from callback.
    exchange::TcpServer* server_ptr = nullptr;
    cfg.on_message = [&](int fd, const char* /*data*/, size_t /*len*/) {
        server_ptr->send_message(fd, snap_buf, snap_len);
    };

    exchange::TcpServer server(cfg);
    server_ptr = &server;
    uint16_t port = server.port();

    ServerRunner runner(server);

    // Recover.
    IceRecovery recovery("127.0.0.1", port, 12345);
    DisplayState ds{};
    recovery.recover("ES", ds);

    // Verify.
    EXPECT_EQ(ds.bid_levels, 2);
    EXPECT_EQ(ds.bids[0].price, 45010000);
    EXPECT_EQ(ds.bids[0].qty, 10);
    EXPECT_EQ(ds.bids[1].price, 45000000);
    EXPECT_EQ(ds.bids[1].qty, 5);

    EXPECT_EQ(ds.ask_levels, 1);
    EXPECT_EQ(ds.asks[0].price, 45020000);
    EXPECT_EQ(ds.asks[0].qty, 8);
}

// ---------------------------------------------------------------------------
// Test: wrong instrument_id is filtered out.
// ---------------------------------------------------------------------------

TEST(IceRecoveryTest, FiltersWrongInstrumentId) {
    char snap_buf[256];
    int64_t bid_prices[] = {45010000};
    int32_t bid_qtys[] = {10};

    // Encode with instrument_id=99999 (wrong).
    size_t snap_len = encode_canned_snapshot(
        snap_buf, sizeof(snap_buf), 99999,
        bid_prices, bid_qtys, 1,
        nullptr, nullptr, 0);
    ASSERT_GT(snap_len, 0u);

    exchange::TcpServer* server_ptr = nullptr;
    exchange::TcpServer::Config cfg{};
    cfg.port = 0;
    cfg.on_message = [&](int fd, const char*, size_t) {
        server_ptr->send_message(fd, snap_buf, snap_len);
    };

    exchange::TcpServer server(cfg);
    server_ptr = &server;

    ServerRunner runner(server);

    // Recovery expects instrument_id=12345 -- should ignore the wrong one.
    IceRecovery recovery("127.0.0.1", server.port(), 12345);
    DisplayState ds{};
    recovery.recover("ES", ds);

    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_EQ(ds.ask_levels, 0);
}

// ---------------------------------------------------------------------------
// Failure test: connection refused.
// ---------------------------------------------------------------------------

TEST(IceRecoveryTest, ConnectionRefusedGracefulFallback) {
    // Port 19999 is almost certainly not listening.
    IceRecovery recovery("127.0.0.1", 19999, 12345);
    DisplayState ds{};
    recovery.recover("ES", ds);

    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_EQ(ds.ask_levels, 0);
}

// ---------------------------------------------------------------------------
// Failure test: server accepts but never responds (timeout).
// ---------------------------------------------------------------------------

TEST(IceRecoveryTest, TimeoutOnNoResponse) {
    exchange::TcpServer::Config cfg{};
    cfg.port = 0;
    cfg.on_message = [](int, const char*, size_t) {};  // no-op

    exchange::TcpServer server(cfg);
    ServerRunner runner(server);

    IceRecovery recovery("127.0.0.1", server.port(), 12345, /*timeout_ms=*/500);
    DisplayState ds{};

    auto start = std::chrono::steady_clock::now();
    recovery.recover("ES", ds);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_EQ(ds.ask_levels, 0);
    EXPECT_LT(elapsed_ms, 3000);  // should timeout around 500ms
}

}  // namespace
