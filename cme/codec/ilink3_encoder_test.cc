#include "cme/codec/ilink3_encoder.h"

#include <cstring>
#include <gtest/gtest.h>

namespace exchange::cme::sbe::ilink3 {
namespace {

// Helper: make a standard EncodeContext for tests.
EncodeContext make_test_ctx() {
    EncodeContext ctx{};
    ctx.seq_num = 1;
    ctx.uuid = 0xAAAABBBBCCCCDDDDull;
    ctx.party_details_list_req_id = 42;
    ctx.security_id = 12345;
    std::strncpy(ctx.sender_id, "TEST_SENDER", sizeof(ctx.sender_id));
    std::strncpy(ctx.location, "CHIC", sizeof(ctx.location));
    return ctx;
}

// ---------------------------------------------------------------------------
// Price / quantity conversions
// ---------------------------------------------------------------------------

TEST(EncoderConversions, PriceToPrice9) {
    // Engine: 100.5000 = 1005000 (PRICE_SCALE=10000)
    // PRICE9: 1005000 * 100000 = 100500000000
    PRICE9 p = engine_price_to_price9(1005000);
    EXPECT_EQ(p.mantissa, 100500000000ll);
    EXPECT_NEAR(p.to_double(), 100.5, 1e-6);
}

TEST(EncoderConversions, PriceNull) {
    PRICE9 p = engine_price_to_pricenull9(0);
    EXPECT_TRUE(p.is_null());
}

TEST(EncoderConversions, PriceNonNull) {
    PRICE9 p = engine_price_to_pricenull9(500000);  // 50.0
    EXPECT_FALSE(p.is_null());
    EXPECT_NEAR(p.to_double(), 50.0, 1e-6);
}

TEST(EncoderConversions, QtyToWire) {
    EXPECT_EQ(engine_qty_to_wire(10000), 1u);   // 1 contract
    EXPECT_EQ(engine_qty_to_wire(100000), 10u);  // 10 contracts
    EXPECT_EQ(engine_qty_to_wire(0), 0u);
}

// ---------------------------------------------------------------------------
// Enum conversions
// ---------------------------------------------------------------------------

TEST(EncoderConversions, Side) {
    EXPECT_EQ(encode_side(Side::Buy), 1);
    EXPECT_EQ(encode_side(Side::Sell), 2);
}

TEST(EncoderConversions, OrdType) {
    EXPECT_EQ(encode_ord_type(OrderType::Limit), 2);
    EXPECT_EQ(encode_ord_type(OrderType::Market), 1);
    EXPECT_EQ(encode_ord_type(OrderType::Stop), 3);
    EXPECT_EQ(encode_ord_type(OrderType::StopLimit), 4);
}

TEST(EncoderConversions, TimeInForce) {
    EXPECT_EQ(encode_tif(exchange::TimeInForce::DAY), 0);
    EXPECT_EQ(encode_tif(exchange::TimeInForce::GTC), 1);
    EXPECT_EQ(encode_tif(exchange::TimeInForce::IOC), 3);
    EXPECT_EQ(encode_tif(exchange::TimeInForce::FOK), 4);
    EXPECT_EQ(encode_tif(exchange::TimeInForce::GTD), 6);
}

// ---------------------------------------------------------------------------
// encode_new_order
// ---------------------------------------------------------------------------

TEST(EncodeNewOrder, BasicFields) {
    auto ctx = make_test_ctx();
    OrderRequest req{};
    req.client_order_id = 1001;
    req.account_id = 100;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::DAY;
    req.price = 1005000;    // 100.50
    req.quantity = 50000;   // 5 contracts
    req.stop_price = 0;
    req.timestamp = 1700000000000000000ll;
    req.display_qty = 0;

    char buf[512];
    size_t n = encode_new_order(buf, req, ctx);

    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(NewOrderSingle514));

    // Decode header
    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 514);
    EXPECT_EQ(hdr.block_length, 116);
    EXPECT_EQ(hdr.schema_id, ILINK3_SCHEMA_ID);

