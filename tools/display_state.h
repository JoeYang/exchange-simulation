#pragma once

#include <algorithm>
#include <cstdint>

// ---------------------------------------------------------------------------
// Display state: 5-level book, last 10 trades, OHLCV, message counters.
//
// Shared between exchange_observer.cc and recovery strategies so that
// recovery can populate the book before the observer enters its event loop.
// ---------------------------------------------------------------------------

static constexpr int BOOK_DEPTH  = 5;
static constexpr int TRADE_DEPTH = 10;

struct BookLevel {
    int64_t  price{0};
    int32_t  qty{0};
    int32_t  order_count{0};
};

struct TradeEntry {
    int64_t  price{0};
    int32_t  qty{0};
    uint8_t  aggressor_side{0}; // 1=Buy, 2=Sell
    uint64_t timestamp_ns{0};
};

struct DisplayState {
    BookLevel bids[BOOK_DEPTH]{};
    BookLevel asks[BOOK_DEPTH]{};
    int       bid_levels{0};
    int       ask_levels{0};

    TradeEntry trades[TRADE_DEPTH]{};
    int        trade_count{0};
    int        trade_write_idx{0}; // circular buffer index

    // OHLCV
    int64_t  open_price{0};
    int64_t  high_price{0};
    int64_t  low_price{0};
    int64_t  close_price{0};
    int64_t  volume{0};

    // Counters
    uint64_t total_messages{0};
    uint64_t messages_this_second{0};
    uint64_t msgs_per_sec{0};      // last completed second
    uint64_t decode_errors{0};
    uint64_t total_trades{0};

    // Sequence tracking
    uint32_t last_seq{0};
    uint64_t seq_gaps{0};
};

// Add a trade to the circular buffer and update OHLCV.
inline void record_trade(DisplayState& ds, int64_t price, int32_t qty,
                          uint8_t aggressor, uint64_t ts) {
    auto& t = ds.trades[ds.trade_write_idx];
    t.price = price;
    t.qty = qty;
    t.aggressor_side = aggressor;
    t.timestamp_ns = ts;
    ds.trade_write_idx = (ds.trade_write_idx + 1) % TRADE_DEPTH;
    if (ds.trade_count < TRADE_DEPTH) ++ds.trade_count;
    ++ds.total_trades;

    // OHLCV
    if (ds.open_price == 0) ds.open_price = price;
    if (price > ds.high_price || ds.high_price == 0) ds.high_price = price;
    if (price < ds.low_price || ds.low_price == 0) ds.low_price = price;
    ds.close_price = price;
    ds.volume += qty;
}

// Update a book side from a single entry. Simple model: treat each level as
// a price-level slot. For New/Change, insert or update; for Delete, remove.
// This is a simplified book that maintains up to BOOK_DEPTH sorted levels.
inline void update_book_side(BookLevel* levels, int& count,
                              int64_t price, int32_t qty, int32_t orders,
                              bool is_delete, bool is_bid) {
    if (is_delete) {
        // Remove the level matching this price.
        for (int i = 0; i < count; ++i) {
            if (levels[i].price == price) {
                for (int j = i; j < count - 1; ++j)
                    levels[j] = levels[j + 1];
                --count;
                levels[count] = BookLevel{};
                return;
            }
        }
        return;
    }

    // Check if the price already exists -- update in place.
    for (int i = 0; i < count; ++i) {
        if (levels[i].price == price) {
            levels[i].qty = qty;
            levels[i].order_count = orders;
            return;
        }
    }

    // Insert new level, maintaining sort order.
    // Bids: descending price. Asks: ascending price.
    BookLevel entry{price, qty, orders};
    if (count < BOOK_DEPTH) {
        levels[count] = entry;
        ++count;
    } else {
        // Replace worst level if this price is better.
        int worst = count - 1;
        bool better = is_bid ? (price > levels[worst].price)
                             : (price < levels[worst].price);
        if (!better) return;
        levels[worst] = entry;
    }

    // Sort: bids descending, asks ascending.
    if (is_bid) {
        std::sort(levels, levels + count,
                  [](const BookLevel& a, const BookLevel& b) {
                      return a.price > b.price;
                  });
    } else {
        std::sort(levels, levels + count,
                  [](const BookLevel& a, const BookLevel& b) {
                      return a.price < b.price;
                  });
    }
}
