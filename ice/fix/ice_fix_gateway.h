#pragma once

#include "exchange-core/matching_engine.h"
#include "exchange-core/types.h"
#include "ice/fix/fix_parser.h"
#include "ice/fix/ice_fix_messages.h"

#include <cstdlib>
#include <string>

namespace exchange::ice::fix {

// Bring parser types from ice::fix into this namespace.
using ::ice::fix::FixMessage;
using ::ice::fix::FixNewOrderSingle;
using ::ice::fix::FixCancelRequest;
using ::ice::fix::FixCancelReplaceRequest;
using ::ice::fix::ParseResult;
namespace tags = ::ice::fix::tags;

// Result of a gateway dispatch call.
struct GatewayResult {
    bool ok{false};
    std::string error;

    static GatewayResult success() { return {true, {}}; }
    static GatewayResult fail(std::string err) { return {false, std::move(err)}; }
};

// ---------------------------------------------------------------------------
// FIX field -> engine type conversions.
// FIX uses fixed-point decimal strings; engine uses int64_t with PRICE_SCALE.
// ---------------------------------------------------------------------------

// Parse FIX decimal price string (e.g. "105.50") to engine Price.
// Returns 0 on parse failure.
inline Price fix_price_to_engine(const std::string& s) {
    if (s.empty()) return 0;
    // strtod is locale-sensitive; FIX mandates '.' (see fix_parser.cc comment).
    double d = std::strtod(s.c_str(), nullptr);
    return static_cast<Price>(d * PRICE_SCALE + (d >= 0 ? 0.5 : -0.5));
}

// Parse FIX qty string to engine Quantity (same scale as Price).
inline Quantity fix_qty_to_engine(const std::string& s) {
    if (s.empty()) return 0;
    double d = std::strtod(s.c_str(), nullptr);
    return static_cast<Quantity>(d * PRICE_SCALE + (d >= 0 ? 0.5 : -0.5));
}

// FIX Side: '1'=Buy, '2'=Sell.
inline bool fix_to_side(char c, Side& out) {
    if (c == '1') { out = Side::Buy; return true; }
    if (c == '2') { out = Side::Sell; return true; }
    return false;
}

// FIX OrdType: '1'=Market, '2'=Limit, '3'=Stop, '4'=StopLimit.
inline bool fix_to_order_type(char c, OrderType& out) {
    switch (c) {
        case '1': out = OrderType::Market;    return true;
        case '2': out = OrderType::Limit;     return true;
        case '3': out = OrderType::Stop;      return true;
        case '4': out = OrderType::StopLimit;  return true;
        default: return false;
    }
}

// FIX TimeInForce: '0'=DAY, '1'=GTC, '3'=IOC, '4'=FOK, '6'=GTD.
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
// IceFixGateway -- decodes incoming FIX messages and dispatches to engine.
//
// Template parameter EngineT must expose:
//   void new_order(const OrderRequest&)
//   void cancel_order(OrderId, Timestamp)
//   void modify_order(const ModifyRequest&)
//   size_t mass_cancel(uint64_t account_id, Timestamp)
//
// Off-hot-path component: FIX text parsing is inherently allocation-heavy.
// ---------------------------------------------------------------------------

template <typename EngineT>
class IceFixGateway {
public:
    explicit IceFixGateway(EngineT& engine) : engine_(engine) {}

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

    // Mass cancel by account. Not triggered by a FIX message type directly;
    // called by the session layer when an ICE mass cancel request is received.
    GatewayResult mass_cancel(uint64_t account_id, Timestamp ts) {
        engine_.mass_cancel(account_id, ts);
        return GatewayResult::success();
    }

private:
    GatewayResult handle_new_order(const FixMessage& msg, Timestamp ts) {
        auto nos = FixNewOrderSingle::from_fix(msg);

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
        auto cr = FixCancelRequest::from_fix(msg);

        if (cr.orig_cl_ord_id.empty()) {
            return GatewayResult::fail("missing OrigClOrdID (tag 41)");
        }

        OrderId orig_id = static_cast<OrderId>(std::strtoll(
            cr.orig_cl_ord_id.c_str(), nullptr, 10));

        engine_.cancel_order(orig_id, ts);
        return GatewayResult::success();
    }

    GatewayResult handle_replace(const FixMessage& msg, Timestamp ts) {
        auto crr = FixCancelReplaceRequest::from_fix(msg);

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
};

}  // namespace exchange::ice::fix
