#include "cme/codec/ilink3_messages.h"

#include <cstring>
#include <gtest/gtest.h>

namespace exchange::cme::sbe::ilink3 {
namespace {

// ---------------------------------------------------------------------------
// Struct sizes must match schema blockLength exactly.
// ---------------------------------------------------------------------------

TEST(ILink3StructSizes, NewOrderSingle514) {
    EXPECT_EQ(sizeof(NewOrderSingle514), 116u);
    EXPECT_EQ(NewOrderSingle514::TEMPLATE_ID, 514);
    EXPECT_EQ(NewOrderSingle514::BLOCK_LENGTH, 116);
}

TEST(ILink3StructSizes, OrderCancelReplaceRequest515) {
    EXPECT_EQ(sizeof(OrderCancelReplaceRequest515), 125u);
    EXPECT_EQ(OrderCancelReplaceRequest515::TEMPLATE_ID, 515);
    EXPECT_EQ(OrderCancelReplaceRequest515::BLOCK_LENGTH, 125);
}

TEST(ILink3StructSizes, OrderCancelRequest516) {
    EXPECT_EQ(sizeof(OrderCancelRequest516), 88u);
    EXPECT_EQ(OrderCancelRequest516::TEMPLATE_ID, 516);
    EXPECT_EQ(OrderCancelRequest516::BLOCK_LENGTH, 88);
}

TEST(ILink3StructSizes, OrderMassActionRequest529) {
    EXPECT_EQ(sizeof(OrderMassActionRequest529), 71u);
    EXPECT_EQ(OrderMassActionRequest529::TEMPLATE_ID, 529);
    EXPECT_EQ(OrderMassActionRequest529::BLOCK_LENGTH, 71);
}

TEST(ILink3StructSizes, ExecutionReportNew522) {
    EXPECT_EQ(sizeof(ExecutionReportNew522), 209u);
    EXPECT_EQ(ExecutionReportNew522::TEMPLATE_ID, 522);
    EXPECT_EQ(ExecutionReportNew522::BLOCK_LENGTH, 209);
}

TEST(ILink3StructSizes, ExecutionReportReject523) {
    EXPECT_EQ(sizeof(ExecutionReportReject523), 467u);
    EXPECT_EQ(ExecutionReportReject523::TEMPLATE_ID, 523);
    EXPECT_EQ(ExecutionReportReject523::BLOCK_LENGTH, 467);
}

TEST(ILink3StructSizes, ExecutionReportTradeOutright525) {
    EXPECT_EQ(sizeof(ExecutionReportTradeOutright525), 235u);
    EXPECT_EQ(ExecutionReportTradeOutright525::TEMPLATE_ID, 525);
    EXPECT_EQ(ExecutionReportTradeOutright525::BLOCK_LENGTH, 235);
}

TEST(ILink3StructSizes, ExecutionReportCancel534) {
    EXPECT_EQ(sizeof(ExecutionReportCancel534), 214u);
    EXPECT_EQ(ExecutionReportCancel534::TEMPLATE_ID, 534);
    EXPECT_EQ(ExecutionReportCancel534::BLOCK_LENGTH, 214);
}

TEST(ILink3StructSizes, OrderCancelReject535) {
    EXPECT_EQ(sizeof(OrderCancelReject535), 409u);
    EXPECT_EQ(OrderCancelReject535::TEMPLATE_ID, 535);
    EXPECT_EQ(OrderCancelReject535::BLOCK_LENGTH, 409);
}

TEST(ILink3StructSizes, FillEntry) {
    EXPECT_EQ(sizeof(FillEntry), 15u);
}

TEST(ILink3StructSizes, OrderEventEntry) {
    EXPECT_EQ(sizeof(OrderEventEntry), 23u);
}

// ---------------------------------------------------------------------------
// Field offset verification — memcpy a known struct to a byte buffer and
// check that fields land at the schema-defined offsets.
// ---------------------------------------------------------------------------

TEST(ILink3Offsets, NewOrderSingle514) {
    NewOrderSingle514 msg{};
    auto* base = reinterpret_cast<const char*>(&msg);

    EXPECT_EQ(reinterpret_cast<const char*>(&msg.price) - base, 0);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_qty) - base, 8);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.security_id) - base, 12);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.side) - base, 16);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.seq_num) - base, 17);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sender_id) - base, 21);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cl_ord_id) - base, 41);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.party_details_list_req_id) - base, 61);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_request_id) - base, 69);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sending_time_epoch) - base, 77);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.stop_px) - base, 85);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.location) - base, 93);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.min_qty) - base, 98);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.display_qty) - base, 102);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.expire_date) - base, 106);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.ord_type) - base, 108);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.time_in_force) - base, 109);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.manual_order_indicator) - base, 110);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.exec_inst) - base, 111);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.execution_mode) - base, 112);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.liquidity_flag) - base, 113);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.managed_order) - base, 114);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.short_sale_type) - base, 115);
}

