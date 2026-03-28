#include "krx/fix/krx_fix_exec_publisher.h"

#include <gtest/gtest.h>

#include <string>

namespace exchange::krx::fix {
namespace {

// Use SOH from ice::fix::fix_encoder.h
using ::exchange::ice::fix::SOH;

// Helper: extract a FIX tag value from a message string.
std::string get_tag(const std::string& msg, const std::string& tag) {
    std::string prefix = tag + "=";
    auto pos = msg.find(SOH + prefix);
    if (pos == std::string::npos) {
        if (msg.substr(0, prefix.size()) == prefix) {
            pos = 0;
        } else {
            return "";
        }
    } else {
        pos += 1;
    }
    auto eq = msg.find('=', pos);
    auto end = msg.find(SOH, eq);
    if (end == std::string::npos) return "";
    return msg.substr(eq + 1, end - eq - 1);
}

class KrxFixExecPublisherTest : public ::testing::Test {
protected:
    void SetUp() override {
        publisher_ = std::make_unique<KrxFixExecPublisher>(
            "KRX_SIM", "CLIENT1", "KOSPI200");
    }

    void register_buy_order(OrderId id, uint64_t client_id,
                            Price price, Quantity qty) {
        publisher_->register_order(id, client_id, price, qty, Side::Buy);
    }

    void register_sell_order(OrderId id, uint64_t client_id,
                             Price price, Quantity qty) {
        publisher_->register_order(id, client_id, price, qty, Side::Sell);
    }

