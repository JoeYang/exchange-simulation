#pragma once

#include "test-harness/recorded_event.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace exchange {

struct PriceLevelView {
    Price    price;
    Quantity total_qty;
    uint32_t order_count;
};

struct TradeView {
    Price     price;
    Quantity  quantity;
    OrderId   aggressor_id;
    OrderId   resting_id;
    Timestamp ts;
};

struct EventLogEntry {
    std::string description;  // human-readable
    Timestamp   ts;
};

class OrderbookState {
    // bids: descending (highest price first)
    std::map<Price, PriceLevelView, std::greater<Price>> bids_;
    // asks: ascending (lowest price first)
    std::map<Price, PriceLevelView> asks_;

    std::vector<TradeView>    recent_trades_;  // last kMaxTrades trades
    std::vector<EventLogEntry> event_log_;     // last kMaxEvents events

    static constexpr size_t kMaxTrades = 20;
    static constexpr size_t kMaxEvents = 50;

    void add_event(const std::string& description, Timestamp ts);

public:
    // Apply a single event to update state.
    void apply(const RecordedEvent& event);

    // Accessors
    const auto& bids() const { return bids_; }
    const auto& asks() const { return asks_; }
    const std::vector<TradeView>&     recent_trades() const { return recent_trades_; }
    const std::vector<EventLogEntry>& event_log()     const { return event_log_; }

    // Returns best bid price, or 0 if the bid side is empty.
    Price best_bid() const;
    // Returns best ask price, or 0 if the ask side is empty.
    Price best_ask() const;

    // Clear all state.
    void reset();
};

}  // namespace exchange
