#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"

#include <gtest/gtest.h>

#include <cstring>

namespace exchange::cme::sbe::ilink3 {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static EncodeContext make_ctx() {
    EncodeContext ctx{};
    ctx.seq_num = 42;
    ctx.uuid = 12345;
    ctx.party_details_list_req_id = 99;
    ctx.security_id = 5678;
    std::memcpy(ctx.sender_id, "SENDER01", 8);
    std::memcpy(ctx.location, "US,IL", 5);
    return ctx;
}

// ---------------------------------------------------------------------------
// Price/quantity conversion tests
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, Price9ToEngine) {
    // 100.5000 in engine = 1005000
    // On wire: 1005000 * 100000 = 100500000000
    PRICE9 p{100500000000ll};
    EXPECT_EQ(price9_to_engine(p), 1005000);
}

TEST(ILink3DecoderTest, Price9NullToEngine) {
    PRICE9 p{INT64_NULL};
    EXPECT_EQ(price9_to_engine(p), 0);
}

TEST(ILink3DecoderTest, WireQtyToEngine) {
    // 10 contracts on wire = 10 * 10000 = 100000 engine qty
    EXPECT_EQ(wire_qty_to_engine(10), 100000);
}

TEST(ILink3DecoderTest, WireQtyNullToEngine) {
    EXPECT_EQ(wire_qty_to_engine(UINT32_NULL), 0);
}

// ---------------------------------------------------------------------------
// Enum conversion tests
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, DecodeSide) {
    EXPECT_EQ(decode_side(1), Side::Buy);
    EXPECT_EQ(decode_side(2), Side::Sell);
}

TEST(ILink3DecoderTest, DecodeOrdType) {
    EXPECT_EQ(decode_ord_type(static_cast<uint8_t>(OrdType::Limit)), OrderType::Limit);
    EXPECT_EQ(decode_ord_type(static_cast<uint8_t>(OrdType::MarketWithProtection)), OrderType::Market);
    EXPECT_EQ(decode_ord_type(static_cast<uint8_t>(OrdType::StopWithProtection)), OrderType::Stop);
    EXPECT_EQ(decode_ord_type(static_cast<uint8_t>(OrdType::StopLimit)), OrderType::StopLimit);
}

TEST(ILink3DecoderTest, DecodeTif) {
    EXPECT_EQ(decode_tif(static_cast<uint8_t>(TimeInForce::Day)), exchange::TimeInForce::DAY);
    EXPECT_EQ(decode_tif(static_cast<uint8_t>(TimeInForce::GTC)), exchange::TimeInForce::GTC);
    EXPECT_EQ(decode_tif(static_cast<uint8_t>(TimeInForce::FAK)), exchange::TimeInForce::IOC);
    EXPECT_EQ(decode_tif(static_cast<uint8_t>(TimeInForce::FOK)), exchange::TimeInForce::FOK);
    EXPECT_EQ(decode_tif(static_cast<uint8_t>(TimeInForce::GTD)), exchange::TimeInForce::GTD);
}

TEST(ILink3DecoderTest, DecodeClOrdId) {
    char field[20] = {};
    std::snprintf(field, sizeof(field), "%lu", 12345UL);
    EXPECT_EQ(decode_cl_ord_id(field), 12345u);
}

