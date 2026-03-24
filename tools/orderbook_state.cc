#include "tools/orderbook_state.h"

#include <string>
#include <variant>

namespace exchange {

namespace {

// Helper: human-readable side label
const char* side_str(Side s) {
    return s == Side::Buy ? "Buy" : "Sell";
}

// Helper: human-readable OrderBookAction::Action label
const char* book_action_str(OrderBookAction::Action a) {
    switch (a) {
        case OrderBookAction::Add:    return "Add";
        case OrderBookAction::Modify: return "Modify";
        case OrderBookAction::Cancel: return "Cancel";
        case OrderBookAction::Fill:   return "Fill";
    }
    return "Unknown";
}

// Helper: human-readable RejectReason label
const char* reject_reason_str(RejectReason r) {
    switch (r) {
        case RejectReason::PoolExhausted:      return "PoolExhausted";
        case RejectReason::InvalidPrice:       return "InvalidPrice";
        case RejectReason::InvalidQuantity:    return "InvalidQuantity";
        case RejectReason::InvalidTif:         return "InvalidTif";
        case RejectReason::InvalidSide:        return "InvalidSide";
        case RejectReason::UnknownOrder:       return "UnknownOrder";
        case RejectReason::PriceBandViolation: return "PriceBandViolation";
        case RejectReason::LevelPoolExhausted: return "LevelPoolExhausted";
        case RejectReason::ExchangeSpecific:   return "ExchangeSpecific";
    }
    return "Unknown";
}

// Helper: human-readable CancelReason label
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

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void OrderbookState::add_event(const std::string& description, Timestamp ts) {
    if (event_log_.size() >= kMaxEvents) {
        event_log_.erase(event_log_.begin());
    }
    event_log_.push_back({description, ts});
}

// ---------------------------------------------------------------------------
// apply()
// ---------------------------------------------------------------------------

void OrderbookState::apply(const RecordedEvent& event) {
    std::visit([this](const auto& e) {
        using T = std::decay_t<decltype(e)>;

        // --- DepthUpdate: primary L2 book state ---
        if constexpr (std::is_same_v<T, DepthUpdate>) {
            // bids_ and asks_ have different comparator types, so each side is
            // handled explicitly rather than through a common reference.
            switch (e.action) {
                case DepthUpdate::Add:
                case DepthUpdate::Update:
                    // Both Add and Update set the level to the supplied values.
                    // The engine always sends the complete new state for the level.
                    if (e.side == Side::Buy) {
                        bids_[e.price] = {e.price, e.total_qty, e.order_count};
                    } else {
                        asks_[e.price] = {e.price, e.total_qty, e.order_count};
                    }
                    break;
                case DepthUpdate::Remove:
                    if (e.side == Side::Buy) {
                        bids_.erase(e.price);
                    } else {
                        asks_.erase(e.price);
                    }
                    break;
            }

        // --- Trade: append to recent_trades_ ---
        } else if constexpr (std::is_same_v<T, Trade>) {
            if (recent_trades_.size() >= kMaxTrades) {
                recent_trades_.erase(recent_trades_.begin());
            }
            recent_trades_.push_back({e.price, e.quantity, e.aggressor_id, e.resting_id, e.ts});

        // --- OrderBookAction: append to event_log_ ---
        } else if constexpr (std::is_same_v<T, OrderBookAction>) {
            std::string desc =
                std::string("OrderBookAction{id=") + std::to_string(e.id) +
                ", side=" + side_str(e.side) +
                ", price=" + std::to_string(e.price) +
                ", qty=" + std::to_string(e.qty) +
                ", action=" + book_action_str(e.action) + "}";
            add_event(desc, e.ts);

        // --- TopOfBook: informational, log it ---
        } else if constexpr (std::is_same_v<T, TopOfBook>) {
            std::string desc =
                std::string("TopOfBook{bid=") + std::to_string(e.best_bid) +
                ", bid_qty=" + std::to_string(e.bid_qty) +
                ", ask=" + std::to_string(e.best_ask) +
                ", ask_qty=" + std::to_string(e.ask_qty) + "}";
            add_event(desc, e.ts);

        // --- Order events: append to event_log_ ---
        } else if constexpr (std::is_same_v<T, OrderAccepted>) {
            std::string desc =
                std::string("OrderAccepted{id=") + std::to_string(e.id) +
                ", cl_ord_id=" + std::to_string(e.client_order_id) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderRejected>) {
            std::string desc =
                std::string("OrderRejected{cl_ord_id=") + std::to_string(e.client_order_id) +
                ", reason=" + reject_reason_str(e.reason) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderFilled>) {
            std::string desc =
                std::string("OrderFilled{aggressor=") + std::to_string(e.aggressor_id) +
                ", resting=" + std::to_string(e.resting_id) +
                ", price=" + std::to_string(e.price) +
                ", qty=" + std::to_string(e.quantity) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderPartiallyFilled>) {
            std::string desc =
                std::string("OrderPartiallyFilled{aggressor=") + std::to_string(e.aggressor_id) +
                ", resting=" + std::to_string(e.resting_id) +
                ", price=" + std::to_string(e.price) +
                ", qty=" + std::to_string(e.quantity) +
                ", agg_rem=" + std::to_string(e.aggressor_remaining) +
                ", rest_rem=" + std::to_string(e.resting_remaining) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderCancelled>) {
            std::string desc =
                std::string("OrderCancelled{id=") + std::to_string(e.id) +
                ", reason=" + cancel_reason_str(e.reason) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderCancelRejected>) {
            std::string desc =
                std::string("OrderCancelRejected{id=") + std::to_string(e.id) +
                ", cl_ord_id=" + std::to_string(e.client_order_id) +
                ", reason=" + reject_reason_str(e.reason) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderModified>) {
            std::string desc =
                std::string("OrderModified{id=") + std::to_string(e.id) +
                ", new_price=" + std::to_string(e.new_price) +
                ", new_qty=" + std::to_string(e.new_qty) + "}";
            add_event(desc, e.ts);

        } else if constexpr (std::is_same_v<T, OrderModifyRejected>) {
            std::string desc =
                std::string("OrderModifyRejected{id=") + std::to_string(e.id) +
                ", cl_ord_id=" + std::to_string(e.client_order_id) +
                ", reason=" + reject_reason_str(e.reason) + "}";
            add_event(desc, e.ts);
        }
    }, event);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Price OrderbookState::best_bid() const {
    if (bids_.empty()) return 0;
    return bids_.begin()->first;
}

Price OrderbookState::best_ask() const {
    if (asks_.empty()) return 0;
    return asks_.begin()->first;
}

void OrderbookState::reset() {
    bids_.clear();
    asks_.clear();
    recent_trades_.clear();
    event_log_.clear();
}

}  // namespace exchange