    // Decode body
    NewOrderSingle514 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_NEAR(msg.price.to_double(), 100.5, 1e-6);
    EXPECT_EQ(msg.order_qty, 5u);
    EXPECT_EQ(msg.security_id, 12345);
    EXPECT_EQ(msg.side, 1);  // Buy
    EXPECT_EQ(msg.seq_num, 1u);
    EXPECT_EQ(msg.order_request_id, 1001u);
    EXPECT_TRUE(msg.stop_px.is_null());
    EXPECT_EQ(msg.ord_type, 2);  // Limit
    EXPECT_EQ(msg.time_in_force, 0);  // Day
    EXPECT_EQ(msg.min_qty, UINT32_NULL);
    EXPECT_EQ(msg.display_qty, UINT32_NULL);
    EXPECT_EQ(msg.expire_date, UINT16_NULL);
}

TEST(EncodeNewOrder, IcebergOrder) {
    auto ctx = make_test_ctx();
    OrderRequest req{};
    req.client_order_id = 2002;
    req.side = Side::Sell;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::GTC;
    req.price = 2000000;   // 200.00
    req.quantity = 100000;  // 10 contracts
    req.display_qty = 20000;  // show 2 contracts

    char buf[512];
    encode_new_order(buf, req, ctx);

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    NewOrderSingle514 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.display_qty, 2u);
    EXPECT_EQ(msg.order_qty, 10u);
    EXPECT_EQ(msg.side, 2);  // Sell
}

TEST(EncodeNewOrder, MarketOrderPriceIsNull) {
    auto ctx = make_test_ctx();
    OrderRequest req{};
    req.client_order_id = 3003;
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.tif = exchange::TimeInForce::DAY;
    req.price = 0;
    req.quantity = 10000;

    char buf[512];
    encode_new_order(buf, req, ctx);

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    NewOrderSingle514 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_TRUE(msg.price.is_null());
    EXPECT_EQ(msg.ord_type, 1);  // MarketWithProtection
}

// ---------------------------------------------------------------------------
// encode_cancel_order
// ---------------------------------------------------------------------------

TEST(EncodeCancelOrder, BasicFields) {
    auto ctx = make_test_ctx();
    char buf[512];

    size_t n = encode_cancel_order(buf, 777, 1001, Side::Buy,
                                    1700000000000000000ll, ctx);

    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(OrderCancelRequest516));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 516);

    OrderCancelRequest516 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.order_id, 777u);
    EXPECT_EQ(msg.security_id, 12345);
    EXPECT_EQ(msg.side, 1);
    EXPECT_EQ(msg.seq_num, 1u);
    EXPECT_EQ(msg.order_request_id, 1001u);
}

// ---------------------------------------------------------------------------
// encode_modify_order
// ---------------------------------------------------------------------------

TEST(EncodeModifyOrder, BasicFields) {
    auto ctx = make_test_ctx();
    ModifyRequest req{};
    req.order_id = 555;
    req.client_order_id = 3003;
    req.new_price = 1010000;   // 101.00
    req.new_quantity = 30000;  // 3 contracts
    req.timestamp = 1700000000000000000ll;

    char buf[512];
    size_t n = encode_modify_order(buf, req, 555, Side::Sell,
                                    OrderType::Limit,
                                    exchange::TimeInForce::DAY, ctx);

    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(OrderCancelReplaceRequest515));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 515);

    OrderCancelReplaceRequest515 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_NEAR(msg.price.to_double(), 101.0, 1e-6);
    EXPECT_EQ(msg.order_qty, 3u);
    EXPECT_EQ(msg.order_id, 555u);
    EXPECT_EQ(msg.side, 2);  // Sell
    EXPECT_EQ(msg.ord_type, 2);  // Limit
    EXPECT_TRUE(msg.stop_px.is_null());
}

// ---------------------------------------------------------------------------
// encode_mass_cancel
// ---------------------------------------------------------------------------