TEST(ILink3DecoderTest, DecodeClOrdIdZero) {
    char field[20] = {};
    std::snprintf(field, sizeof(field), "%lu", 0UL);
    EXPECT_EQ(decode_cl_ord_id(field), 0u);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: NewOrderSingle514
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripNewOrder) {
    auto ctx = make_ctx();
    OrderRequest req{};
    req.client_order_id = 7;
    req.account_id = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::GTC;
    req.price = 1005000;
    req.quantity = 100000;
    req.stop_price = 0;
    req.timestamp = 9999;
    req.gtd_expiry = 0;
    req.display_qty = 0;

    char buf[1024]{};
    size_t n = encode_new_order(buf, req, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedNewOrder514>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(price9_to_engine(msg.price), 1005000);
            EXPECT_EQ(wire_qty_to_engine(msg.order_qty), 100000);
            EXPECT_EQ(msg.security_id, 5678);
            EXPECT_EQ(decode_side(msg.side), Side::Buy);
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 7u);
            EXPECT_EQ(msg.sending_time_epoch, 9999u);
            EXPECT_EQ(decode_ord_type(msg.ord_type), OrderType::Limit);
            EXPECT_EQ(decode_tif(msg.time_in_force), exchange::TimeInForce::GTC);
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: OrderCancelRequest516
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripCancel) {
    auto ctx = make_ctx();
    char buf[1024]{};
    size_t n = encode_cancel_order(buf, 42, 7, Side::Sell, 5000, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedCancelRequest516>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(msg.order_id, 42u);
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 7u);
            EXPECT_EQ(decode_side(msg.side), Side::Sell);
            EXPECT_EQ(msg.sending_time_epoch, 5000u);
            EXPECT_EQ(msg.security_id, 5678);
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: OrderCancelReplaceRequest515
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripReplace) {
    auto ctx = make_ctx();
    ModifyRequest req{};
    req.order_id = 10;
    req.client_order_id = 20;
    req.new_price = 2000000;
    req.new_quantity = 50000;
    req.timestamp = 7000;

    char buf[1024]{};
    size_t n = encode_modify_order(buf, req, 10, Side::Buy,
                                    OrderType::Limit, exchange::TimeInForce::DAY, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedReplaceRequest515>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(price9_to_engine(msg.price), 2000000);
            EXPECT_EQ(wire_qty_to_engine(msg.order_qty), 50000);
            EXPECT_EQ(msg.order_id, 10u);
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 20u);
            EXPECT_EQ(decode_side(msg.side), Side::Buy);
            EXPECT_EQ(msg.sending_time_epoch, 7000u);
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: OrderMassActionRequest529
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripMassCancel) {
    auto ctx = make_ctx();
    char buf[1024]{};
    size_t n = encode_mass_cancel(buf, MassActionScope::Instrument, 8000, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedMassAction529>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(msg.sending_time_epoch, 8000u);
            EXPECT_EQ(msg.security_id, 5678);
            EXPECT_EQ(static_cast<MassActionScope>(msg.mass_action_scope),
                      MassActionScope::Instrument);
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: ExecutionReportNew522
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripExecNew) {
    auto ctx = make_ctx();
    OrderAccepted evt{};
    evt.id = 1;
    evt.client_order_id = 7;
    evt.ts = 1000;

    Order order{};
    order.id = 1;
    order.client_order_id = 7;
    order.price = 1005000;
    order.quantity = 100000;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = exchange::TimeInForce::GTC;

    char buf[1024]{};
    size_t n = encode_exec_new(buf, evt, order, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedExecNew522>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(msg.order_id, 1u);
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 7u);
            EXPECT_EQ(price9_to_engine(msg.price), 1005000);
            EXPECT_EQ(wire_qty_to_engine(msg.order_qty), 100000);
            EXPECT_EQ(decode_side(msg.side), Side::Buy);
            EXPECT_EQ(msg.transact_time, 1000u);
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: ExecutionReportReject523
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripExecReject) {
    auto ctx = make_ctx();
    OrderRejected evt{};
    evt.client_order_id = 7;
    evt.ts = 2000;
    evt.reason = RejectReason::InvalidPrice;

    char buf[1024]{};
    size_t n = encode_exec_reject(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedExecReject523>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 7u);
            EXPECT_EQ(msg.transact_time, 2000u);
            EXPECT_EQ(msg.ord_rej_reason, static_cast<uint16_t>(RejectReason::InvalidPrice));
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: ExecutionReportTradeOutright525
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripExecFill) {
    auto ctx = make_ctx();
    OrderFilled evt{};
    evt.aggressor_id = 2;
    evt.resting_id = 1;
    evt.price = 1005000;
    evt.quantity = 100000;
    evt.ts = 3000;

    Order order{};
    order.id = 1;
    order.client_order_id = 7;
    order.price = 1005000;
    order.quantity = 100000;
    order.filled_quantity = 0;
    order.side = Side::Sell;
    order.type = OrderType::Limit;
    order.tif = exchange::TimeInForce::GTC;

    char buf[1024]{};
    size_t n = encode_exec_fill(buf, evt, order, false, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedExecTrade525>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(msg.order_id, 1u);
            EXPECT_EQ(price9_to_engine(msg.last_px), 1005000);
            EXPECT_EQ(wire_qty_to_engine(msg.last_qty), 100000);
            EXPECT_EQ(decode_side(msg.side), Side::Sell);
            EXPECT_EQ(msg.transact_time, 3000u);
            EXPECT_EQ(msg.aggressor_indicator, 0u);  // resting
            EXPECT_EQ(decoded.num_fills, 0u);  // empty group
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: ExecutionReportCancel534
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripExecCancel) {
    auto ctx = make_ctx();
    OrderCancelled evt{};
    evt.id = 1;
    evt.ts = 4000;
    evt.reason = CancelReason::UserRequested;

    Order order{};
    order.id = 1;
    order.client_order_id = 7;
    order.price = 1005000;
    order.quantity = 100000;
    order.filled_quantity = 0;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = exchange::TimeInForce::DAY;

    char buf[1024]{};
    size_t n = encode_exec_cancel(buf, evt, order, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedExecCancel534>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(msg.order_id, 1u);
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 7u);
            EXPECT_EQ(msg.transact_time, 4000u);
            EXPECT_EQ(decode_side(msg.side), Side::Buy);
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Encode -> Decode round-trip: OrderCancelReject535
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, RoundTripCancelReject) {
    auto ctx = make_ctx();
    OrderCancelRejected evt{};
    evt.id = 1;
    evt.client_order_id = 7;
    evt.ts = 5000;
    evt.reason = RejectReason::UnknownOrder;

    char buf[1024]{};
    size_t n = encode_cancel_reject(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, n, [&](const auto& decoded) {
        using T = std::decay_t<decltype(decoded)>;
        if constexpr (std::is_same_v<T, DecodedCancelReject535>) {
            visited = true;
            const auto& msg = decoded.root;
            EXPECT_EQ(msg.order_id, 1u);
            EXPECT_EQ(decode_cl_ord_id(msg.cl_ord_id), 7u);
            EXPECT_EQ(msg.transact_time, 5000u);
            EXPECT_EQ(msg.cxl_rej_reason, static_cast<uint16_t>(RejectReason::UnknownOrder));
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Error handling tests
// ---------------------------------------------------------------------------

TEST(ILink3DecoderTest, BufferTooShort) {
    char buf[4] = {};
    bool visited = false;
    auto rc = decode_ilink3_message(buf, 4, [&](const auto&) { visited = true; });
    EXPECT_EQ(rc, DecodeResult::kBufferTooShort);
    EXPECT_FALSE(visited);
}

TEST(ILink3DecoderTest, UnknownTemplateId) {
    // Encode a valid header with unknown template ID.
    MessageHeader hdr{};
    hdr.block_length = 10;
    hdr.template_id = 999;
    hdr.schema_id = ILINK3_SCHEMA_ID;
    hdr.version = ILINK3_VERSION;

    char buf[64]{};
    hdr.encode_to(buf);

    bool visited = false;
    auto rc = decode_ilink3_message(buf, sizeof(buf), [&](const auto&) { visited = true; });
    EXPECT_EQ(rc, DecodeResult::kUnknownTemplateId);
    EXPECT_FALSE(visited);
}

TEST(ILink3DecoderTest, TruncatedBodyReturnsBufferTooShort) {
    // Encode a valid new order then truncate the buffer.
    auto ctx = make_ctx();
    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::GTC;
    req.price = 1005000;
    req.quantity = 100000;
    req.timestamp = 1000;

    char buf[1024]{};
    size_t n = encode_new_order(buf, req, ctx);
    ASSERT_GT(n, sizeof(MessageHeader) + 1);

    // Truncate to just the header + 1 byte of body.
    bool visited = false;
    auto rc = decode_ilink3_message(buf, sizeof(MessageHeader) + 1,
                                     [&](const auto&) { visited = true; });
    EXPECT_EQ(rc, DecodeResult::kBufferTooShort);
    EXPECT_FALSE(visited);
}

}  // namespace exchange::cme::sbe::ilink3
