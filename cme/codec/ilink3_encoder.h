#pragma once

#include "cme/codec/ilink3_messages.h"
#include "cme/codec/sbe_header.h"
#include "exchange-core/events.h"
#include "exchange-core/matching_engine.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace exchange::cme::sbe::ilink3 {

// ---------------------------------------------------------------------------
// Price conversion: engine fixed-point (PRICE_SCALE=10000) <-> SBE PRICE9 (1e9)
//
// engine_price * (1e9 / 10000) = engine_price * 100000
// ~2ns on modern x86 — integer multiply, no division.
// ---------------------------------------------------------------------------

constexpr int64_t ENGINE_TO_PRICE9_FACTOR = 100000;  // 1e9 / PRICE_SCALE

inline PRICE9 engine_price_to_price9(Price p) {
    return PRICE9{p * ENGINE_TO_PRICE9_FACTOR};
}

inline PRICE9 engine_price_to_pricenull9(Price p) {
    if (p == 0) return PRICE9{INT64_NULL};
    return PRICE9{p * ENGINE_TO_PRICE9_FACTOR};
}

// Quantity: engine uses Quantity (int64, scale 10000), wire uses uint32.
// Engine qty 10000 = 1 contract on wire.
inline uint32_t engine_qty_to_wire(Quantity q) {
    return static_cast<uint32_t>(q / PRICE_SCALE);
}

// ---------------------------------------------------------------------------
// Enum conversion: engine enums -> iLink3 wire values
// ---------------------------------------------------------------------------

inline uint8_t encode_side(Side s) {
    return static_cast<uint8_t>(s) + 1;  // Buy=0->1, Sell=1->2
}

inline uint8_t encode_ord_type(OrderType t) {
    switch (t) {
        case OrderType::Limit:     return static_cast<uint8_t>(OrdType::Limit);
        case OrderType::Market:    return static_cast<uint8_t>(OrdType::MarketWithProtection);
        case OrderType::Stop:      return static_cast<uint8_t>(OrdType::StopWithProtection);
        case OrderType::StopLimit: return static_cast<uint8_t>(OrdType::StopLimit);
    }
    return static_cast<uint8_t>(OrdType::Limit);
}

inline uint8_t encode_tif(exchange::TimeInForce tif) {
    switch (tif) {
        case exchange::TimeInForce::DAY: return static_cast<uint8_t>(TimeInForce::Day);
        case exchange::TimeInForce::GTC: return static_cast<uint8_t>(TimeInForce::GTC);
        case exchange::TimeInForce::IOC: return static_cast<uint8_t>(TimeInForce::FAK);
        case exchange::TimeInForce::FOK: return static_cast<uint8_t>(TimeInForce::FOK);
        case exchange::TimeInForce::GTD: return static_cast<uint8_t>(TimeInForce::GTD);
    }
    return static_cast<uint8_t>(TimeInForce::Day);
}

// ---------------------------------------------------------------------------
// Encode context — session-level state carried through encoding calls.
// ---------------------------------------------------------------------------

struct EncodeContext {
    uint32_t seq_num{0};
    uint64_t uuid{0};
    uint64_t party_details_list_req_id{0};
    int32_t  security_id{0};
    char     sender_id[20]{};
    char     location[5]{};
};

// ---------------------------------------------------------------------------
// Encoder functions.
//
// Each returns the total bytes written (header + message body).
// The caller must ensure buf has enough space (header + BLOCK_LENGTH).
// On error returns 0 (should not happen with valid inputs).
// ---------------------------------------------------------------------------