TEST(ILink3Offsets, OrderCancelRequest516) {
    OrderCancelRequest516 msg{};
    auto* base = reinterpret_cast<const char*>(&msg);

    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_id) - base, 0);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.party_details_list_req_id) - base, 8);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.manual_order_indicator) - base, 16);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.seq_num) - base, 17);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sender_id) - base, 21);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cl_ord_id) - base, 41);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_request_id) - base, 61);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sending_time_epoch) - base, 69);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.location) - base, 77);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.security_id) - base, 82);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.side) - base, 86);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.liquidity_flag) - base, 87);
}

TEST(ILink3Offsets, ExecutionReportNew522) {
    ExecutionReportNew522 msg{};
    auto* base = reinterpret_cast<const char*>(&msg);

    EXPECT_EQ(reinterpret_cast<const char*>(&msg.seq_num) - base, 0);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.uuid) - base, 4);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.exec_id) - base, 12);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sender_id) - base, 52);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cl_ord_id) - base, 72);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.party_details_list_req_id) - base, 92);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_id) - base, 100);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.price) - base, 108);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.stop_px) - base, 116);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.transact_time) - base, 124);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sending_time_epoch) - base, 132);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_request_id) - base, 140);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cross_id) - base, 148);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.host_cross_id) - base, 156);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.location) - base, 164);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.security_id) - base, 169);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_qty) - base, 173);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.min_qty) - base, 177);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.display_qty) - base, 181);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.expire_date) - base, 185);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.delay_duration) - base, 187);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.ord_type) - base, 189);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.side) - base, 190);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.time_in_force) - base, 191);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.delay_to_time) - base, 201);
}

TEST(ILink3Offsets, ExecutionReportTradeOutright525) {
    ExecutionReportTradeOutright525 msg{};
    auto* base = reinterpret_cast<const char*>(&msg);

    EXPECT_EQ(reinterpret_cast<const char*>(&msg.seq_num) - base, 0);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.uuid) - base, 4);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.last_px) - base, 100);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_id) - base, 108);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.price) - base, 116);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.stop_px) - base, 124);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.last_qty) - base, 193);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cum_qty) - base, 197);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.leaves_qty) - base, 213);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.ord_status) - base, 221);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.aggressor_indicator) - base, 227);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.ownership) - base, 234);
}

TEST(ILink3Offsets, OrderCancelReject535) {
    OrderCancelReject535 msg{};
    auto* base = reinterpret_cast<const char*>(&msg);

    EXPECT_EQ(reinterpret_cast<const char*>(&msg.seq_num) - base, 0);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.uuid) - base, 4);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.text) - base, 12);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.exec_id) - base, 268);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sender_id) - base, 308);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cl_ord_id) - base, 328);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.party_details_list_req_id) - base, 348);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_id) - base, 356);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.transact_time) - base, 364);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.sending_time_epoch) - base, 372);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.order_request_id) - base, 380);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.location) - base, 388);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.cxl_rej_reason) - base, 393);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.delay_duration) - base, 395);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.manual_order_indicator) - base, 397);
    EXPECT_EQ(reinterpret_cast<const char*>(&msg.delay_to_time) - base, 401);
}