TEST(EncodeMassCancel, InstrumentScope) {
    auto ctx = make_test_ctx();
    char buf[512];

    size_t n = encode_mass_cancel(buf, MassActionScope::Instrument,
                                   1700000000000000000ll, ctx);

    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(OrderMassActionRequest529));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 529);

    OrderMassActionRequest529 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.mass_action_scope, 1);  // Instrument
    EXPECT_EQ(msg.security_id, 12345);
    EXPECT_EQ(msg.side, UINT8_NULL);  // all sides
}

// ---------------------------------------------------------------------------
// encode_exec_new
// ---------------------------------------------------------------------------

TEST(EncodeExecNew, BasicFields) {
    auto ctx = make_test_ctx();
    OrderAccepted evt{};
    evt.id = 42;
    evt.client_order_id = 1001;
    evt.ts = 1700000000000000000ll;

    Order order{};
    order.id = 42;
    order.client_order_id = 1001;
    order.price = 1005000;  // 100.50
    order.quantity = 50000;  // 5 contracts
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = exchange::TimeInForce::DAY;

    char buf[512];
    size_t n = encode_exec_new(buf, evt, order, ctx);
    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(ExecutionReportNew522));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 522);

    ExecutionReportNew522 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.order_id, 42u);
    EXPECT_EQ(msg.seq_num, 1u);
    EXPECT_EQ(msg.uuid, ctx.uuid);
    EXPECT_NEAR(msg.price.to_double(), 100.5, 1e-6);
    EXPECT_EQ(msg.order_qty, 5u);
    EXPECT_EQ(msg.side, 1);
    EXPECT_EQ(msg.ord_type, 2);
    EXPECT_EQ(msg.cross_id, UINT64_NULL);
}

// ---------------------------------------------------------------------------
// encode_exec_fill
// ---------------------------------------------------------------------------

TEST(EncodeExecFill, FullFill) {
    auto ctx = make_test_ctx();
    OrderFilled evt{};
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.price = 1005000;   // 100.50
    evt.quantity = 50000;  // 5 contracts
    evt.ts = 1700000000000000000ll;

    Order order{};
    order.id = 20;
    order.client_order_id = 2002;
    order.price = 1005000;
    order.quantity = 50000;
    order.filled_quantity = 0;
    order.side = Side::Sell;
    order.type = OrderType::Limit;
    order.tif = exchange::TimeInForce::GTC;

    char buf[1024];
    size_t n = encode_exec_fill(buf, evt, order, false, ctx);

    // Root block + 2 empty group headers (3 bytes each)
    size_t expected = sizeof(MessageHeader) + sizeof(ExecutionReportTradeOutright525)
                      + sizeof(GroupHeader) * 2;
    EXPECT_EQ(n, expected);

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 525);
    EXPECT_EQ(hdr.block_length, 235);

    ExecutionReportTradeOutright525 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_NEAR(msg.last_px.to_double(), 100.5, 1e-6);
    EXPECT_EQ(msg.last_qty, 5u);
    EXPECT_EQ(msg.cum_qty, 5u);
    EXPECT_EQ(msg.leaves_qty, 0u);
    EXPECT_EQ(msg.ord_status, static_cast<uint8_t>(OrdStatus::Filled));
    EXPECT_EQ(msg.aggressor_indicator, 0);  // resting side
    EXPECT_EQ(msg.order_id, 20u);

    // Verify empty group headers follow
    rp += sizeof(msg);
    GroupHeader gh1{};
    rp = GroupHeader::decode_from(rp, gh1);
    EXPECT_EQ(gh1.num_in_group, 0);
    EXPECT_EQ(gh1.block_length, sizeof(FillEntry));

    GroupHeader gh2{};
    GroupHeader::decode_from(rp, gh2);
    EXPECT_EQ(gh2.num_in_group, 0);
    EXPECT_EQ(gh2.block_length, sizeof(OrderEventEntry));
}

// ---------------------------------------------------------------------------
// encode_exec_cancel
// ---------------------------------------------------------------------------

