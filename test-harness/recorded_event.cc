#include "test-harness/recorded_event.h"

#include <string>

namespace exchange {

// --- Equality operators ---

bool operator==(const OrderAccepted& a, const OrderAccepted& b) {
    return a.id == b.id &&
           a.client_order_id == b.client_order_id &&
           a.ts == b.ts;
}

bool operator==(const OrderRejected& a, const OrderRejected& b) {
    return a.client_order_id == b.client_order_id &&
           a.ts == b.ts &&
           a.reason == b.reason;
}

bool operator==(const OrderFilled& a, const OrderFilled& b) {
    return a.aggressor_id == b.aggressor_id &&
           a.resting_id == b.resting_id &&
           a.price == b.price &&
           a.quantity == b.quantity &&
           a.ts == b.ts;
}

bool operator==(const OrderPartiallyFilled& a, const OrderPartiallyFilled& b) {
    return a.aggressor_id == b.aggressor_id &&
           a.resting_id == b.resting_id &&
           a.price == b.price &&
           a.quantity == b.quantity &&
           a.aggressor_remaining == b.aggressor_remaining &&
           a.resting_remaining == b.resting_remaining &&
           a.ts == b.ts;
}

bool operator==(const OrderCancelled& a, const OrderCancelled& b) {
    return a.id == b.id &&
           a.ts == b.ts &&
           a.reason == b.reason;
}

bool operator==(const OrderCancelRejected& a, const OrderCancelRejected& b) {
    return a.id == b.id &&
           a.client_order_id == b.client_order_id &&
           a.ts == b.ts &&
           a.reason == b.reason;
}

bool operator==(const OrderModified& a, const OrderModified& b) {
    return a.id == b.id &&
           a.client_order_id == b.client_order_id &&
           a.new_price == b.new_price &&
           a.new_qty == b.new_qty &&
           a.ts == b.ts;
}

bool operator==(const OrderModifyRejected& a, const OrderModifyRejected& b) {
    return a.id == b.id &&
           a.client_order_id == b.client_order_id &&
           a.ts == b.ts &&
           a.reason == b.reason;
}

bool operator==(const TopOfBook& a, const TopOfBook& b) {
    return a.best_bid == b.best_bid &&
           a.bid_qty == b.bid_qty &&
           a.best_ask == b.best_ask &&
           a.ask_qty == b.ask_qty &&
           a.ts == b.ts;
}

bool operator==(const DepthUpdate& a, const DepthUpdate& b) {
    return a.side == b.side &&
           a.price == b.price &&
           a.total_qty == b.total_qty &&
           a.order_count == b.order_count &&
           a.action == b.action &&
           a.ts == b.ts;
}

bool operator==(const OrderBookAction& a, const OrderBookAction& b) {
    return a.id == b.id &&
           a.side == b.side &&
           a.price == b.price &&
           a.qty == b.qty &&
           a.action == b.action &&
           a.ts == b.ts;
}

bool operator==(const Trade& a, const Trade& b) {
    return a.price == b.price &&
           a.quantity == b.quantity &&
           a.aggressor_id == b.aggressor_id &&
           a.resting_id == b.resting_id &&
           a.aggressor_side == b.aggressor_side &&
           a.ts == b.ts;
}

// --- to_string helpers ---

namespace {

const char* side_str(Side s) {
    return s == Side::Buy ? "Buy" : "Sell";
}

const char* depth_action_str(DepthUpdate::Action a) {
    switch (a) {
        case DepthUpdate::Add:    return "Add";
        case DepthUpdate::Update: return "Update";
        case DepthUpdate::Remove: return "Remove";
    }
    return "Unknown";
}

const char* book_action_str(OrderBookAction::Action a) {
    switch (a) {
        case OrderBookAction::Add:    return "Add";
        case OrderBookAction::Modify: return "Modify";
        case OrderBookAction::Cancel: return "Cancel";
        case OrderBookAction::Fill:   return "Fill";
    }
    return "Unknown";
}

const char* reject_reason_str(RejectReason r) {
    switch (r) {
        case RejectReason::PoolExhausted:       return "PoolExhausted";
        case RejectReason::InvalidPrice:        return "InvalidPrice";
        case RejectReason::InvalidQuantity:     return "InvalidQuantity";
        case RejectReason::InvalidTif:          return "InvalidTif";
        case RejectReason::InvalidSide:         return "InvalidSide";
        case RejectReason::UnknownOrder:        return "UnknownOrder";
        case RejectReason::PriceBandViolation:  return "PriceBandViolation";
        case RejectReason::LevelPoolExhausted:  return "LevelPoolExhausted";
        case RejectReason::ExchangeSpecific:    return "ExchangeSpecific";
    }
    return "Unknown";
}

const char* cancel_reason_str(CancelReason r) {
    switch (r) {
        case CancelReason::UserRequested:       return "UserRequested";
        case CancelReason::IOCRemainder:        return "IOCRemainder";
        case CancelReason::FOKFailed:           return "FOKFailed";
        case CancelReason::Expired:             return "Expired";
        case CancelReason::SelfMatchPrevention: return "SelfMatchPrevention";
        case CancelReason::LevelPoolExhausted:  return "LevelPoolExhausted";
    }
    return "Unknown";
}

}  // namespace

// --- to_string implementation ---

std::string to_string(const RecordedEvent& event) {
    return std::visit([](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, OrderAccepted>) {
            return "OrderAccepted{id=" + std::to_string(e.id) +
                   ", cl_ord_id=" + std::to_string(e.client_order_id) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderRejected>) {
            return "OrderRejected{cl_ord_id=" + std::to_string(e.client_order_id) +
                   ", reason=" + reject_reason_str(e.reason) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderFilled>) {
            return "OrderFilled{aggressor=" + std::to_string(e.aggressor_id) +
                   ", resting=" + std::to_string(e.resting_id) +
                   ", price=" + std::to_string(e.price) +
                   ", qty=" + std::to_string(e.quantity) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderPartiallyFilled>) {
            return "OrderPartiallyFilled{aggressor=" + std::to_string(e.aggressor_id) +
                   ", resting=" + std::to_string(e.resting_id) +
                   ", price=" + std::to_string(e.price) +
                   ", qty=" + std::to_string(e.quantity) +
                   ", aggressor_rem=" + std::to_string(e.aggressor_remaining) +
                   ", resting_rem=" + std::to_string(e.resting_remaining) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderCancelled>) {
            return "OrderCancelled{id=" + std::to_string(e.id) +
                   ", reason=" + cancel_reason_str(e.reason) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderCancelRejected>) {
            return "OrderCancelRejected{id=" + std::to_string(e.id) +
                   ", cl_ord_id=" + std::to_string(e.client_order_id) +
                   ", reason=" + reject_reason_str(e.reason) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderModified>) {
            return "OrderModified{id=" + std::to_string(e.id) +
                   ", cl_ord_id=" + std::to_string(e.client_order_id) +
                   ", new_price=" + std::to_string(e.new_price) +
                   ", new_qty=" + std::to_string(e.new_qty) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderModifyRejected>) {
            return "OrderModifyRejected{id=" + std::to_string(e.id) +
                   ", cl_ord_id=" + std::to_string(e.client_order_id) +
                   ", reason=" + reject_reason_str(e.reason) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, TopOfBook>) {
            return "TopOfBook{bid=" + std::to_string(e.best_bid) +
                   ", bid_qty=" + std::to_string(e.bid_qty) +
                   ", ask=" + std::to_string(e.best_ask) +
                   ", ask_qty=" + std::to_string(e.ask_qty) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, DepthUpdate>) {
            return "DepthUpdate{side=" + std::string(side_str(e.side)) +
                   ", price=" + std::to_string(e.price) +
                   ", qty=" + std::to_string(e.total_qty) +
                   ", count=" + std::to_string(e.order_count) +
                   ", action=" + depth_action_str(e.action) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, OrderBookAction>) {
            return "OrderBookAction{id=" + std::to_string(e.id) +
                   ", side=" + std::string(side_str(e.side)) +
                   ", price=" + std::to_string(e.price) +
                   ", qty=" + std::to_string(e.qty) +
                   ", action=" + book_action_str(e.action) +
                   ", ts=" + std::to_string(e.ts) + "}";
        } else if constexpr (std::is_same_v<T, Trade>) {
            return "Trade{aggressor=" + std::to_string(e.aggressor_id) +
                   ", resting=" + std::to_string(e.resting_id) +
                   ", price=" + std::to_string(e.price) +
                   ", qty=" + std::to_string(e.quantity) +
                   ", side=" + std::string(side_str(e.aggressor_side)) +
                   ", ts=" + std::to_string(e.ts) + "}";
        }
    }, event);
}

}  // namespace exchange