// ---------------------------------------------------------------------------
// make_header helper
// ---------------------------------------------------------------------------

TEST(ILink3MakeHeader, NewOrderSingle514) {
    auto hdr = make_header<NewOrderSingle514>();
    EXPECT_EQ(hdr.block_length, 116);
    EXPECT_EQ(hdr.template_id, 514);
    EXPECT_EQ(hdr.schema_id, ILINK3_SCHEMA_ID);
    EXPECT_EQ(hdr.version, ILINK3_VERSION);
}

TEST(ILink3MakeHeader, ExecutionReportNew522) {
    auto hdr = make_header<ExecutionReportNew522>();
    EXPECT_EQ(hdr.block_length, 209);
    EXPECT_EQ(hdr.template_id, 522);
    EXPECT_EQ(hdr.schema_id, 8);
    EXPECT_EQ(hdr.version, 5);
}

// ---------------------------------------------------------------------------
// Wire round-trip: write a NewOrderSingle to a buffer and read it back.
// ---------------------------------------------------------------------------

TEST(ILink3RoundTrip, NewOrderSingle514) {
    NewOrderSingle514 msg{};
    msg.price = PRICE9::from_double(12345.50);
    msg.order_qty = 100;
    msg.security_id = 54321;
    msg.side = 1;  // Buy
    msg.seq_num = 42;
    std::strncpy(msg.sender_id, "TRADER01", sizeof(msg.sender_id));
    std::strncpy(msg.cl_ord_id, "ORD-00001", sizeof(msg.cl_ord_id));
    msg.party_details_list_req_id = 9999;
    msg.order_request_id = 1001;
    msg.sending_time_epoch = 1700000000000000000ull;
    msg.stop_px = PRICE9{INT64_NULL};  // null
    std::strncpy(msg.location, "CHIC", sizeof(msg.location));
    msg.min_qty = UINT32_NULL;
    msg.display_qty = UINT32_NULL;
    msg.expire_date = UINT16_NULL;
    msg.ord_type = 2;  // Limit
    msg.time_in_force = 0;  // Day
    msg.manual_order_indicator = 0;
    msg.exec_inst = 0;
    msg.execution_mode = 0;
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;

    // Write header + message to buffer
    char buf[sizeof(MessageHeader) + sizeof(NewOrderSingle514)];
    auto hdr = make_header<NewOrderSingle514>();
    char* p = hdr.encode_to(buf);
    std::memcpy(p, &msg, sizeof(msg));

    // Read back
    MessageHeader decoded_hdr{};
    const char* rp = MessageHeader::decode_from(buf, decoded_hdr);
    EXPECT_EQ(decoded_hdr.template_id, 514);
    EXPECT_EQ(decoded_hdr.block_length, 116);

    NewOrderSingle514 decoded_msg{};
    std::memcpy(&decoded_msg, rp, sizeof(decoded_msg));

    EXPECT_NEAR(decoded_msg.price.to_double(), 12345.50, 1e-6);
    EXPECT_EQ(decoded_msg.order_qty, 100u);
    EXPECT_EQ(decoded_msg.security_id, 54321);
    EXPECT_EQ(decoded_msg.side, 1);
    EXPECT_EQ(decoded_msg.seq_num, 42u);
    EXPECT_EQ(std::string(decoded_msg.sender_id, 8), "TRADER01");
    EXPECT_EQ(std::string(decoded_msg.cl_ord_id, 9), "ORD-00001");
    EXPECT_EQ(decoded_msg.party_details_list_req_id, 9999u);
    EXPECT_EQ(decoded_msg.order_request_id, 1001u);
    EXPECT_TRUE(decoded_msg.stop_px.is_null());
    EXPECT_EQ(decoded_msg.min_qty, UINT32_NULL);
    EXPECT_EQ(decoded_msg.ord_type, 2);
}

