#pragma once

#include <cstdint>
#include <string>

#include "ice/fix/fix_parser.h"

namespace exchange::krx::fix {

// --- KRX-specific FIX tag constants ---
// Standard FIX 4.2 tags are reused from ice::fix::tags.
// KRX custom tags live in the user-defined range (5000+).

namespace tags {
// Standard FIX tags (duplicated here for self-contained KRX usage)
constexpr int Account = 1;
constexpr int BeginString = 8;
constexpr int BodyLength = 9;
constexpr int CheckSum = 10;
constexpr int ClOrdID = 11;
constexpr int ExecID = 17;
constexpr int LastPx = 31;
constexpr int LastQty = 32;
constexpr int MsgType = 35;
constexpr int OrderID = 37;
constexpr int OrderQty = 38;
constexpr int OrdStatus = 39;
constexpr int OrdType = 40;
constexpr int OrigClOrdID = 41;
constexpr int Price = 44;
constexpr int Side = 54;
constexpr int Symbol = 55;
constexpr int TimeInForce = 59;
constexpr int MaxFloor = 111;       // iceberg display qty
constexpr int ExecType = 150;
constexpr int LeavesQty = 151;
constexpr int CumQty = 14;
constexpr int AvgPx = 6;
constexpr int OrdRejReason = 103;

// KRX custom tags
constexpr int ProgramTradingFlag = 5001;  // '1'=program order, '0'=non-program
constexpr int InvestorType = 5002;        // investor classification code
constexpr int BoardId = 5003;             // KRX board identifier (e.g. "KOSPI200")
}  // namespace tags

// --- Typed message structs ---

struct KrxNewOrderSingle {
    std::string cl_ord_id;
    std::string account;
    std::string symbol;
    char side{'\0'};           // '1'=Buy, '2'=Sell
    int64_t order_qty{0};
    double price{0.0};
    char ord_type{'\0'};       // '1'=Market, '2'=Limit
    char time_in_force{'\0'};
    int64_t max_floor{0};      // iceberg display qty (0 = fully visible)
    // KRX custom fields
    char program_trading{'\0'};  // '1'=program, '0'=non-program
    std::string investor_type;
    std::string board_id;

    static KrxNewOrderSingle from_fix(const ::ice::fix::FixMessage& m) {
        KrxNewOrderSingle nos;
        nos.cl_ord_id = m.get_string(tags::ClOrdID);
        nos.account = m.get_string(tags::Account);
        nos.symbol = m.get_string(tags::Symbol);
        nos.side = m.get_char(tags::Side);
        nos.order_qty = m.get_int(tags::OrderQty);
        nos.price = m.get_double(tags::Price);
        nos.ord_type = m.get_char(tags::OrdType);
        nos.time_in_force = m.get_char(tags::TimeInForce);
        nos.max_floor = m.get_int(tags::MaxFloor);
        nos.program_trading = m.get_char(tags::ProgramTradingFlag);
        nos.investor_type = m.get_string(tags::InvestorType);
        nos.board_id = m.get_string(tags::BoardId);
        return nos;
    }
};

struct KrxCancelRequest {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    char side{'\0'};

    static KrxCancelRequest from_fix(const ::ice::fix::FixMessage& m) {
        KrxCancelRequest cr;
        cr.cl_ord_id = m.get_string(tags::ClOrdID);
        cr.orig_cl_ord_id = m.get_string(tags::OrigClOrdID);
        cr.symbol = m.get_string(tags::Symbol);
        cr.side = m.get_char(tags::Side);
        return cr;
    }
};

struct KrxCancelReplaceRequest {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    char side{'\0'};
    int64_t order_qty{0};
    double price{0.0};
    char ord_type{'\0'};

    static KrxCancelReplaceRequest from_fix(const ::ice::fix::FixMessage& m) {
        KrxCancelReplaceRequest crr;
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

struct KrxExecutionReport {
    std::string cl_ord_id;
    std::string exec_id;
    char exec_type{'\0'};
    char ord_status{'\0'};
    std::string symbol;
    char side{'\0'};
    double price{0.0};
    int64_t order_qty{0};
    // KRX custom fields echoed back
    std::string board_id;

    static KrxExecutionReport from_fix(const ::ice::fix::FixMessage& m) {
        KrxExecutionReport er;
        er.cl_ord_id = m.get_string(tags::ClOrdID);
        er.exec_id = m.get_string(tags::ExecID);
        er.exec_type = m.get_char(tags::ExecType);
        er.ord_status = m.get_char(tags::OrdStatus);
        er.symbol = m.get_string(tags::Symbol);
        er.side = m.get_char(tags::Side);
        er.price = m.get_double(tags::Price);
        er.order_qty = m.get_int(tags::OrderQty);
        er.board_id = m.get_string(tags::BoardId);
        return er;
    }
};

}  // namespace exchange::krx::fix
