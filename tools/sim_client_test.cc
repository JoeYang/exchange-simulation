#include "tools/sim_client.h"
#include "tools/tcp_server.h"
#include "tools/trading_strategy.h"

#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"
#include "cme/codec/ilink3_messages.h"
#include "exchange-core/types.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace exchange {
namespace {

namespace ilink3 = cme::sbe::ilink3;

// Helper: TcpServer that echoes back exec reports for testing.
class SimClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        TcpServer::Config cfg{};
        cfg.port = 0;
        cfg.on_connect = [this](int fd) {
            client_fd_.store(fd);
        };
        cfg.on_message = [this](int fd, const char* data, size_t len) {
            handle_client_message(fd, data, len);
        };
        cfg.on_disconnect = [](int) {};
        server_ = std::make_unique<TcpServer>(cfg);
        server_thread_ = std::thread([this] {
            while (!stop_.load()) server_->poll(10);
        });
    }

    void TearDown() override {
        stop_.store(true);
        if (server_thread_.joinable()) server_thread_.join();
        server_->shutdown();
    }

    // Reply with ExecutionReportNew522 (accepted).
    void send_accept(int fd, uint64_t order_id, uint64_t cl_ord_id,
                     uint8_t side, Price price, Quantity qty)
    {
        OrderAccepted evt{};
        evt.id = order_id;
        evt.client_order_id = cl_ord_id;
        evt.ts = now_ns();

        Order order{};
        order.id = order_id;
        order.client_order_id = cl_ord_id;
        order.side = (side == 2) ? Side::Sell : Side::Buy;
        order.price = price;
        order.quantity = qty;

        ilink3::EncodeContext ctx{};
        ctx.seq_num = 1;
        ctx.uuid = 1;
        ctx.security_id = 1;
        std::memcpy(ctx.sender_id, "SIM", 3);
        std::memcpy(ctx.location, "US,IL", 5);
        ctx.party_details_list_req_id = 1;

        char buf[512];
        size_t n = ilink3::encode_exec_new(buf, evt, order, ctx);
        server_->send_message(fd, buf, n);
    }

    // Reply with ExecutionReportTradeOutright525 (fill).
    void send_fill(int fd, uint64_t order_id, uint64_t cl_ord_id,
                   uint8_t side, Price fill_price, Quantity fill_qty,
                   Quantity leaves_qty)
    {
        OrderFilled evt{};
        evt.aggressor_id = order_id;
        evt.resting_id = 99;
        evt.price = fill_price;
        evt.quantity = fill_qty;
        evt.ts = now_ns();

        Order order{};
        order.id = order_id;
        order.client_order_id = cl_ord_id;
        order.side = (side == 2) ? Side::Sell : Side::Buy;
        order.price = fill_price;
        order.quantity = fill_qty + leaves_qty;
        order.filled_quantity = 0;

        ilink3::EncodeContext ctx{};
        ctx.seq_num = 2;
        ctx.uuid = 1;
        ctx.security_id = 1;
        std::memcpy(ctx.sender_id, "SIM", 3);
        std::memcpy(ctx.location, "US,IL", 5);
        ctx.party_details_list_req_id = 1;

        char buf[512];
        size_t n = ilink3::encode_exec_fill(buf, evt, order, true, ctx);
        server_->send_message(fd, buf, n);
    }

    // Reply with ExecutionReportCancel534.
    void send_cancel_ack(int fd, uint64_t order_id, uint64_t cl_ord_id) {
        OrderCancelled evt{};
        evt.id = order_id;
        evt.ts = now_ns();
        evt.reason = CancelReason::UserRequested;

        Order order{};
        order.id = order_id;
        order.client_order_id = cl_ord_id;
        order.side = Side::Buy;
        order.price = 50000000;
        order.quantity = 10000;

        ilink3::EncodeContext ctx{};
        ctx.seq_num = 3;
        ctx.uuid = 1;
        ctx.security_id = 1;
        std::memcpy(ctx.sender_id, "SIM", 3);
        std::memcpy(ctx.location, "US,IL", 5);
        ctx.party_details_list_req_id = 1;

        char buf[512];
        size_t n = ilink3::encode_exec_cancel(buf, evt, order, ctx);
        server_->send_message(fd, buf, n);
    }

    void handle_client_message(int fd, const char* data, size_t len) {
        ilink3::decode_ilink3_message(data, len, [&](const auto& decoded) {
            using T = std::decay_t<decltype(decoded)>;
            if constexpr (std::is_same_v<T, ilink3::DecodedNewOrder514>) {
                uint64_t clid = ilink3::decode_cl_ord_id(decoded.root.cl_ord_id);
                Price price = ilink3::price9_to_engine(decoded.root.price);
                Quantity qty = ilink3::wire_qty_to_engine(decoded.root.order_qty);
                uint64_t oid = next_order_id_++;
                send_accept(fd, oid, clid, decoded.root.side, price, qty);

                if (auto_fill_.load()) {
                    send_fill(fd, oid, clid, decoded.root.side,
                              price, qty, 0);
                }
            } else if constexpr (std::is_same_v<T, ilink3::DecodedCancelRequest516>) {
                uint64_t clid = ilink3::decode_cl_ord_id(decoded.root.cl_ord_id);
                send_cancel_ack(fd, decoded.root.order_id, clid);
            }
        });
    }

    std::unique_ptr<TcpServer> server_;
    std::thread server_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> client_fd_{-1};
    std::atomic<bool> auto_fill_{false};
    uint64_t next_order_id_{100};
};