// ---------------------------------------------------------------------------
// Wire round-trip: ExecutionReportCancel534
// ---------------------------------------------------------------------------

TEST(ILink3RoundTrip, ExecutionReportCancel534) {
    ExecutionReportCancel534 msg{};
    msg.seq_num = 100;
    msg.uuid = 0xDEADBEEF;
    std::strncpy(msg.exec_id, "EXEC-001", sizeof(msg.exec_id));
    msg.order_id = 77777;
    msg.price = PRICE9::from_double(99.99);
    msg.stop_px = PRICE9{INT64_NULL};
    msg.transact_time = 1700000000000000000ull;
    msg.sending_time_epoch = 1700000000000000001ull;
    msg.cum_qty = 50;
    msg.order_qty = 100;
    msg.exec_restatement_reason = 8;  // MassCancel

    char buf[sizeof(MessageHeader) + sizeof(ExecutionReportCancel534)];
    auto hdr = make_header<ExecutionReportCancel534>();
    char* p = hdr.encode_to(buf);
    std::memcpy(p, &msg, sizeof(msg));

    MessageHeader decoded_hdr{};
    const char* rp = MessageHeader::decode_from(buf, decoded_hdr);
    EXPECT_EQ(decoded_hdr.template_id, 534);
    EXPECT_EQ(decoded_hdr.block_length, 214);

    ExecutionReportCancel534 decoded{};
    std::memcpy(&decoded, rp, sizeof(decoded));

    EXPECT_EQ(decoded.seq_num, 100u);
    EXPECT_EQ(decoded.uuid, 0xDEADBEEFu);
    EXPECT_EQ(decoded.order_id, 77777u);
    EXPECT_NEAR(decoded.price.to_double(), 99.99, 1e-6);
    EXPECT_TRUE(decoded.stop_px.is_null());
    EXPECT_EQ(decoded.cum_qty, 50u);
    EXPECT_EQ(decoded.exec_restatement_reason, 8);
}

// ---------------------------------------------------------------------------
// Wire round-trip: OrderCancelReject535 with text
// ---------------------------------------------------------------------------

TEST(ILink3RoundTrip, OrderCancelReject535) {
    OrderCancelReject535 msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.seq_num = 200;
    msg.uuid = 42;
    std::strncpy(msg.text, "Unknown order ID", sizeof(msg.text));
    msg.order_id = 12345;
    msg.cxl_rej_reason = 1;  // UnknownOrder

    char buf[sizeof(MessageHeader) + sizeof(OrderCancelReject535)];
    auto hdr = make_header<OrderCancelReject535>();
    char* p = hdr.encode_to(buf);
    std::memcpy(p, &msg, sizeof(msg));

    MessageHeader decoded_hdr{};
    const char* rp = MessageHeader::decode_from(buf, decoded_hdr);
    EXPECT_EQ(decoded_hdr.template_id, 535);

    OrderCancelReject535 decoded{};
    std::memcpy(&decoded, rp, sizeof(decoded));

    EXPECT_EQ(decoded.seq_num, 200u);
    EXPECT_EQ(decoded.order_id, 12345u);
    EXPECT_EQ(decoded.cxl_rej_reason, 1u);
    EXPECT_EQ(std::string(decoded.text, 16), "Unknown order ID");
}

// ---------------------------------------------------------------------------
// Template ID constants match between struct and named constant.
// ---------------------------------------------------------------------------