TEST(EncodeExecCancel, UserCancel) {
    auto ctx = make_test_ctx();
    OrderCancelled evt{};
    evt.id = 77;
    evt.ts = 1700000000000000000ll;
    evt.reason = CancelReason::UserRequested;

    Order order{};
    order.id = 77;
    order.client_order_id = 4004;
    order.price = 990000;  // 99.00
    order.quantity = 100000;
    order.filled_quantity = 30000;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = exchange::TimeInForce::GTC;

    char buf[512];
    size_t n = encode_exec_cancel(buf, evt, order, ctx);
    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(ExecutionReportCancel534));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 534);

    ExecutionReportCancel534 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.order_id, 77u);
    EXPECT_NEAR(msg.price.to_double(), 99.0, 1e-6);
    EXPECT_EQ(msg.order_qty, 10u);
    EXPECT_EQ(msg.cum_qty, 3u);
    EXPECT_EQ(msg.exec_restatement_reason, 0);  // UserRequested
}

// ---------------------------------------------------------------------------
// encode_exec_reject
// ---------------------------------------------------------------------------

TEST(EncodeExecReject, InvalidPrice) {
    auto ctx = make_test_ctx();
    OrderRejected evt{};
    evt.client_order_id = 5005;
    evt.ts = 1700000000000000000ll;
    evt.reason = RejectReason::InvalidPrice;

    char buf[1024];
    size_t n = encode_exec_reject(buf, evt, ctx);
    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(ExecutionReportReject523));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 523);

    ExecutionReportReject523 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.seq_num, 1u);
    EXPECT_EQ(msg.ord_rej_reason, static_cast<uint16_t>(RejectReason::InvalidPrice));
    EXPECT_EQ(msg.order_request_id, 5005u);
    EXPECT_TRUE(msg.price.is_null());
    EXPECT_EQ(msg.order_id, UINT64_NULL);
    // CRITICAL: unknown fields must be UINT8_NULL, not 0
    EXPECT_EQ(msg.ord_type, UINT8_NULL);
    EXPECT_EQ(msg.side, UINT8_NULL);
    EXPECT_EQ(msg.time_in_force, UINT8_NULL);
    EXPECT_EQ(msg.execution_mode, UINT8_NULL);
}

// ---------------------------------------------------------------------------
// encode_cancel_reject
// ---------------------------------------------------------------------------

TEST(EncodeCancelReject, UnknownOrder) {
    auto ctx = make_test_ctx();
    OrderCancelRejected evt{};
    evt.id = 99;
    evt.client_order_id = 6006;
    evt.ts = 1700000000000000000ll;
    evt.reason = RejectReason::UnknownOrder;

    char buf[1024];
    size_t n = encode_cancel_reject(buf, evt, ctx);
    EXPECT_EQ(n, sizeof(MessageHeader) + sizeof(OrderCancelReject535));

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    EXPECT_EQ(hdr.template_id, 535);

    OrderCancelReject535 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(msg.order_id, 99u);
    EXPECT_EQ(msg.cxl_rej_reason, static_cast<uint16_t>(RejectReason::UnknownOrder));
    EXPECT_EQ(msg.order_request_id, 6006u);
    EXPECT_EQ(msg.delay_to_time, UINT64_NULL);
}

// ---------------------------------------------------------------------------
// Context fields propagated correctly
// ---------------------------------------------------------------------------

TEST(EncodeContext, SenderIdAndLocation) {
    auto ctx = make_test_ctx();
    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = exchange::TimeInForce::DAY;
    req.price = 1000000;
    req.quantity = 10000;

    char buf[512];
    encode_new_order(buf, req, ctx);

    MessageHeader hdr{};
    const char* rp = MessageHeader::decode_from(buf, hdr);
    NewOrderSingle514 msg{};
    std::memcpy(&msg, rp, sizeof(msg));

    EXPECT_EQ(std::string(msg.sender_id, 11), "TEST_SENDER");
    EXPECT_EQ(std::string(msg.location, 4), "CHIC");
    EXPECT_EQ(msg.party_details_list_req_id, 42u);
}

}  // namespace
}  // namespace exchange::cme::sbe::ilink3
