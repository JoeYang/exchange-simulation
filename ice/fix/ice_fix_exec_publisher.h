#pragma once

#include "exchange-core/listeners.h"
#include "exchange-core/types.h"
#include "ice/fix/fix_encoder.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::ice::fix {

// ---------------------------------------------------------------------------
// IceFixExecPublisher — OrderListenerBase that encodes engine events as
// FIX 4.2 ExecutionReport messages and stores them for retrieval.
//
// The publisher maintains a lightweight order info cache so it can produce
// FIX messages with full order context (price, qty, side). Orders must be
// registered via register_order() before the engine processes them.
//
// Off-hot-path component: uses std::string, std::vector, std::unordered_map.
// ---------------------------------------------------------------------------

class IceFixExecPublisher : public OrderListenerBase {
public:
    IceFixExecPublisher(std::string sender, std::string target, std::string symbol)
    {
        ctx_.sender_comp_id = std::move(sender);
        ctx_.target_comp_id = std::move(target);
        ctx_.symbol = std::move(symbol);
    }

    // Register order info before submitting to the engine.
    // Must be called so that fill/cancel/modify callbacks have context.
    void register_order(OrderId id, uint64_t client_order_id,
                        Price price, Quantity qty, Side side)
    {
        OrderInfo info;
        info.id = id;
        info.client_order_id = client_order_id;
        info.price = price;
        info.quantity = qty;
        info.filled_quantity = 0;
        info.side = side;
        orders_[id] = info;
    }

    // --- OrderListenerBase callbacks (name hiding, not virtual) ---

    void on_order_accepted(const OrderAccepted& e) {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(encode_exec_new(e, order, ctx_));
    }

    void on_order_rejected(const OrderRejected& e) {
        messages_.push_back(encode_exec_reject(e, ctx_));
    }

    void on_order_filled(const OrderFilled& e) {
        // Fills reference the resting order
        auto it = orders_.find(e.resting_id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(encode_exec_fill(e, order, ctx_));

        // Update tracked filled quantity
        it->second.filled_quantity += e.quantity;
    }

    void on_order_cancelled(const OrderCancelled& e) {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(encode_exec_cancel(e, order, ctx_));

        orders_.erase(it);
    }

    void on_order_modified(const OrderModified& e) {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) return;

        Order order = to_order(it->second);
        messages_.push_back(encode_exec_replace(e, order, ctx_));

        // Update cached order state
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

    EncodeContext ctx_;
    std::unordered_map<OrderId, OrderInfo> orders_;
    std::vector<std::string> messages_;
};

}  // namespace exchange::ice::fix