    std::unique_ptr<KrxFixExecPublisher> publisher_;
};

// --- on_order_accepted ---

TEST_F(KrxFixExecPublisherTest, AcceptedProducesExecNew) {
    register_buy_order(1, 100, 3502500, 100000);  // 350.2500, qty 10

    OrderAccepted evt{.id = 1, .client_order_id = 100,
                      .ts = 1700000000000000000LL};
    publisher_->on_order_accepted(evt);

    ASSERT_EQ(publisher_->messages().size(), 1u);
    const auto& msg = publisher_->messages()[0];

    EXPECT_EQ(get_tag(msg, "35"), "8");
    EXPECT_EQ(get_tag(msg, "150"), "0");  // ExecType=New
    EXPECT_EQ(get_tag(msg, "39"), "0");   // OrdStatus=New
    EXPECT_EQ(get_tag(msg, "37"), "1");
    EXPECT_EQ(get_tag(msg, "11"), "100");
    EXPECT_EQ(get_tag(msg, "55"), "KOSPI200");
    EXPECT_EQ(get_tag(msg, "54"), "1");   // Buy
    EXPECT_EQ(get_tag(msg, "44"), "350.2500");
    EXPECT_EQ(get_tag(msg, "38"), "10.0000");
    EXPECT_EQ(get_tag(msg, "49"), "KRX_SIM");
    EXPECT_EQ(get_tag(msg, "56"), "CLIENT1");
}

// --- on_order_filled ---

TEST_F(KrxFixExecPublisherTest, FilledProducesExecFill) {
    register_sell_order(20, 200, 3505000, 100000);  // 350.5000, qty 10

    publisher_->on_order_accepted(
        OrderAccepted{.id = 20, .client_order_id = 200, .ts = 0});

    OrderFilled evt{};
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.price = 3505000;
    evt.quantity = 30000;  // 3.0000
    evt.ts = 1700000000000000000LL;

    publisher_->on_order_filled(evt);

    ASSERT_EQ(publisher_->messages().size(), 2u);
    const auto& msg = publisher_->messages()[1];

    EXPECT_EQ(get_tag(msg, "150"), "1");          // ExecType=PartialFill
    EXPECT_EQ(get_tag(msg, "39"), "1");
    EXPECT_EQ(get_tag(msg, "37"), "20");
    EXPECT_EQ(get_tag(msg, "31"), "350.5000");    // LastPx
    EXPECT_EQ(get_tag(msg, "32"), "3.0000");      // LastQty
    EXPECT_EQ(get_tag(msg, "151"), "7.0000");     // LeavesQty
    EXPECT_EQ(get_tag(msg, "14"), "3.0000");      // CumQty
}

TEST_F(KrxFixExecPublisherTest, FullFillProducesExecTypeFill) {
    register_buy_order(30, 300, 3500000, 100000);  // qty 10.0000

    publisher_->on_order_accepted(
        OrderAccepted{.id = 30, .client_order_id = 300, .ts = 0});

    // First fill: 4.0000
    OrderFilled f1{};
    f1.aggressor_id = 99;
    f1.resting_id = 30;
    f1.price = 3500000;
    f1.quantity = 40000;
    f1.ts = 0;
    publisher_->on_order_filled(f1);

    // Second fill: 6.0000 (fully filled)
    OrderFilled f2{};
    f2.aggressor_id = 98;
    f2.resting_id = 30;
    f2.price = 3500000;
    f2.quantity = 60000;
    f2.ts = 0;
    publisher_->on_order_filled(f2);

    ASSERT_EQ(publisher_->messages().size(), 3u);
    const auto& msg2 = publisher_->messages()[2];

    EXPECT_EQ(get_tag(msg2, "150"), "2");         // ExecType=Fill
    EXPECT_EQ(get_tag(msg2, "39"), "2");
    EXPECT_EQ(get_tag(msg2, "14"), "10.0000");    // CumQty = 4 + 6
    EXPECT_EQ(get_tag(msg2, "151"), "0.0000");    // LeavesQty = 0
}

// --- on_order_cancelled ---

TEST_F(KrxFixExecPublisherTest, CancelledProducesExecCancel) {
    register_buy_order(50, 500, 3450000, 50000);
    publisher_->on_order_accepted(
        OrderAccepted{.id = 50, .client_order_id = 500, .ts = 0});

    OrderCancelled evt{.id = 50, .ts = 1700000000000000000LL,
                       .reason = CancelReason::UserRequested};
    publisher_->on_order_cancelled(evt);

    ASSERT_EQ(publisher_->messages().size(), 2u);
    const auto& msg = publisher_->messages()[1];

    EXPECT_EQ(get_tag(msg, "150"), "4");  // Cancelled
    EXPECT_EQ(get_tag(msg, "39"), "4");
    EXPECT_EQ(get_tag(msg, "37"), "50");
    EXPECT_EQ(get_tag(msg, "11"), "500");
}

// --- on_order_rejected ---

TEST_F(KrxFixExecPublisherTest, RejectedProducesExecReject) {
    OrderRejected evt{.client_order_id = 999,
                      .ts = 1700000000000000000LL,
                      .reason = RejectReason::InvalidPrice};
    publisher_->on_order_rejected(evt);

    ASSERT_EQ(publisher_->messages().size(), 1u);
    const auto& msg = publisher_->messages()[0];

    EXPECT_EQ(get_tag(msg, "150"), "8");  // Rejected
    EXPECT_EQ(get_tag(msg, "39"), "8");
    EXPECT_EQ(get_tag(msg, "11"), "999");
}

// --- on_order_modified ---

TEST_F(KrxFixExecPublisherTest, ModifiedProducesExecReplace) {
    register_sell_order(60, 600, 3500000, 100000);
    publisher_->on_order_accepted(
        OrderAccepted{.id = 60, .client_order_id = 600, .ts = 0});

    OrderModified evt{};
    evt.id = 60;
    evt.client_order_id = 600;
    evt.new_price = 3550000;
    evt.new_qty = 200000;
    evt.ts = 1700000000000000000LL;

    publisher_->on_order_modified(evt);

    ASSERT_EQ(publisher_->messages().size(), 2u);
    const auto& msg = publisher_->messages()[1];

    EXPECT_EQ(get_tag(msg, "150"), "5");       // Replace
    EXPECT_EQ(get_tag(msg, "39"), "0");        // New (active)
    EXPECT_EQ(get_tag(msg, "44"), "355.0000");
    EXPECT_EQ(get_tag(msg, "38"), "20.0000");
}

// --- clear ---

TEST_F(KrxFixExecPublisherTest, ClearRemovesMessages) {
    OrderRejected evt{.client_order_id = 1, .ts = 0,
                      .reason = RejectReason::InvalidPrice};
    publisher_->on_order_rejected(evt);
    ASSERT_EQ(publisher_->messages().size(), 1u);

    publisher_->clear_messages();
    EXPECT_EQ(publisher_->messages().size(), 0u);
}

// --- unknown order on fill is silently skipped ---

TEST_F(KrxFixExecPublisherTest, FillForUnknownOrderSkipped) {
    OrderFilled evt{};
    evt.aggressor_id = 99;
    evt.resting_id = 999;  // not registered
    evt.price = 3500000;
    evt.quantity = 10000;
    evt.ts = 0;

    publisher_->on_order_filled(evt);
    EXPECT_EQ(publisher_->messages().size(), 0u);
}

// --- SeqNum increments across messages ---

TEST_F(KrxFixExecPublisherTest, SeqNumIncrements) {
    OrderRejected r1{.client_order_id = 1, .ts = 0,
                     .reason = RejectReason::InvalidPrice};
    OrderRejected r2{.client_order_id = 2, .ts = 0,
                     .reason = RejectReason::InvalidPrice};
    publisher_->on_order_rejected(r1);
    publisher_->on_order_rejected(r2);

    int seq1 = std::stoi(get_tag(publisher_->messages()[0], "34"));
    int seq2 = std::stoi(get_tag(publisher_->messages()[1], "34"));
    EXPECT_EQ(seq2, seq1 + 1);
}

}  // namespace
}  // namespace exchange::krx::fix