void wait_connected(const std::atomic<int>& fd) {
    for (int i = 0; i < 100 && fd.load() < 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST_F(SimClientTest, ConnectAndSendNewOrder) {
    auto codec = std::make_unique<CmeCodec>(1, "TESTER");
    SimClient client(std::move(codec), "ES");

    ASSERT_TRUE(client.connect("127.0.0.1", server_->port()));
    wait_connected(client_fd_);

    ASSERT_TRUE(client.send_new_order(1, Side::Buy, 50000000, 10000));

    int responses = 0;
    for (int i = 0; i < 100 && responses == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        responses = client.poll_responses();
    }
    EXPECT_GT(responses, 0);
    EXPECT_EQ(client.open_orders().size(), 1u);
    EXPECT_EQ(client.open_orders().begin()->second.cl_ord_id, 1u);
}

TEST_F(SimClientTest, FillUpdatesPositionAndPnl) {
    auto_fill_.store(true);

    auto codec = std::make_unique<CmeCodec>(1, "TESTER");
    SimClient client(std::move(codec), "ES");
    ASSERT_TRUE(client.connect("127.0.0.1", server_->port()));
    wait_connected(client_fd_);

    client.send_new_order(1, Side::Buy, 50000000, 10000);

    int total = 0;
    for (int i = 0; i < 100 && total < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        total += client.poll_responses();
    }
    EXPECT_GE(total, 2);
    EXPECT_EQ(client.fill_count(), 1u);
    EXPECT_EQ(client.position(), 10000);
    EXPECT_EQ(client.last_fill_price(), 50000000);
    EXPECT_EQ(client.open_orders().size(), 0u);
}

TEST_F(SimClientTest, CancelRemovesOpenOrder) {
    auto codec = std::make_unique<CmeCodec>(1, "TESTER");
    SimClient client(std::move(codec), "ES");
    ASSERT_TRUE(client.connect("127.0.0.1", server_->port()));
    wait_connected(client_fd_);

    client.send_new_order(1, Side::Buy, 50000000, 10000);

    int total = 0;
    for (int i = 0; i < 100 && total < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        total += client.poll_responses();
    }
    EXPECT_EQ(client.open_orders().size(), 1u);

    ASSERT_TRUE(client.send_cancel(1));

    for (int i = 0; i < 100 && client.open_orders().size() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        client.poll_responses();
    }
    EXPECT_EQ(client.open_orders().size(), 0u);
}

TEST_F(SimClientTest, JournalOutput) {
    std::string journal_path = "/tmp/sim_client_test_journal.txt";

    auto codec = std::make_unique<CmeCodec>(1, "TESTER");
    SimClient client(std::move(codec), "ES");
    ASSERT_TRUE(client.open_journal(journal_path.c_str()));
    ASSERT_TRUE(client.connect("127.0.0.1", server_->port()));
    wait_connected(client_fd_);

    client.send_new_order(1, Side::Buy, 50000000, 10000);

    int total = 0;
    for (int i = 0; i < 100 && total < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        total += client.poll_responses();
    }

    client.close_journal();

    std::ifstream f(journal_path);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("ACTION ILINK3_NEW_ORDER"), std::string::npos);
    EXPECT_NE(content.find("cl_ord_id=1"), std::string::npos);
    EXPECT_NE(content.find("side=BUY"), std::string::npos);
    EXPECT_NE(content.find("EXPECT ORDER_ACCEPTED"), std::string::npos);

    std::filesystem::remove(journal_path);
}

TEST_F(SimClientTest, StatusLine) {
    auto codec = std::make_unique<CmeCodec>(1, "TESTER");
    SimClient client(std::move(codec), "ES");

    std::string status = client.status_line();
    EXPECT_NE(status.find("ES"), std::string::npos);
    EXPECT_NE(status.find("pos=0"), std::string::npos);
    EXPECT_NE(status.find("fills=0"), std::string::npos);
}

TEST_F(SimClientTest, SyncStatePopulatesClientState) {
    auto codec = std::make_unique<CmeCodec>(1, "TESTER");
    SimClient client(std::move(codec), "ES");
    ASSERT_TRUE(client.connect("127.0.0.1", server_->port()));
    wait_connected(client_fd_);

    client.send_new_order(1, Side::Buy, 50000000, 10000);

    int total = 0;
    for (int i = 0; i < 100 && total < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        total += client.poll_responses();
    }

    client.sync_state();
    EXPECT_EQ(client.state().open_orders.size(), 1u);
    EXPECT_EQ(client.state().position, 0);
    EXPECT_EQ(client.state().fill_count, 0u);
}

}  // namespace
}  // namespace exchange
