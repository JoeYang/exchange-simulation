#pragma once

#include <cstdint>
#include <string>

#include "ice/fix/fix_parser.h"

namespace ice::fix {

// --- ICE FIX tag constants ---
namespace tags {
constexpr int Account = 1;
constexpr int BeginString = 8;
constexpr int BodyLength = 9;
constexpr int CheckSum = 10;
constexpr int ClOrdID = 11;
constexpr int ExecID = 17;
constexpr int MsgType = 35;
constexpr int OrderQty = 38;
constexpr int OrdStatus = 39;
constexpr int OrdType = 40;
constexpr int OrigClOrdID = 41;
constexpr int Price = 44;
constexpr int Side = 54;
constexpr int Symbol = 55;
constexpr int TimeInForce = 59;
constexpr int MaxFloor = 111;     // iceberg display qty
constexpr int ExecType = 150;
// ICE custom tags
constexpr int OriginatorUserID = 9139;
constexpr int AccountCode = 9195;
constexpr int MemoField = 9121;
}  // namespace tags

// --- Typed message structs ---

struct FixNewOrderSingle {
    std::string cl_ord_id;
    std::string account;
    std::string symbol;
    char side{'\0'};         // '1'=Buy, '2'=Sell
    int64_t order_qty{0};
    double price{0.0};
    char ord_type{'\0'};     // '1'=Market, '2'=Limit
    char time_in_force{'\0'};
    int64_t max_floor{0};    // iceberg display qty (0 = fully visible)
    // ICE custom
    std::string originator_user_id;
    std::string account_code;
    std::string memo;

    static FixNewOrderSingle from_fix(const FixMessage& m) {
        FixNewOrderSingle nos;
        nos.cl_ord_id = m.get_string(tags::ClOrdID);
        nos.account = m.get_string(tags::Account);
        nos.symbol = m.get_string(tags::Symbol);
        nos.side = m.get_char(tags::Side);
        nos.order_qty = m.get_int(tags::OrderQty);
        nos.price = m.get_double(tags::Price);
        nos.ord_type = m.get_char(tags::OrdType);
        nos.time_in_force = m.get_char(tags::TimeInForce);
        nos.max_floor = m.get_int(tags::MaxFloor);
        nos.originator_user_id = m.get_string(tags::OriginatorUserID);
        nos.account_code = m.get_string(tags::AccountCode);
        nos.memo = m.get_string(tags::MemoField);
        return nos;
    }
};

struct FixCancelRequest {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    char side{'\0'};

    static FixCancelRequest from_fix(const FixMessage& m) {
        FixCancelRequest cr;
        cr.cl_ord_id = m.get_string(tags::ClOrdID);
        cr.orig_cl_ord_id = m.get_string(tags::OrigClOrdID);
        cr.symbol = m.get_string(tags::Symbol);
        cr.side = m.get_char(tags::Side);
        return cr;
    }
};

struct FixCancelReplaceRequest {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    char side{'\0'};
    int64_t order_qty{0};
    double price{0.0};
    char ord_type{'\0'};

    static FixCancelReplaceRequest from_fix(const FixMessage& m) {
        FixCancelReplaceRequest crr;
        crr.cl_ord_id = m.get_string(tags::ClOrdID);
        crr.orig_cl_ord_id = m.get_string(tags::OrigClOrdID);
        crr.symbol = m.get_string(tags::Symbol);
        crr.side = m.get_char(tags::Side);
        crr.order_qty = m.get_int(tags::OrderQty);
        crr.price = m.get_double(tags::Price);
        crr.ord_type = m.get_char(tags::OrdType);
        return crr;
    }
};

struct FixExecutionReport {
    std::string cl_ord_id;
    std::string exec_id;
    char exec_type{'\0'};
    char ord_status{'\0'};
    std::string symbol;
    char side{'\0'};
    double price{0.0};
    int64_t order_qty{0};

    static FixExecutionReport from_fix(const FixMessage& m) {
        FixExecutionReport er;
        er.cl_ord_id = m.get_string(tags::ClOrdID);
        er.exec_id = m.get_string(tags::ExecID);
        er.exec_type = m.get_char(tags::ExecType);
        er.ord_status = m.get_char(tags::OrdStatus);
        er.symbol = m.get_string(tags::Symbol);
        er.side = m.get_char(tags::Side);
        er.price = m.get_double(tags::Price);
        er.order_qty = m.get_int(tags::OrderQty);
        return er;
    }
};

// --- Simple FIX encoder for round-trip testing ---
// Builds a valid FIX 4.2 message string from a FixMessage.
// Not optimized — intended for tests and off-hot-path use.
inline std::string encode_fix_message(const FixMessage& msg) {
    constexpr char SOH = '\x01';

    // Build body: 35=MsgType + all other fields
    std::string body = "35=" + msg.msg_type + SOH;
    for (const auto& [tag, val] : msg.fields) {
        if (tag == 35) continue;  // already emitted
        body += std::to_string(tag) + "=" + val + SOH;
    }

    std::string begin = std::string("8=FIX.4.2") + SOH;
    std::string body_len = "9=" + std::to_string(body.size()) + SOH;
    std::string pre_checksum = begin + body_len + body;

    // Checksum: sum of all bytes before 10= tag, mod 256
    uint32_t sum = 0;
    for (char c : pre_checksum) {
        sum += static_cast<uint8_t>(c);
    }
    char cs[4];
    std::snprintf(cs, sizeof(cs), "%03u", sum % 256);

    return pre_checksum + "10=" + std::string(cs, 3) + SOH;
}

}  // namespace ice::fix