// Encode OrderRequest -> NewOrderSingle514 SBE bytes.
inline size_t encode_new_order(
    char* buf,
    const OrderRequest& req,
    const EncodeContext& ctx)
{
    auto hdr = make_header<NewOrderSingle514>();
    char* p = hdr.encode_to(buf);

    NewOrderSingle514 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.price = (req.type == OrderType::Market)
        ? PRICE9{INT64_NULL}
        : engine_price_to_price9(req.price);
    msg.order_qty = engine_qty_to_wire(req.quantity);
    msg.security_id = ctx.security_id;
    msg.side = encode_side(req.side);
    msg.seq_num = ctx.seq_num;
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    // cl_ord_id: encode client_order_id as numeric string
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(req.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_request_id = req.client_order_id;
    msg.sending_time_epoch = static_cast<uint64_t>(req.timestamp);
    msg.stop_px = engine_price_to_pricenull9(req.stop_price);
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.min_qty = UINT32_NULL;
    msg.display_qty = (req.display_qty > 0)
        ? engine_qty_to_wire(req.display_qty) : UINT32_NULL;
    msg.expire_date = (req.tif == exchange::TimeInForce::GTD)
        ? static_cast<uint16_t>(req.gtd_expiry / 86400000000000ll) : UINT16_NULL;
    msg.ord_type = encode_ord_type(req.type);
    msg.time_in_force = encode_tif(req.tif);
    msg.manual_order_indicator = 0;
    msg.exec_inst = 0;
    msg.execution_mode = static_cast<uint8_t>(ExecMode::Aggressive);
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(NewOrderSingle514);
}

// Encode cancel -> OrderCancelRequest516 SBE bytes.
inline size_t encode_cancel_order(
    char* buf,
    OrderId order_id,
    uint64_t client_order_id,
    Side side,
    Timestamp ts,
    const EncodeContext& ctx)
{
    auto hdr = make_header<OrderCancelRequest516>();
    char* p = hdr.encode_to(buf);

    OrderCancelRequest516 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.order_id = order_id;
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.manual_order_indicator = 0;
    msg.seq_num = ctx.seq_num;
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(client_order_id));
    msg.order_request_id = client_order_id;
    msg.sending_time_epoch = static_cast<uint64_t>(ts);
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.security_id = ctx.security_id;
    msg.side = encode_side(side);
    msg.liquidity_flag = UINT8_NULL;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(OrderCancelRequest516);
}

