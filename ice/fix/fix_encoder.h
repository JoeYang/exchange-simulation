#pragma once

#include "exchange-core/events.h"
#include "exchange-core/types.h"

#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace exchange::ice::fix {

// ---------------------------------------------------------------------------
// FIX 4.2 encoder — encode engine callback events as ExecutionReport (35=8).
//
// FIX is a text-based tag=value protocol with SOH (0x01) delimiters.
// Each message: 8=FIX.4.2|9=BodyLength|...body...|10=Checksum|
// Body length = bytes from after tag 9's SOH to the SOH before tag 10.
// Checksum = sum of all bytes before tag 10 (including its leading SOH), mod 256.
// ---------------------------------------------------------------------------

constexpr char SOH = '\x01';

// Session-level state carried through encoding calls.
struct EncodeContext {
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string symbol;
    uint64_t next_exec_id{1};
    uint32_t next_seq_num{1};
};

// ---------------------------------------------------------------------------
// Price/quantity conversion: engine fixed-point (PRICE_SCALE=10000) -> FIX decimal string.
// 50000000 -> "5000.0000", 10000 -> "1.0000"
// ---------------------------------------------------------------------------

inline std::string price_to_fix_str(Price p) {
    bool negative = p < 0;
    int64_t abs_p = negative ? -p : p;
    int64_t whole = abs_p / PRICE_SCALE;
    int64_t frac = abs_p % PRICE_SCALE;
    char buf[32];
    if (negative) {
        std::snprintf(buf, sizeof(buf), "-%" PRId64 ".%04" PRId64, whole, frac);
    } else {
        std::snprintf(buf, sizeof(buf), "%" PRId64 ".%04" PRId64, whole, frac);
    }
    return buf;
}

inline std::string qty_to_fix_str(Quantity q) {
    return price_to_fix_str(q);  // same scale
}

// ---------------------------------------------------------------------------
// FIX side encoding: Buy=1, Sell=2.
// ---------------------------------------------------------------------------

inline char encode_side(Side s) {
    return (s == Side::Buy) ? '1' : '2';
}

// ---------------------------------------------------------------------------
// Internal: build the body of a FIX message (tags 35 through the last
// content tag, before checksum). Returns body string WITH trailing SOH on
// each tag.
// ---------------------------------------------------------------------------

namespace detail {

// Append tag=value|SOH to a string.
inline void append_tag(std::string& out, const char* tag, const std::string& val) {
    out += tag;
    out += '=';
    out += val;
    out += SOH;
}

inline void append_tag(std::string& out, const char* tag, uint64_t val) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%" PRIu64, val);
    append_tag(out, tag, std::string(buf));
}

inline void append_tag(std::string& out, const char* tag, int64_t val) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%" PRId64, val);
    append_tag(out, tag, std::string(buf));
}

inline void append_tag(std::string& out, const char* tag, char val) {
    std::string s(1, val);
    append_tag(out, tag, s);
}

// Compute FIX checksum: sum of all bytes mod 256, zero-padded 3 digits.
inline std::string compute_checksum(const std::string& msg) {
    uint32_t sum = 0;
    for (char c : msg) {
        sum += static_cast<uint8_t>(c);
    }
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03u", sum % 256);
    return buf;
}

