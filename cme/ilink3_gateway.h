#pragma once

#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_messages.h"
#include "cme/codec/sbe_header.h"
#include "exchange-core/matching_engine.h"
#include "exchange-core/types.h"
#include "exchange-sim/instrument_config.h"

#include <cstddef>
#include <cstdint>

namespace exchange::cme {

// ---------------------------------------------------------------------------
// GatewayResult — outcome of processing an incoming iLink3 message.
// ---------------------------------------------------------------------------

enum class GatewayResult : uint8_t {
    kOk,
    kDecodeError,
    kUnknownInstrument,
    kUnsupportedMessage,
};

// ---------------------------------------------------------------------------
// ILink3Gateway — decodes incoming iLink3 SBE bytes and dispatches to
// a multi-instrument exchange simulator.
//
// SimulatorT must provide:
//   - new_order(InstrumentId, const OrderRequest&)
//   - cancel_order(InstrumentId, OrderId, Timestamp)
//   - modify_order(InstrumentId, const ModifyRequest&)
//   - mass_cancel_all(Timestamp)
//   - get_engine(InstrumentId)
//
// Maps SBE security_id to InstrumentId (they are the same integer in our
// simulator — CmeProductConfig::instrument_id == security_id on wire).
//
// No heap allocation. No exceptions. Suitable for the hot path.
// ---------------------------------------------------------------------------

template <typename SimulatorT>
class ILink3Gateway {
public:
    explicit ILink3Gateway(SimulatorT& sim) : sim_(sim) {}

    // Process a raw SBE message buffer.
    // Returns GatewayResult indicating success or the kind of failure.
    GatewayResult process(const char* buf, size_t len) {
        using namespace sbe;
        using namespace sbe::ilink3;

        auto rc = decode_ilink3_message(buf, len, [this](const auto& decoded) {
            dispatch(decoded);
        });

        if (rc == DecodeResult::kUnknownTemplateId) {
            last_result_ = GatewayResult::kUnsupportedMessage;
            return last_result_;
        }
        if (rc != DecodeResult::kOk) {
            last_result_ = GatewayResult::kDecodeError;
            return last_result_;
        }

        return last_result_;
    }

private:
    // --- Dispatch overloads for each decoded message type ---

    void dispatch(const sbe::ilink3::DecodedNewOrder514& decoded) {
        using namespace sbe::ilink3;
        const auto& msg = decoded.root;

        auto instrument_id = static_cast<InstrumentId>(msg.security_id);
        if (!sim_.get_engine(instrument_id)) {
            last_result_ = GatewayResult::kUnknownInstrument;
            return;
        }

        OrderRequest req{};
        req.client_order_id = decode_cl_ord_id(msg.cl_ord_id);
        req.account_id = msg.party_details_list_req_id;
        req.side = decode_side(msg.side);
        req.type = decode_ord_type(msg.ord_type);
        req.tif = decode_tif(msg.time_in_force);
        req.price = price9_to_engine(msg.price);
        req.quantity = wire_qty_to_engine(msg.order_qty);
        req.stop_price = price9_to_engine(msg.stop_px);
        req.timestamp = static_cast<Timestamp>(msg.sending_time_epoch);
        req.gtd_expiry = 0;
        req.display_qty = wire_qty_to_engine(msg.display_qty);

        sim_.new_order(instrument_id, req);
        last_result_ = GatewayResult::kOk;
    }

    void dispatch(const sbe::ilink3::DecodedCancelRequest516& decoded) {
        using namespace sbe::ilink3;
        const auto& msg = decoded.root;

        auto instrument_id = static_cast<InstrumentId>(msg.security_id);
        if (!sim_.get_engine(instrument_id)) {
            last_result_ = GatewayResult::kUnknownInstrument;
            return;
        }

        auto order_id = static_cast<OrderId>(msg.order_id);
        auto ts = static_cast<Timestamp>(msg.sending_time_epoch);

        sim_.cancel_order(instrument_id, order_id, ts);
        last_result_ = GatewayResult::kOk;
    }

    void dispatch(const sbe::ilink3::DecodedReplaceRequest515& decoded) {
        using namespace sbe::ilink3;
        const auto& msg = decoded.root;

        auto instrument_id = static_cast<InstrumentId>(msg.security_id);
        if (!sim_.get_engine(instrument_id)) {
            last_result_ = GatewayResult::kUnknownInstrument;
            return;
        }

        ModifyRequest req{};
        req.order_id = static_cast<OrderId>(msg.order_id);
        req.client_order_id = decode_cl_ord_id(msg.cl_ord_id);
        req.new_price = price9_to_engine(msg.price);
        req.new_quantity = wire_qty_to_engine(msg.order_qty);
        req.timestamp = static_cast<Timestamp>(msg.sending_time_epoch);

        sim_.modify_order(instrument_id, req);
        last_result_ = GatewayResult::kOk;
    }

    void dispatch(const sbe::ilink3::DecodedMassAction529& decoded) {
        using namespace sbe::ilink3;
        const auto& msg = decoded.root;

        auto ts = static_cast<Timestamp>(msg.sending_time_epoch);

        // MassActionScope::Instrument cancels a single instrument.
        if (static_cast<MassActionScope>(msg.mass_action_scope) ==
            MassActionScope::Instrument) {
            auto instrument_id = static_cast<InstrumentId>(msg.security_id);
            if (!sim_.get_engine(instrument_id)) {
                last_result_ = GatewayResult::kUnknownInstrument;
                return;
            }
            // Use the engine's mass_cancel_all for the single instrument.
            sim_.get_engine(instrument_id)->mass_cancel_all(ts);
        } else {
            // Scope::All — cancel across all instruments.
            sim_.mass_cancel_all(ts);
        }

        last_result_ = GatewayResult::kOk;
    }

    // Execution reports are exchange->client — the gateway ignores them
    // (they would be handled by the client-side decoder, not the gateway).
    void dispatch(const sbe::ilink3::DecodedExecNew522&) {
        last_result_ = GatewayResult::kUnsupportedMessage;
    }
    void dispatch(const sbe::ilink3::DecodedExecReject523&) {
        last_result_ = GatewayResult::kUnsupportedMessage;
    }
    void dispatch(const sbe::ilink3::DecodedExecTrade525&) {
        last_result_ = GatewayResult::kUnsupportedMessage;
    }
    void dispatch(const sbe::ilink3::DecodedExecCancel534&) {
        last_result_ = GatewayResult::kUnsupportedMessage;
    }
    void dispatch(const sbe::ilink3::DecodedCancelReject535&) {
        last_result_ = GatewayResult::kUnsupportedMessage;
    }

    SimulatorT& sim_;
    GatewayResult last_result_{GatewayResult::kOk};
};

}  // namespace exchange::cme
