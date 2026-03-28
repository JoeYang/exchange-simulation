#pragma once

#include "exchange-core/matching_engine.h"
#include "exchange-core/types.h"
#include "ice/fix/fix_parser.h"
#include "krx/fix/krx_fix_messages.h"

#include <cstdlib>
#include <string>

namespace exchange::krx::fix {

// Bring parser types into this namespace.
using ::ice::fix::FixMessage;
using ::ice::fix::ParseResult;

// Result of a gateway dispatch call.
struct GatewayResult {
    bool ok{false};
    std::string error;

    static GatewayResult success() { return {true, {}}; }
    static GatewayResult fail(std::string err) { return {false, std::move(err)}; }
};

// ---------------------------------------------------------------------------
// FIX field -> engine type conversions.
// Identical to ICE conversions (same fixed-point scale). Duplicated here
// to avoid coupling KRX to ICE namespace internals.
// ---------------------------------------------------------------------------

inline Price fix_price_to_engine(const std::string& s) {
    if (s.empty()) return 0;
    double d = std::strtod(s.c_str(), nullptr);
    return static_cast<Price>(d * PRICE_SCALE + (d >= 0 ? 0.5 : -0.5));
}

inline Quantity fix_qty_to_engine(const std::string& s) {
    if (s.empty()) return 0;
    double d = std::strtod(s.c_str(), nullptr);
    return static_cast<Quantity>(d * PRICE_SCALE + (d >= 0 ? 0.5 : -0.5));
}

inline bool fix_to_side(char c, Side& out) {
    if (c == '1') { out = Side::Buy; return true; }
    if (c == '2') { out = Side::Sell; return true; }
    return false;
}

inline bool fix_to_order_type(char c, OrderType& out) {
    switch (c) {
        case '1': out = OrderType::Market;    return true;
        case '2': out = OrderType::Limit;     return true;
        case '3': out = OrderType::Stop;      return true;
        case '4': out = OrderType::StopLimit;  return true;
        default: return false;
    }
}

inline bool fix_to_tif(char c, TimeInForce& out) {
    switch (c) {
        case '0': out = TimeInForce::DAY; return true;
        case '1': out = TimeInForce::GTC; return true;
        case '3': out = TimeInForce::IOC; return true;
        case '4': out = TimeInForce::FOK; return true;
        case '6': out = TimeInForce::GTD; return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// KrxFixGateway -- decodes incoming FIX messages and dispatches to engine.
//
// Template parameter EngineT must expose:
//   void new_order(const OrderRequest&)
//   void cancel_order(OrderId, Timestamp)
//   void modify_order(const ModifyRequest&)
//
// KRX-specific behavior:
//   - Extracts program_trading flag (tag 5001) and stores it in the
//     OrderRequest for sidecar validation by the KrxExchange CRTP hook.
//   - Extracts board_id (tag 5003) for multi-instrument routing.
//
// Off-hot-path component: FIX text parsing is inherently allocation-heavy.
// ---------------------------------------------------------------------------

template <typename EngineT>
class KrxFixGateway {
public:
    explicit KrxFixGateway(EngineT& engine) : engine_(engine) {}

    // Process a raw FIX message. Returns success/error.
    GatewayResult on_message(const char* data, size_t len, Timestamp ts) {
        auto result = ::ice::fix::parse_fix_message(data, len);
        if (!result.has_value()) {
            return GatewayResult::fail("parse error: " + result.error());
        }

        const auto& msg = result.value();
        if (msg.msg_type == "D") {
            return handle_new_order(msg, ts);
        } else if (msg.msg_type == "F") {
            return handle_cancel(msg, ts);
        } else if (msg.msg_type == "G") {
            return handle_replace(msg, ts);
        }
        // Session-level messages (A, 5, 0, 1) are not dispatched to the engine.
        return GatewayResult::success();
    }

    // Accessor: was the last parsed NOS a program trade?
    // Useful for the simulator layer to pass sidecar context to the engine.
    bool last_order_is_program_trade() const { return last_program_trade_; }

    // Accessor: board_id from last parsed NOS.
    const std::string& last_board_id() const { return last_board_id_; }

private:
    GatewayResult handle_new_order(const FixMessage& msg, Timestamp ts) {
        auto nos = KrxNewOrderSingle::from_fix(msg);

        if (nos.cl_ord_id.empty()) {
            return GatewayResult::fail("missing ClOrdID (tag 11)");
        }

        Side side;
        if (!fix_to_side(nos.side, side)) {
            return GatewayResult::fail("invalid Side (tag 54)");
        }

        OrderType ord_type;
        if (!fix_to_order_type(nos.ord_type, ord_type)) {
            return GatewayResult::fail("invalid OrdType (tag 40)");
        }

        TimeInForce tif;
        if (!fix_to_tif(nos.time_in_force, tif)) {
            return GatewayResult::fail("invalid TimeInForce (tag 59)");
        }

        // Track KRX-specific metadata for upstream use
        last_program_trade_ = (nos.program_trading == '1');
        last_board_id_ = nos.board_id;

        OrderRequest req{};
        req.client_order_id = static_cast<uint64_t>(std::strtoll(
            nos.cl_ord_id.c_str(), nullptr, 10));
        req.account_id = nos.account.empty() ? 0 :
            static_cast<uint64_t>(std::strtoll(nos.account.c_str(), nullptr, 10));
        req.side = side;
        req.type = ord_type;
        req.tif = tif;
        req.price = fix_price_to_engine(msg.get_string(tags::Price));
        req.quantity = fix_qty_to_engine(msg.get_string(tags::OrderQty));
        req.timestamp = ts;
        req.display_qty = fix_qty_to_engine(
            msg.get_string(tags::MaxFloor));

        engine_.new_order(req);
        return GatewayResult::success();
    }

    GatewayResult handle_cancel(const FixMessage& msg, Timestamp ts) {
        auto cr = KrxCancelRequest::from_fix(msg);

        if (cr.orig_cl_ord_id.empty()) {
            return GatewayResult::fail("missing OrigClOrdID (tag 41)");
        }

        OrderId orig_id = static_cast<OrderId>(std::strtoll(
            cr.orig_cl_ord_id.c_str(), nullptr, 10));

        engine_.cancel_order(orig_id, ts);
        return GatewayResult::success();
    }

    GatewayResult handle_replace(const FixMessage& msg, Timestamp ts) {
        auto crr = KrxCancelReplaceRequest::from_fix(msg);

        if (crr.orig_cl_ord_id.empty()) {
            return GatewayResult::fail("missing OrigClOrdID (tag 41)");
        }

        ModifyRequest req{};
        req.order_id = static_cast<OrderId>(std::strtoll(
            crr.orig_cl_ord_id.c_str(), nullptr, 10));
        req.client_order_id = static_cast<uint64_t>(std::strtoll(
            crr.cl_ord_id.c_str(), nullptr, 10));
        req.new_price = fix_price_to_engine(msg.get_string(tags::Price));
        req.new_quantity = fix_qty_to_engine(msg.get_string(tags::OrderQty));
        req.timestamp = ts;

        engine_.modify_order(req);
        return GatewayResult::success();
    }

    EngineT& engine_;
    bool last_program_trade_{false};
    std::string last_board_id_;
};

}  // namespace exchange::krx::fix