TEST(ILink3TemplateIds, AllMatch) {
    EXPECT_EQ(NewOrderSingle514::TEMPLATE_ID, NEW_ORDER_SINGLE_ID);
    EXPECT_EQ(OrderCancelReplaceRequest515::TEMPLATE_ID, ORDER_CANCEL_REPLACE_REQUEST_ID);
    EXPECT_EQ(OrderCancelRequest516::TEMPLATE_ID, ORDER_CANCEL_REQUEST_ID);
    EXPECT_EQ(OrderMassActionRequest529::TEMPLATE_ID, ORDER_MASS_ACTION_REQUEST_ID);
    EXPECT_EQ(ExecutionReportNew522::TEMPLATE_ID, EXEC_REPORT_NEW_ID);
    EXPECT_EQ(ExecutionReportReject523::TEMPLATE_ID, EXEC_REPORT_REJECT_ID);
    EXPECT_EQ(ExecutionReportTradeOutright525::TEMPLATE_ID, EXEC_REPORT_TRADE_OUTRIGHT_ID);
    EXPECT_EQ(ExecutionReportCancel534::TEMPLATE_ID, EXEC_REPORT_CANCEL_ID);
    EXPECT_EQ(OrderCancelReject535::TEMPLATE_ID, ORDER_CANCEL_REJECT_ID);
}

// ---------------------------------------------------------------------------
// Enum values match schema definitions.
// ---------------------------------------------------------------------------

TEST(ILink3Enums, Side) {
    EXPECT_EQ(static_cast<uint8_t>(SideReq::Buy), 1);
    EXPECT_EQ(static_cast<uint8_t>(SideReq::Sell), 2);
}

TEST(ILink3Enums, OrdType) {
    EXPECT_EQ(static_cast<uint8_t>(OrdType::MarketWithProtection), 1);
    EXPECT_EQ(static_cast<uint8_t>(OrdType::Limit), 2);
    EXPECT_EQ(static_cast<uint8_t>(OrdType::StopWithProtection), 3);
    EXPECT_EQ(static_cast<uint8_t>(OrdType::StopLimit), 4);
}

TEST(ILink3Enums, TimeInForce) {
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::Day), 0);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::GTC), 1);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::FAK), 3);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::FOK), 4);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::GTD), 6);
}

TEST(ILink3Enums, ExecType) {
    EXPECT_EQ(static_cast<uint8_t>(ExecType::New), '0');
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Canceled), '4');
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Replaced), '5');
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Rejected), '8');
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Expired), 'C');
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Trade), 'F');
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Status), 'I');
}

TEST(ILink3Enums, OrdStatus) {
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::New), '0');
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::PartiallyFilled), '1');
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::Filled), '2');
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::Canceled), '4');
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::Rejected), '8');
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::Expired), 'C');
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::Undefined), 'U');
}

TEST(ILink3Enums, MassActionScope) {
    EXPECT_EQ(static_cast<uint8_t>(MassActionScope::Instrument), 1);
    EXPECT_EQ(static_cast<uint8_t>(MassActionScope::All), 7);
    EXPECT_EQ(static_cast<uint8_t>(MassActionScope::MarketSegment), 9);
    EXPECT_EQ(static_cast<uint8_t>(MassActionScope::ProductGroup), 10);
}

TEST(ILink3Enums, ExecMode) {
    EXPECT_EQ(static_cast<uint8_t>(ExecMode::Aggressive), 'A');
    EXPECT_EQ(static_cast<uint8_t>(ExecMode::Passive), 'P');
}

// Enums fit in uint8_t (single byte on wire)
TEST(ILink3Enums, AllSingleByte) {
    EXPECT_EQ(sizeof(SideReq), 1u);
    EXPECT_EQ(sizeof(OrdType), 1u);
    EXPECT_EQ(sizeof(TimeInForce), 1u);
    EXPECT_EQ(sizeof(ExecType), 1u);
    EXPECT_EQ(sizeof(OrdStatus), 1u);
    EXPECT_EQ(sizeof(MassActionScope), 1u);
    EXPECT_EQ(sizeof(ManualOrdInd), 1u);
    EXPECT_EQ(sizeof(ExecMode), 1u);
}

}  // namespace
}  // namespace exchange::cme::sbe::ilink3
