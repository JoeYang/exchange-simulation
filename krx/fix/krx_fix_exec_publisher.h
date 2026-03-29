#pragma once

#include "exchange-core/listeners.h"
#include "exchange-core/types.h"
#include "ice/fix/fix_encoder.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::krx::fix {

// ---------------------------------------------------------------------------
// KrxFixExecPublisher -- OrderListenerBase that encodes engine events as
// FIX 4.2 ExecutionReport messages with KRX-specific fields (board_id).
//
// Reuses ice::fix encoder helpers (detail::append_tag, assemble_message,
// price_to_fix_str, etc.) for the standard FIX body construction.
//
// Off-hot-path component: uses std::string, std::vector, std::unordered_map.
// ---------------------------------------------------------------------------

class KrxFixExecPublisher : public OrderListenerBase {
public:
    KrxFixExecPublisher(std::string sender, std::string target, std::string symbol)
    {
        ctx_.sender_comp_id = std::move(sender);
        ctx_.target_comp_id = std::move(target);
        ctx_.symbol = std::move(symbol);
    }

    // Register order info before submitting to the engine.
    // Stores in pending_ keyed by client_order_id; on_order_accepted()
    // moves to orders_ keyed by engine-assigned OrderId.
    void register_order(uint64_t client_order_id,
                        Price price, Quantity qty, Side side)
    {
        OrderInfo info;
        info.id = 0;  // placeholder; set in on_order_accepted
        info.client_order_id = client_order_id;
        info.price = price;
        info.quantity = qty;
        info.filled_quantity = 0;
        info.side = side;
        pending_[client_order_id] = info;
    }

    // --- OrderListenerBase callbacks (name hiding, not virtual) ---

    void on_order_accepted(const OrderAccepted& e) {
        auto pit = pending_.find(e.client_order_id);
        if (pit == pending_.end()) return;

        OrderInfo info = pit->second;
        info.id = e.id;
        pending_.erase(pit);
        orders_[e.id] = info;

        Order order = to_order(info);
        messages_.push_back(
            ::exchange::ice::fix::encode_exec_new(e, order, ctx_));
    }

    void on_order_rejected(const OrderRejected& e) {
        pending_.erase(e.client_order_id);
        messages_.push_back(
            ::exchange::ice::fix::encode_exec_reject(e, ctx_));
    }

    void on_order_filled(const OrderFilled& e) {
        auto it = orders_.find(e.resting_id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(
            ::exchange::ice::fix::encode_exec_fill(e, order, ctx_));

        it->second.filled_quantity += e.quantity;
    }

    void on_order_cancelled(const OrderCancelled& e) {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(
            ::exchange::ice::fix::encode_exec_cancel(e, order, ctx_));

        orders_.erase(it);
    }

    void on_order_modified(const OrderModified& e) {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(
            ::exchange::ice::fix::encode_exec_replace(e, order, ctx_));

        it->second.price = e.new_price;
        it->second.quantity = e.new_qty;
    }

    // --- Accessors ---

    const std::vector<std::string>& messages() const { return messages_; }
    void clear_messages() { messages_.clear(); }

private:
    struct OrderInfo {
        OrderId id{0};
        uint64_t client_order_id{0};
        Price price{0};
        Quantity quantity{0};
        Quantity filled_quantity{0};
        Side side{Side::Buy};
    };

    static Order to_order(const OrderInfo& info) {
        Order o;
        o.id = info.id;
        o.client_order_id = info.client_order_id;
        o.price = info.price;
        o.quantity = info.quantity;
        o.filled_quantity = info.filled_quantity;
        o.side = info.side;
        return o;
    }

    ::exchange::ice::fix::EncodeContext ctx_;
    std::unordered_map<uint64_t, OrderInfo> pending_;   // keyed by client_order_id
    std::unordered_map<OrderId, OrderInfo> orders_;     // keyed by engine-assigned OrderId
    std::vector<std::string> messages_;
};

}  // namespace exchange::krx::fix
