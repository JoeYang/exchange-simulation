#pragma once

#include "exchange-core/types.h"

#include <array>
#include <optional>

namespace exchange {

// TradeRecord captures the essential details of an executed trade.
// Stored in the TradeRegistry for later bust/adjustment operations.
struct TradeRecord {
    TradeId   trade_id{0};
    OrderId   aggressor_id{0};
    OrderId   resting_id{0};
    Price     price{0};
    Quantity  quantity{0};
    Timestamp ts{0};
    Side      aggressor_side{Side::Buy};
    uint64_t  aggressor_account_id{0};
    uint64_t  resting_account_id{0};
    bool      busted{false};  // set to true after a successful bust
};

// TradeRegistry is a fixed-capacity, index-based store of executed trades.
// Trade IDs are sequential starting at 1. Lookup is O(1) by trade ID.
//
// Template parameter MaxTrades controls the maximum number of trades that
// can be recorded. This is a compile-time constant to avoid dynamic
// allocation on the hot path.
template <size_t MaxTrades = 100000>
class TradeRegistry {
public:
    // Record a new trade. Returns the assigned TradeId, or 0 if capacity
    // is exhausted.
    TradeId record(OrderId aggressor_id, OrderId resting_id,
                   Price price, Quantity quantity, Timestamp ts,
                   Side aggressor_side = Side::Buy,
                   uint64_t aggressor_account_id = 0,
                   uint64_t resting_account_id = 0) {
        if (next_trade_id_ >= MaxTrades) return 0;

        TradeId id = next_trade_id_++;
        trades_[id] = TradeRecord{
            .trade_id              = id,
            .aggressor_id          = aggressor_id,
            .resting_id            = resting_id,
            .price                 = price,
            .quantity              = quantity,
            .ts                    = ts,
            .aggressor_side        = aggressor_side,
            .aggressor_account_id  = aggressor_account_id,
            .resting_account_id    = resting_account_id,
            .busted                = false,
        };
        return id;
    }

    // Look up a trade by ID. Returns nullopt if the ID is out of range
    // or no trade has been recorded at that ID.
    std::optional<TradeRecord> lookup(TradeId id) const {
        if (id == 0 || id >= next_trade_id_) return std::nullopt;
        return trades_[id];
    }

    // Mark a trade as busted. Returns false if the trade doesn't exist
    // or was already busted.
    bool mark_busted(TradeId id) {
        if (id == 0 || id >= next_trade_id_) return false;
        if (trades_[id].busted) return false;
        trades_[id].busted = true;
        return true;
    }

    // Returns the number of trades recorded so far.
    size_t trade_count() const { return next_trade_id_ - 1; }

    // Returns the next trade ID that will be assigned.
    TradeId next_trade_id() const { return next_trade_id_; }

private:
    TradeId next_trade_id_{1};
    std::array<TradeRecord, MaxTrades> trades_{};
};

}  // namespace exchange
