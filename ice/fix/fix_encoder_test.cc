#include "ice/fix/fix_encoder.h"

#include <gtest/gtest.h>

#include <string>

namespace exchange::ice::fix {
namespace {

// Use SOH from fix_encoder.h — no duplicate definition.

// Helper: extract a FIX tag value from a message string.
// Returns empty string if tag not found.
std::string get_tag(const std::string& msg, const std::string& tag) {
    std::string prefix = tag + "=";
    auto pos = msg.find(SOH + prefix);
    if (pos == std::string::npos) {
        // Check if it's at the start of the message
        if (msg.substr(0, prefix.size()) == prefix) {
            pos = 0;
        } else {
            return "";
        }
    } else {
        pos += 1;  // skip the SOH
    }
    auto eq = msg.find('=', pos);
    auto end = msg.find(SOH, eq);
    if (end == std::string::npos) return "";
    return msg.substr(eq + 1, end - eq - 1);
}

// Verify FIX checksum: sum of all bytes before "10=", mod 256, zero-padded 3 digits.
bool verify_checksum(const std::string& msg) {
    std::string tag10_prefix;
    tag10_prefix += SOH;
    tag10_prefix += "10=";
    auto pos = msg.find(tag10_prefix);
    if (pos == std::string::npos) return false;

    uint32_t sum = 0;
    for (size_t i = 0; i <= pos; ++i) {
        sum += static_cast<uint8_t>(msg[i]);
    }
    uint8_t expected = sum % 256;

    std::string cksum_str = get_tag(msg, "10");
    int cksum_val = std::stoi(cksum_str);
    return cksum_val == expected;
}

EncodeContext make_ctx() {
    EncodeContext ctx;
    ctx.sender_comp_id = "ICE_SIM";
    ctx.target_comp_id = "CLIENT1";
    ctx.symbol = "WTI";
    ctx.next_exec_id = 1000;
    ctx.next_seq_num = 1;
    return ctx;
}

// --- encode_exec_new ---

TEST(FixEncoderTest, ExecNewHasRequiredTags) {
    auto ctx = make_ctx();
    OrderAccepted evt{.id = 42, .client_order_id = 100, .ts = 1700000000000000000LL};
    Order order{};
    order.id = 42;
    order.client_order_id = 100;
    order.price = 50000000;  // 5000.0000
    order.quantity = 100000; // 10.0000
    order.side = Side::Buy;

    std::string msg = encode_exec_new(evt, order, ctx);

    EXPECT_EQ(get_tag(msg, "8"), "FIX.4.2");
    EXPECT_EQ(get_tag(msg, "35"), "8");
    EXPECT_EQ(get_tag(msg, "49"), "ICE_SIM");
    EXPECT_EQ(get_tag(msg, "56"), "CLIENT1");
    EXPECT_EQ(get_tag(msg, "37"), "42");
    EXPECT_EQ(get_tag(msg, "11"), "100");
    EXPECT_EQ(get_tag(msg, "150"), "0");  // ExecType=New
    EXPECT_EQ(get_tag(msg, "39"), "0");   // OrdStatus=New
    EXPECT_EQ(get_tag(msg, "55"), "WTI");
    EXPECT_EQ(get_tag(msg, "54"), "1");   // Side=Buy
    EXPECT_EQ(get_tag(msg, "44"), "5000.0000");
    EXPECT_EQ(get_tag(msg, "38"), "10.0000");
    EXPECT_EQ(get_tag(msg, "151"), "10.0000");  // LeavesQty = full qty
    EXPECT_EQ(get_tag(msg, "14"), "0.0000");    // CumQty = 0
    EXPECT_EQ(get_tag(msg, "6"), "0.0000");     // AvgPx = 0
    EXPECT_FALSE(get_tag(msg, "34").empty());   // MsgSeqNum present
    EXPECT_FALSE(get_tag(msg, "52").empty());   // SendingTime present
    EXPECT_FALSE(get_tag(msg, "17").empty());   // ExecID present
}

TEST(FixEncoderTest, ExecNewChecksum) {
    auto ctx = make_ctx();
    OrderAccepted evt{.id = 1, .client_order_id = 1, .ts = 1700000000000000000LL};
    Order order{};
    order.id = 1;
    order.client_order_id = 1;
    order.price = 10000;   // 1.0000
    order.quantity = 10000; // 1.0000
    order.side = Side::Sell;

    std::string msg = encode_exec_new(evt, order, ctx);
    EXPECT_TRUE(verify_checksum(msg));
}

TEST(FixEncoderTest, ExecNewBodyLength) {
    auto ctx = make_ctx();
    OrderAccepted evt{.id = 5, .client_order_id = 5, .ts = 1700000000000000000LL};
    Order order{};
    order.id = 5;
    order.price = 10000;
    order.quantity = 10000;
    order.side = Side::Buy;

    std::string msg = encode_exec_new(evt, order, ctx);

    // BodyLength (tag 9) should equal bytes from after 9=...SOH to before 10=
    int body_len = std::stoi(get_tag(msg, "9"));

    // Find end of tag 9
    std::string nine_prefix = "9=";
    auto nine_pos = msg.find(nine_prefix);
    auto nine_soh = msg.find(SOH, nine_pos);
    size_t body_start = nine_soh + 1;

    // Find start of tag 10
    std::string tag10_prefix;
    tag10_prefix += SOH;
    tag10_prefix += "10=";
    auto ten_pos = msg.find(tag10_prefix);
    size_t actual_body_len = ten_pos + 1 - body_start;  // include the SOH before 10=

    EXPECT_EQ(static_cast<size_t>(body_len), actual_body_len);
}

TEST(FixEncoderTest, ExecNewSellSide) {
    auto ctx = make_ctx();
    OrderAccepted evt{.id = 7, .client_order_id = 7, .ts = 0};
    Order order{};
    order.id = 7;
    order.price = 10000;
    order.quantity = 10000;
    order.side = Side::Sell;

    std::string msg = encode_exec_new(evt, order, ctx);
    EXPECT_EQ(get_tag(msg, "54"), "2");  // Side=Sell
}

// --- encode_exec_fill ---

TEST(FixEncoderTest, ExecFillHasRequiredTags) {
    auto ctx = make_ctx();
    OrderFilled evt{};
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.price = 50050000;   // 5005.0000
    evt.quantity = 30000;   // 3.0000
    evt.ts = 1700000000000000000LL;

    Order order{};
    order.id = 20;
    order.client_order_id = 200;
    order.price = 50050000;
    order.quantity = 100000;  // 10.0000
    order.filled_quantity = 0;
    order.remaining_quantity = 100000;
    order.side = Side::Sell;

    std::string msg = encode_exec_fill(evt, order, ctx);

    EXPECT_EQ(get_tag(msg, "35"), "8");
    EXPECT_EQ(get_tag(msg, "150"), "1");  // ExecType=PartialFill (FIX 4.2)
    EXPECT_EQ(get_tag(msg, "39"), "1");   // OrdStatus=PartiallyFilled (3 of 10 filled)
    EXPECT_EQ(get_tag(msg, "37"), "20");
    EXPECT_EQ(get_tag(msg, "11"), "200");
    EXPECT_EQ(get_tag(msg, "31"), "5005.0000");  // LastPx
    EXPECT_EQ(get_tag(msg, "32"), "3.0000");     // LastQty
    EXPECT_EQ(get_tag(msg, "151"), "7.0000");    // LeavesQty = 10 - 3
    EXPECT_EQ(get_tag(msg, "14"), "3.0000");     // CumQty
    EXPECT_EQ(get_tag(msg, "6"), "5005.0000");   // AvgPx (single fill)
    EXPECT_TRUE(verify_checksum(msg));
}

TEST(FixEncoderTest, ExecFillFullyFilled) {
    auto ctx = make_ctx();
    OrderFilled evt{};
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.price = 50050000;
    evt.quantity = 100000;  // 10.0000 — fills entire order
    evt.ts = 0;

    Order order{};
    order.id = 20;
    order.client_order_id = 200;
    order.price = 50050000;
    order.quantity = 100000;
    order.filled_quantity = 0;
    order.remaining_quantity = 100000;
    order.side = Side::Buy;

    std::string msg = encode_exec_fill(evt, order, ctx);

    EXPECT_EQ(get_tag(msg, "150"), "2");        // ExecType=Fill (FIX 4.2)
    EXPECT_EQ(get_tag(msg, "39"), "2");        // OrdStatus=Filled
    EXPECT_EQ(get_tag(msg, "151"), "0.0000");  // LeavesQty = 0
    EXPECT_EQ(get_tag(msg, "14"), "10.0000");  // CumQty = 10
}

// --- encode_exec_cancel ---

TEST(FixEncoderTest, ExecCancelHasRequiredTags) {
    auto ctx = make_ctx();
    OrderCancelled evt{.id = 50, .ts = 1700000000000000000LL, .reason = CancelReason::UserRequested};

    Order order{};
    order.id = 50;
    order.client_order_id = 500;
    order.price = 40000000;  // 4000.0000
    order.quantity = 50000;  // 5.0000
    order.filled_quantity = 10000;  // 1.0000
    order.side = Side::Buy;

    std::string msg = encode_exec_cancel(evt, order, ctx);

    EXPECT_EQ(get_tag(msg, "35"), "8");
    EXPECT_EQ(get_tag(msg, "150"), "4");  // ExecType=Cancelled
    EXPECT_EQ(get_tag(msg, "39"), "4");   // OrdStatus=Cancelled
    EXPECT_EQ(get_tag(msg, "37"), "50");
    EXPECT_EQ(get_tag(msg, "11"), "500");
    EXPECT_EQ(get_tag(msg, "44"), "4000.0000");
    EXPECT_EQ(get_tag(msg, "38"), "5.0000");
    EXPECT_EQ(get_tag(msg, "151"), "0.0000");    // LeavesQty = 0 (cancelled)
    EXPECT_EQ(get_tag(msg, "14"), "1.0000");     // CumQty = filled_quantity
    EXPECT_EQ(get_tag(msg, "6"), "0.0000");      // AvgPx = 0
    EXPECT_TRUE(verify_checksum(msg));
}

// --- encode_exec_reject ---

TEST(FixEncoderTest, ExecRejectHasRequiredTags) {
    auto ctx = make_ctx();
    OrderRejected evt{.client_order_id = 999, .ts = 1700000000000000000LL, .reason = RejectReason::InvalidPrice};

    std::string msg = encode_exec_reject(evt, ctx);

    EXPECT_EQ(get_tag(msg, "35"), "8");
    EXPECT_EQ(get_tag(msg, "150"), "8");  // ExecType=Rejected
    EXPECT_EQ(get_tag(msg, "39"), "8");   // OrdStatus=Rejected
    EXPECT_EQ(get_tag(msg, "11"), "999");
    EXPECT_TRUE(get_tag(msg, "37").empty());    // OrderID omitted on reject
    EXPECT_TRUE(get_tag(msg, "54").empty());    // Side omitted on reject
    EXPECT_FALSE(get_tag(msg, "103").empty());  // OrdRejReason present
    EXPECT_TRUE(verify_checksum(msg));
}

// --- encode_exec_replace ---

TEST(FixEncoderTest, ExecReplaceHasRequiredTags) {
    auto ctx = make_ctx();
    OrderModified evt{};
    evt.id = 60;
    evt.client_order_id = 600;
    evt.new_price = 51000000;  // 5100.0000
    evt.new_qty = 200000;      // 20.0000
    evt.ts = 1700000000000000000LL;

    Order order{};
    order.id = 60;
    order.client_order_id = 600;
    order.price = 50000000;
    order.quantity = 100000;
    order.filled_quantity = 0;
    order.side = Side::Sell;

    std::string msg = encode_exec_replace(evt, order, ctx);

    EXPECT_EQ(get_tag(msg, "35"), "8");
    EXPECT_EQ(get_tag(msg, "150"), "5");  // ExecType=Replace
    EXPECT_EQ(get_tag(msg, "39"), "0");   // OrdStatus=New (replaced = active)
    EXPECT_EQ(get_tag(msg, "37"), "60");
    EXPECT_EQ(get_tag(msg, "11"), "600");
    EXPECT_EQ(get_tag(msg, "44"), "5100.0000");   // new price
    EXPECT_EQ(get_tag(msg, "38"), "20.0000");     // new qty
    EXPECT_EQ(get_tag(msg, "151"), "20.0000");    // LeavesQty = new_qty - filled
    EXPECT_EQ(get_tag(msg, "14"), "0.0000");      // CumQty = filled_quantity
    EXPECT_EQ(get_tag(msg, "6"), "0.0000");       // AvgPx = 0
    EXPECT_TRUE(verify_checksum(msg));
}

// --- Sequence number increment ---

TEST(FixEncoderTest, SeqNumIncrementsAcrossCalls) {
    auto ctx = make_ctx();
    OrderAccepted evt{.id = 1, .client_order_id = 1, .ts = 0};
    Order order{};
    order.id = 1;
    order.price = 10000;
    order.quantity = 10000;
    order.side = Side::Buy;

    std::string msg1 = encode_exec_new(evt, order, ctx);
    std::string msg2 = encode_exec_new(evt, order, ctx);

    int seq1 = std::stoi(get_tag(msg1, "34"));
    int seq2 = std::stoi(get_tag(msg2, "34"));
    EXPECT_EQ(seq2, seq1 + 1);
}

// --- Price conversion ---

TEST(FixEncoderTest, PriceConversion) {
    EXPECT_EQ(price_to_fix_str(50000000), "5000.0000");
    EXPECT_EQ(price_to_fix_str(10000), "1.0000");
    EXPECT_EQ(price_to_fix_str(15555), "1.5555");
    EXPECT_EQ(price_to_fix_str(0), "0.0000");
    EXPECT_EQ(price_to_fix_str(-50000000), "-5000.0000");
}

// --- Quantity conversion ---

TEST(FixEncoderTest, QuantityConversion) {
    EXPECT_EQ(qty_to_fix_str(100000), "10.0000");
    EXPECT_EQ(qty_to_fix_str(10000), "1.0000");
    EXPECT_EQ(qty_to_fix_str(0), "0.0000");
}

}  // namespace
}  // namespace exchange::ice::fix