// Build SendingTime from engine timestamp (epoch nanoseconds).
// Format: YYYYMMDD-HH:MM:SS (simplified — uses epoch seconds).
inline std::string format_sending_time(Timestamp ts) {
    int64_t secs = ts / 1000000000LL;
    struct tm tm{};
    time_t t = static_cast<time_t>(secs);
    gmtime_r(&t, &tm);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// Assemble a complete FIX message from header tags and body.
// header_body = everything from tag 35 onward (not including 8, 9, 10).
inline std::string assemble_message(const std::string& body) {
    // 1. Compute body length
    std::string body_len = std::to_string(body.size());

    // 2. Build prefix: 8=FIX.4.2|9=BodyLength|
    std::string prefix;
    prefix += "8=FIX.4.2";
    prefix += SOH;
    prefix += "9=";
    prefix += body_len;
    prefix += SOH;

    // 3. Combine prefix + body
    std::string msg = prefix + body;

    // 4. Compute checksum over entire message so far
    std::string cksum = compute_checksum(msg);

    // 5. Append tag 10
    msg += "10=";
    msg += cksum;
    msg += SOH;

    return msg;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Public encode functions.
// Each returns a complete FIX 4.2 ExecutionReport (35=8) message string.
// The EncodeContext is mutated: seq_num and exec_id are incremented.
// ---------------------------------------------------------------------------

// OrderAccepted -> ExecType=New(0), OrdStatus=New(0)
inline std::string encode_exec_new(
    const OrderAccepted& evt,
    const Order& order,
    EncodeContext& ctx)
{
    std::string body;
    body.reserve(256);

    detail::append_tag(body, "35", std::string("8"));
    detail::append_tag(body, "49", ctx.sender_comp_id);
    detail::append_tag(body, "56", ctx.target_comp_id);
    detail::append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
    detail::append_tag(body, "52", detail::format_sending_time(evt.ts));
    detail::append_tag(body, "37", static_cast<uint64_t>(evt.id));
    detail::append_tag(body, "11", static_cast<uint64_t>(evt.client_order_id));
    detail::append_tag(body, "17", static_cast<uint64_t>(ctx.next_exec_id++));
    detail::append_tag(body, "150", '0');   // ExecType=New
    detail::append_tag(body, "39", '0');    // OrdStatus=New
    detail::append_tag(body, "55", ctx.symbol);
    detail::append_tag(body, "54", encode_side(order.side));
    detail::append_tag(body, "44", price_to_fix_str(order.price));
    detail::append_tag(body, "38", qty_to_fix_str(order.quantity));
    detail::append_tag(body, "151", qty_to_fix_str(order.quantity));  // LeavesQty = full qty
    detail::append_tag(body, "14", qty_to_fix_str(0));               // CumQty = 0
    detail::append_tag(body, "6", qty_to_fix_str(0));                // AvgPx = 0

    return detail::assemble_message(body);
}

// OrderFilled -> ExecType per FIX 4.2: '2'=Fill, '1'=PartialFill
inline std::string encode_exec_fill(
    const OrderFilled& evt,
    const Order& order,
    EncodeContext& ctx)
{
    Quantity cum_qty = order.filled_quantity + evt.quantity;
    Quantity leaves_qty = order.quantity - cum_qty;
    assert(leaves_qty >= 0 && "over-fill: cum_qty exceeds order quantity");

    // FIX 4.2 ExecType: '2'=Fill (fully filled), '1'=PartialFill
    // OrdStatus mirrors ExecType for fills
    char exec_type = (leaves_qty == 0) ? '2' : '1';
    char ord_status = exec_type;

    // AvgPx: for simplicity, use fill price (single fill snapshot)
    Price avg_px = evt.price;

    std::string body;
    body.reserve(384);

    detail::append_tag(body, "35", std::string("8"));
    detail::append_tag(body, "49", ctx.sender_comp_id);
    detail::append_tag(body, "56", ctx.target_comp_id);
    detail::append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
    detail::append_tag(body, "52", detail::format_sending_time(evt.ts));
    detail::append_tag(body, "37", static_cast<uint64_t>(order.id));
    detail::append_tag(body, "11", static_cast<uint64_t>(order.client_order_id));
    detail::append_tag(body, "17", static_cast<uint64_t>(ctx.next_exec_id++));
    detail::append_tag(body, "150", exec_type);    // ExecType per FIX 4.2
    detail::append_tag(body, "39", ord_status);    // OrdStatus
    detail::append_tag(body, "55", ctx.symbol);
    detail::append_tag(body, "54", encode_side(order.side));
    detail::append_tag(body, "44", price_to_fix_str(order.price));
    detail::append_tag(body, "38", qty_to_fix_str(order.quantity));
    detail::append_tag(body, "31", price_to_fix_str(evt.price));       // LastPx
    detail::append_tag(body, "32", qty_to_fix_str(evt.quantity));      // LastQty
    detail::append_tag(body, "151", qty_to_fix_str(leaves_qty));       // LeavesQty
    detail::append_tag(body, "14", qty_to_fix_str(cum_qty));           // CumQty
    detail::append_tag(body, "6", price_to_fix_str(avg_px));           // AvgPx

    return detail::assemble_message(body);
}

// OrderCancelled -> ExecType=Cancelled(4), OrdStatus=Cancelled(4)
inline std::string encode_exec_cancel(
    const OrderCancelled& evt,
    const Order& order,
    EncodeContext& ctx)
{
    std::string body;
    body.reserve(256);

    detail::append_tag(body, "35", std::string("8"));
    detail::append_tag(body, "49", ctx.sender_comp_id);
    detail::append_tag(body, "56", ctx.target_comp_id);
    detail::append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
    detail::append_tag(body, "52", detail::format_sending_time(evt.ts));
    detail::append_tag(body, "37", static_cast<uint64_t>(evt.id));
    detail::append_tag(body, "11", static_cast<uint64_t>(order.client_order_id));
    detail::append_tag(body, "17", static_cast<uint64_t>(ctx.next_exec_id++));
    detail::append_tag(body, "150", '4');   // ExecType=Cancelled
    detail::append_tag(body, "39", '4');    // OrdStatus=Cancelled
    detail::append_tag(body, "55", ctx.symbol);
    detail::append_tag(body, "54", encode_side(order.side));
    detail::append_tag(body, "44", price_to_fix_str(order.price));
    detail::append_tag(body, "38", qty_to_fix_str(order.quantity));
    detail::append_tag(body, "151", qty_to_fix_str(0));                          // LeavesQty = 0
    detail::append_tag(body, "14", qty_to_fix_str(order.filled_quantity));       // CumQty
    detail::append_tag(body, "6", qty_to_fix_str(0));                            // AvgPx = 0

    return detail::assemble_message(body);
}

// OrderRejected -> ExecType=Rejected(8), OrdStatus=Rejected(8)
inline std::string encode_exec_reject(
    const OrderRejected& evt,
    EncodeContext& ctx)
{
    std::string body;
    body.reserve(256);

    detail::append_tag(body, "35", std::string("8"));
    detail::append_tag(body, "49", ctx.sender_comp_id);
    detail::append_tag(body, "56", ctx.target_comp_id);
    detail::append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
    detail::append_tag(body, "52", detail::format_sending_time(evt.ts));
    // Tag 37 (OrderID) omitted — no order was assigned on reject
    detail::append_tag(body, "11", static_cast<uint64_t>(evt.client_order_id));
    detail::append_tag(body, "17", static_cast<uint64_t>(ctx.next_exec_id++));
    detail::append_tag(body, "150", '8');   // ExecType=Rejected
    detail::append_tag(body, "39", '8');    // OrdStatus=Rejected
    detail::append_tag(body, "55", ctx.symbol);
    // Tag 54 (Side) omitted — no side info available on reject
    detail::append_tag(body, "44", std::string("0"));
    detail::append_tag(body, "38", std::string("0"));
    detail::append_tag(body, "103", static_cast<int64_t>(static_cast<uint8_t>(evt.reason)));

    return detail::assemble_message(body);
}

// OrderModified -> ExecType=Replace(5), OrdStatus=New(0)
inline std::string encode_exec_replace(
    const OrderModified& evt,
    const Order& order,
    EncodeContext& ctx)
{
    std::string body;
    body.reserve(256);

    detail::append_tag(body, "35", std::string("8"));
    detail::append_tag(body, "49", ctx.sender_comp_id);
    detail::append_tag(body, "56", ctx.target_comp_id);
    detail::append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
    detail::append_tag(body, "52", detail::format_sending_time(evt.ts));
    detail::append_tag(body, "37", static_cast<uint64_t>(evt.id));
    detail::append_tag(body, "11", static_cast<uint64_t>(evt.client_order_id));
    detail::append_tag(body, "17", static_cast<uint64_t>(ctx.next_exec_id++));
    detail::append_tag(body, "150", '5');   // ExecType=Replace
    detail::append_tag(body, "39", '0');    // OrdStatus=New (active after replace)
    detail::append_tag(body, "55", ctx.symbol);
    detail::append_tag(body, "54", encode_side(order.side));
    detail::append_tag(body, "44", price_to_fix_str(evt.new_price));
    detail::append_tag(body, "38", qty_to_fix_str(evt.new_qty));
    Quantity leaves = evt.new_qty - order.filled_quantity;
    detail::append_tag(body, "151", qty_to_fix_str(leaves));                     // LeavesQty
    detail::append_tag(body, "14", qty_to_fix_str(order.filled_quantity));       // CumQty
    detail::append_tag(body, "6", qty_to_fix_str(0));                            // AvgPx = 0

    return detail::assemble_message(body);
}

}  // namespace exchange::ice::fix
