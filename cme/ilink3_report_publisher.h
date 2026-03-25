#pragma once

#include "cme/codec/ilink3_encoder.h"
#include "exchange-core/listeners.h"
#include "exchange-core/types.h"

#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace exchange::cme {

// Encoded SBE report stored as a fixed-size byte buffer + actual length.
struct EncodedReport {
    static constexpr size_t MAX_SIZE = sbe::ilink3::MAX_ENCODED_SIZE;
    std::array<char, MAX_SIZE> data{};
    size_t length{0};
    uint16_t template_id{0};
};

// ILink3ReportPublisher — OrderListenerBase that encodes every engine callback
// as iLink3 SBE ExecutionReport bytes and stores them for retrieval.
//
// Maintains a lightweight order map so that fill/cancel reports can include
// full order context (price, qty, side, etc.) that the engine events alone
// don't carry.
//
// Not intended for hot-path use — the std::vector and std::unordered_map
// allocate. Suitable for simulation, testing, and journal recording.
class ILink3ReportPublisher : public OrderListenerBase {
public:
    explicit ILink3ReportPublisher(sbe::ilink3::EncodeContext ctx)
        : ctx_(ctx) {}

    // -- OrderListenerBase overrides --

    void on_order_accepted(const OrderAccepted& e) {
        // Store order state for future fill/cancel encoding.
        OrderState state{};
        state.order.id = e.id;
        state.order.client_order_id = e.client_order_id;
        // Price/qty/side are set via register_order() before submit.
        auto it = pending_.find(e.client_order_id);
        if (it != pending_.end()) {
            state.order = it->second;
            state.order.id = e.id;
            pending_.erase(it);
        }
        orders_[e.id] = state;

        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_exec_new(
            report_buf_, e, state.order, ctx_);
        store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_NEW_ID);
    }

    void on_order_rejected(const OrderRejected& e) {
        pending_.erase(e.client_order_id);
        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_exec_reject(report_buf_, e, ctx_);
        store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_REJECT_ID);
    }

    void on_order_filled(const OrderFilled& e) {
        // Emit fill report for the resting order.
        auto it = orders_.find(e.resting_id);
        Order resting_order{};
        if (it != orders_.end()) {
            resting_order = it->second.order;
            orders_.erase(it);  // fully filled, remove
        }

        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_exec_fill(
            report_buf_, e, resting_order, false, ctx_);
        store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_TRADE_OUTRIGHT_ID);

        // Emit fill report for the aggressor order.
        auto ag_it = orders_.find(e.aggressor_id);
        if (ag_it != orders_.end()) {
            Order aggressor_order = ag_it->second.order;
            aggressor_order.filled_quantity += e.quantity;
            // If aggressor is also fully filled, remove it.
            if (aggressor_order.filled_quantity >= aggressor_order.quantity) {
                orders_.erase(ag_it);
            } else {
                ag_it->second.order = aggressor_order;
            }

            ctx_.seq_num++;
            n = sbe::ilink3::encode_exec_fill(
                report_buf_, e, aggressor_order, true, ctx_);
            store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_TRADE_OUTRIGHT_ID);
        }
    }

    void on_order_partially_filled(const OrderPartiallyFilled& e) {
        // Reuse the fill encoder — partial fills are the same wire message.
        OrderFilled fill_evt{};
        fill_evt.aggressor_id = e.aggressor_id;
        fill_evt.resting_id = e.resting_id;
        fill_evt.price = e.price;
        fill_evt.quantity = e.quantity;
        fill_evt.ts = e.ts;

        // Emit fill report for the resting order.
        auto it = orders_.find(e.resting_id);
        Order resting_order{};
        if (it != orders_.end()) {
            resting_order = it->second.order;
            resting_order.filled_quantity += e.quantity;
            it->second.order = resting_order;
        }

        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_exec_fill(
            report_buf_, fill_evt, resting_order, false, ctx_);
        store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_TRADE_OUTRIGHT_ID);

        // Emit fill report for the aggressor order.
        auto ag_it = orders_.find(e.aggressor_id);
        if (ag_it != orders_.end()) {
            Order aggressor_order = ag_it->second.order;
            aggressor_order.filled_quantity += e.quantity;
            ag_it->second.order = aggressor_order;

            ctx_.seq_num++;
            n = sbe::ilink3::encode_exec_fill(
                report_buf_, fill_evt, aggressor_order, true, ctx_);
            store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_TRADE_OUTRIGHT_ID);
        }
    }

    void on_order_cancelled(const OrderCancelled& e) {
        auto it = orders_.find(e.id);
        Order order{};
        if (it != orders_.end()) {
            order = it->second.order;
            orders_.erase(it);
        }

        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_exec_cancel(
            report_buf_, e, order, ctx_);
        store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_CANCEL_ID);
    }

    void on_order_cancel_rejected(const OrderCancelRejected& e) {
        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_cancel_reject(report_buf_, e, ctx_);
        store_report(report_buf_, n, sbe::ilink3::ORDER_CANCEL_REJECT_ID);
    }

    void on_order_modified(const OrderModified& e) {
        auto it = orders_.find(e.id);
        if (it != orders_.end()) {
            it->second.order.price = e.new_price;
            it->second.order.quantity = e.new_qty;
            it->second.order.client_order_id = e.client_order_id;
        }

        // CME modify ack is an ExecutionReportNew (cancel-replace).
        OrderAccepted accept{};
        accept.id = e.id;
        accept.client_order_id = e.client_order_id;
        accept.ts = e.ts;

        Order order{};
        if (it != orders_.end()) order = it->second.order;

        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_exec_new(
            report_buf_, accept, order, ctx_);
        store_report(report_buf_, n, sbe::ilink3::EXEC_REPORT_NEW_ID);
    }

    void on_order_modify_rejected(const OrderModifyRejected& e) {
        // Modify reject uses the same wire format as cancel reject.
        OrderCancelRejected cxl_rej{};
        cxl_rej.id = e.id;
        cxl_rej.client_order_id = e.client_order_id;
        cxl_rej.ts = e.ts;
        cxl_rej.reason = e.reason;

        ctx_.seq_num++;
        size_t n = sbe::ilink3::encode_cancel_reject(
            report_buf_, cxl_rej, ctx_);
        store_report(report_buf_, n, sbe::ilink3::ORDER_CANCEL_REJECT_ID);
    }

    // -- Order registration --
    // Call before submitting an OrderRequest so the publisher knows the
    // order's price/qty/side when the accepted callback arrives.
    void register_order(const OrderRequest& req) {
        Order o{};
        o.client_order_id = req.client_order_id;
        o.price = req.price;
        o.quantity = req.quantity;
        o.side = req.side;
        o.type = req.type;
        o.tif = req.tif;
        o.display_qty = req.display_qty;
        pending_[req.client_order_id] = o;
    }

    // -- Report access --
    const std::vector<EncodedReport>& reports() const { return reports_; }
    size_t report_count() const { return reports_.size(); }
    void clear_reports() { reports_.clear(); }

    const sbe::ilink3::EncodeContext& context() const { return ctx_; }
    sbe::ilink3::EncodeContext& context() { return ctx_; }

private:
    void store_report(const char* buf, size_t len, uint16_t tmpl_id) {
        EncodedReport r{};
        r.length = len;
        r.template_id = tmpl_id;
        std::memcpy(r.data.data(), buf, len);
        reports_.push_back(r);
    }

    struct OrderState {
        Order order{};
    };

    sbe::ilink3::EncodeContext ctx_;
    std::unordered_map<OrderId, OrderState> orders_;
    std::unordered_map<uint64_t, Order> pending_;  // keyed by client_order_id
    std::vector<EncodedReport> reports_;
    char report_buf_[EncodedReport::MAX_SIZE]{};
};

}  // namespace exchange::cme