// Encode modify -> OrderCancelReplaceRequest515 SBE bytes.
inline size_t encode_modify_order(
    char* buf,
    const ModifyRequest& req,
    OrderId exchange_order_id,
    Side side,
    exchange::OrderType ord_type,
    exchange::TimeInForce tif,
    const EncodeContext& ctx)
{
    auto hdr = make_header<OrderCancelReplaceRequest515>();
    char* p = hdr.encode_to(buf);

    OrderCancelReplaceRequest515 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.price = engine_price_to_price9(req.new_price);
    msg.order_qty = engine_qty_to_wire(req.new_quantity);
    msg.security_id = ctx.security_id;
    msg.side = encode_side(side);
    msg.seq_num = ctx.seq_num;
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(req.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_id = exchange_order_id;
    msg.stop_px = PRICE9{INT64_NULL};
    msg.order_request_id = req.client_order_id;
    msg.sending_time_epoch = static_cast<uint64_t>(req.timestamp);
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.min_qty = UINT32_NULL;
    msg.display_qty = UINT32_NULL;
    msg.expire_date = UINT16_NULL;
    msg.ord_type = encode_ord_type(ord_type);
    msg.time_in_force = encode_tif(tif);
    msg.manual_order_indicator = 0;
    msg.ofm_override = 0;
    msg.exec_inst = 0;
    msg.execution_mode = static_cast<uint8_t>(ExecMode::Aggressive);
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(OrderCancelReplaceRequest515);
}

// Encode mass cancel -> OrderMassActionRequest529 SBE bytes.
inline size_t encode_mass_cancel(
    char* buf,
    MassActionScope scope,
    Timestamp ts,
    const EncodeContext& ctx)
{
    auto hdr = make_header<OrderMassActionRequest529>();
    char* p = hdr.encode_to(buf);

    OrderMassActionRequest529 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_request_id = 0;
    msg.manual_order_indicator = 0;
    msg.seq_num = ctx.seq_num;
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    msg.sending_time_epoch = static_cast<uint64_t>(ts);
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.security_id = ctx.security_id;
    msg.mass_action_scope = static_cast<uint8_t>(scope);
    msg.market_segment_id = UINT8_NULL;
    msg.mass_cancel_request_type = UINT8_NULL;
    msg.side = UINT8_NULL;
    msg.ord_type = UINT8_NULL;
    msg.time_in_force = UINT8_NULL;
    msg.liquidity_flag = UINT8_NULL;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(OrderMassActionRequest529);
}

// ---------------------------------------------------------------------------
// Execution report encoders — engine events -> SBE wire bytes.
// ---------------------------------------------------------------------------

// Encode OrderAccepted -> ExecutionReportNew522.
inline size_t encode_exec_new(
    char* buf,
    const OrderAccepted& evt,
    const Order& order,
    const EncodeContext& ctx)
{
    auto hdr = make_header<ExecutionReportNew522>();
    char* p = hdr.encode_to(buf);

    ExecutionReportNew522 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.seq_num = ctx.seq_num;
    msg.uuid = ctx.uuid;
    std::snprintf(msg.exec_id, sizeof(msg.exec_id), "E%lu",
                  static_cast<unsigned long>(evt.id));
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(evt.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_id = evt.id;
    msg.price = engine_price_to_price9(order.price);
    msg.stop_px = engine_price_to_pricenull9(0);
    msg.transact_time = static_cast<uint64_t>(evt.ts);
    msg.sending_time_epoch = static_cast<uint64_t>(evt.ts);
    msg.order_request_id = evt.client_order_id;
    msg.cross_id = UINT64_NULL;
    msg.host_cross_id = UINT64_NULL;
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.security_id = ctx.security_id;
    msg.order_qty = engine_qty_to_wire(order.quantity);
    msg.min_qty = UINT32_NULL;
    msg.display_qty = (order.display_qty > 0)
        ? engine_qty_to_wire(order.display_qty) : UINT32_NULL;
    msg.expire_date = UINT16_NULL;
    msg.delay_duration = UINT16_NULL;
    msg.ord_type = encode_ord_type(order.type);
    msg.side = encode_side(order.side);
    msg.time_in_force = encode_tif(order.tif);
    msg.manual_order_indicator = 0;
    msg.poss_retrans_flag = 0;
    msg.split_msg = UINT8_NULL;
    msg.cross_type = UINT8_NULL;
    msg.exec_inst = 0;
    msg.execution_mode = static_cast<uint8_t>(ExecMode::Aggressive);
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;
    msg.delay_to_time = UINT64_NULL;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(ExecutionReportNew522);
}

// Encode fill -> ExecutionReportTradeOutright525 (root block only, no groups).
inline size_t encode_exec_fill(
    char* buf,
    const OrderFilled& evt,
    const Order& order,
    bool is_aggressor,
    const EncodeContext& ctx)
{
    auto hdr = make_header<ExecutionReportTradeOutright525>();
    char* p = hdr.encode_to(buf);

    ExecutionReportTradeOutright525 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.seq_num = ctx.seq_num;
    msg.uuid = ctx.uuid;
    std::snprintf(msg.exec_id, sizeof(msg.exec_id), "T%lu",
                  static_cast<unsigned long>(evt.aggressor_id));
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(order.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.last_px = engine_price_to_price9(evt.price);
    msg.order_id = order.id;
    msg.price = engine_price_to_price9(order.price);
    msg.stop_px = PRICE9{INT64_NULL};
    msg.transact_time = static_cast<uint64_t>(evt.ts);
    msg.sending_time_epoch = static_cast<uint64_t>(evt.ts);
    msg.order_request_id = order.client_order_id;
    msg.sec_exec_id = ctx.seq_num;
    msg.cross_id = UINT64_NULL;
    msg.host_cross_id = UINT64_NULL;
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.security_id = ctx.security_id;
    msg.order_qty = engine_qty_to_wire(order.quantity);
    msg.last_qty = engine_qty_to_wire(evt.quantity);
    Quantity cum = order.filled_quantity + evt.quantity;
    Quantity leaves = order.quantity - cum;
    if (leaves < 0) leaves = 0;
    msg.cum_qty = engine_qty_to_wire(cum);
    msg.leaves_qty = engine_qty_to_wire(leaves);
    msg.trade_date = UINT16_NULL;
    msg.expire_date = UINT16_NULL;
    msg.ord_status = (leaves == 0)
        ? static_cast<uint8_t>(OrdStatus::Filled)
        : static_cast<uint8_t>(OrdStatus::PartiallyFilled);
    msg.ord_type = encode_ord_type(order.type);
    msg.side = encode_side(order.side);
    msg.time_in_force = encode_tif(order.tif);
    msg.manual_order_indicator = 0;
    msg.poss_retrans_flag = 0;
    msg.aggressor_indicator = is_aggressor ? 1 : 0;
    msg.cross_type = UINT8_NULL;
    msg.exec_inst = 0;
    msg.execution_mode = static_cast<uint8_t>(ExecMode::Aggressive);
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;
    msg.ownership = 0;

    // Append empty NoFills group header
    std::memcpy(p, &msg, sizeof(msg));
    p += sizeof(msg);
    GroupHeader gh{};
    gh.block_length = sizeof(FillEntry);
    gh.num_in_group = 0;
    p = gh.encode_to(p);
    // Append empty NoOrderEvents group header
    GroupHeader gh2{};
    gh2.block_length = sizeof(OrderEventEntry);
    gh2.num_in_group = 0;
    p = gh2.encode_to(p);

    return static_cast<size_t>(p - buf);
}

// Encode OrderCancelled -> ExecutionReportCancel534.
inline size_t encode_exec_cancel(
    char* buf,
    const OrderCancelled& evt,
    const Order& order,
    const EncodeContext& ctx)
{
    auto hdr = make_header<ExecutionReportCancel534>();
    char* p = hdr.encode_to(buf);

    ExecutionReportCancel534 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.seq_num = ctx.seq_num;
    msg.uuid = ctx.uuid;
    std::snprintf(msg.exec_id, sizeof(msg.exec_id), "C%lu",
                  static_cast<unsigned long>(evt.id));
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(order.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_id = evt.id;
    msg.price = engine_price_to_price9(order.price);
    msg.stop_px = PRICE9{INT64_NULL};
    msg.transact_time = static_cast<uint64_t>(evt.ts);
    msg.sending_time_epoch = static_cast<uint64_t>(evt.ts);
    msg.order_request_id = order.client_order_id;
    msg.cross_id = UINT64_NULL;
    msg.host_cross_id = UINT64_NULL;
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.security_id = ctx.security_id;
    msg.order_qty = engine_qty_to_wire(order.quantity);
    msg.cum_qty = engine_qty_to_wire(order.filled_quantity);
    msg.min_qty = UINT32_NULL;
    msg.display_qty = UINT32_NULL;
    msg.expire_date = UINT16_NULL;
    msg.delay_duration = UINT16_NULL;
    msg.ord_type = encode_ord_type(order.type);
    msg.side = encode_side(order.side);
    msg.time_in_force = encode_tif(order.tif);
    msg.manual_order_indicator = 0;
    msg.poss_retrans_flag = 0;
    msg.split_msg = UINT8_NULL;
    msg.exec_restatement_reason = static_cast<uint8_t>(evt.reason);
    msg.cross_type = UINT8_NULL;
    msg.exec_inst = 0;
    msg.execution_mode = static_cast<uint8_t>(ExecMode::Aggressive);
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;
    msg.delay_to_time = UINT64_NULL;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(ExecutionReportCancel534);
}

// Encode OrderRejected -> ExecutionReportReject523.
inline size_t encode_exec_reject(
    char* buf,
    const OrderRejected& evt,
    const EncodeContext& ctx)
{
    auto hdr = make_header<ExecutionReportReject523>();
    char* p = hdr.encode_to(buf);

    ExecutionReportReject523 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.seq_num = ctx.seq_num;
    msg.uuid = ctx.uuid;
    // text left as zeros (no specific reject text)
    std::snprintf(msg.exec_id, sizeof(msg.exec_id), "R%lu",
                  static_cast<unsigned long>(evt.client_order_id));
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(evt.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_id = UINT64_NULL;
    msg.price = PRICE9{INT64_NULL};
    msg.stop_px = PRICE9{INT64_NULL};
    msg.transact_time = static_cast<uint64_t>(evt.ts);
    msg.sending_time_epoch = static_cast<uint64_t>(evt.ts);
    msg.order_request_id = evt.client_order_id;
    msg.cross_id = UINT64_NULL;
    msg.host_cross_id = UINT64_NULL;
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.security_id = ctx.security_id;
    msg.order_qty = 0;
    msg.min_qty = UINT32_NULL;
    msg.display_qty = UINT32_NULL;
    msg.ord_rej_reason = static_cast<uint16_t>(evt.reason);
    msg.expire_date = UINT16_NULL;
    msg.delay_duration = UINT16_NULL;
    msg.ord_type = UINT8_NULL;          // unknown on reject
    msg.side = UINT8_NULL;              // unknown on reject
    msg.time_in_force = UINT8_NULL;     // unknown on reject
    msg.manual_order_indicator = 0;
    msg.poss_retrans_flag = 0;
    msg.split_msg = UINT8_NULL;
    msg.cross_type = UINT8_NULL;
    msg.exec_inst = 0;
    msg.execution_mode = UINT8_NULL;    // unknown on reject
    msg.liquidity_flag = UINT8_NULL;
    msg.managed_order = UINT8_NULL;
    msg.short_sale_type = 0;
    msg.delay_to_time = UINT64_NULL;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(ExecutionReportReject523);
}

// Encode OrderCancelRejected -> OrderCancelReject535.
inline size_t encode_cancel_reject(
    char* buf,
    const OrderCancelRejected& evt,
    const EncodeContext& ctx)
{
    auto hdr = make_header<OrderCancelReject535>();
    char* p = hdr.encode_to(buf);

    OrderCancelReject535 msg{};
    std::memset(&msg, 0, sizeof(msg));

    msg.seq_num = ctx.seq_num;
    msg.uuid = ctx.uuid;
    std::snprintf(msg.exec_id, sizeof(msg.exec_id), "CR%lu",
                  static_cast<unsigned long>(evt.id));
    std::memcpy(msg.sender_id, ctx.sender_id, sizeof(msg.sender_id));
    std::snprintf(msg.cl_ord_id, sizeof(msg.cl_ord_id), "%lu",
                  static_cast<unsigned long>(evt.client_order_id));
    msg.party_details_list_req_id = ctx.party_details_list_req_id;
    msg.order_id = evt.id;
    msg.transact_time = static_cast<uint64_t>(evt.ts);
    msg.sending_time_epoch = static_cast<uint64_t>(evt.ts);
    msg.order_request_id = evt.client_order_id;
    std::memcpy(msg.location, ctx.location, sizeof(msg.location));
    msg.cxl_rej_reason = static_cast<uint16_t>(evt.reason);
    msg.delay_duration = UINT16_NULL;
    msg.manual_order_indicator = 0;
    msg.poss_retrans_flag = 0;
    msg.split_msg = UINT8_NULL;
    msg.liquidity_flag = UINT8_NULL;
    msg.delay_to_time = UINT64_NULL;

    std::memcpy(p, &msg, sizeof(msg));
    return sizeof(MessageHeader) + sizeof(OrderCancelReject535);
}

// Maximum buffer size needed for any single encoded message.
constexpr size_t MAX_ENCODED_SIZE =
    sizeof(MessageHeader) + sizeof(ExecutionReportReject523) + 16;

}  // namespace exchange::cme::sbe::ilink3
