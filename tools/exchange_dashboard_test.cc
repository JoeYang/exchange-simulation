// exchange_dashboard_test.cc -- unit tests for the dashboard state logic.
//
// Tests the DashboardState class (throughput tracking, event routing, OHLCV
// accumulation) without starting an interactive FTXUI session or connecting
// to shared memory.
//
// The rendering functions are exercised indirectly through the binary;
// this test focuses on the data model correctness.

#include "exchange-core/events.h"
#include "exchange-core/ohlcv.h"
#include "exchange-core/types.h"
#include "test-harness/recorded_event.h"
#include "tools/orderbook_state.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <variant>

namespace exchange {

// ---------------------------------------------------------------------------
// Minimal DashboardState replica for testing (mirrors exchange_dashboard.cc).
// The production struct lives in the binary's anonymous namespace; we duplicate
// the data model here to validate the logic independently.
// ---------------------------------------------------------------------------

struct TestInstrumentView {
    std::string symbol;
    SessionState state{SessionState::Closed};
    OhlcvStats ohlcv;
    OrderbookState book;
};

struct TestDashboardState {
    std::vector<TestInstrumentView> instruments;
    int selected_instrument{0};

    uint64_t total_messages{0};
    uint64_t messages_this_sec{0};
    uint64_t messages_per_sec{0};
    uint64_t peak_msg_per_sec{0};
    std::chrono::steady_clock::time_point last_sec_tick;

    TestDashboardState() : last_sec_tick(std::chrono::steady_clock::now()) {
        TestInstrumentView v;
        v.symbol = "ES";
        instruments.push_back(std::move(v));
    }

    void tick_throughput() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_sec_tick);
        if (elapsed.count() >= 1) {
            messages_per_sec = messages_this_sec;
            if (messages_per_sec > peak_msg_per_sec) {
                peak_msg_per_sec = messages_per_sec;
            }
            messages_this_sec = 0;
            last_sec_tick = now;
        }
    }

    void on_event(const RecordedEvent& event) {
        ++total_messages;
        ++messages_this_sec;

        if (instruments.empty()) return;
        auto& inst = instruments[0];
        inst.book.apply(event);

        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, Trade>) {
                inst.ohlcv.on_trade(e.price, e.quantity);
            } else if constexpr (std::is_same_v<T, MarketStatus>) {
                inst.state = e.state;
            }
        }, event);
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ExchangeDashboardTest, InitialState) {
    TestDashboardState ds;
    ASSERT_EQ(ds.instruments.size(), 1u);
    EXPECT_EQ(ds.instruments[0].symbol, "ES");
    EXPECT_EQ(ds.instruments[0].state, SessionState::Closed);
    EXPECT_FALSE(ds.instruments[0].ohlcv.has_traded);
    EXPECT_EQ(ds.total_messages, 0u);
    EXPECT_EQ(ds.messages_per_sec, 0u);
}

TEST(ExchangeDashboardTest, TradeUpdatesOhlcv) {
    TestDashboardState ds;

    Trade t1;
    t1.price = 50000000;
    t1.quantity = 10000;
    t1.aggressor_id = 2;
    t1.resting_id = 1;
    t1.aggressor_side = Side::Sell;
    t1.ts = 1000;
    ds.on_event(RecordedEvent{t1});

    auto& ohlcv = ds.instruments[0].ohlcv;
    EXPECT_TRUE(ohlcv.has_traded);
    EXPECT_EQ(ohlcv.open, 50000000);
    EXPECT_EQ(ohlcv.high, 50000000);
    EXPECT_EQ(ohlcv.low, 50000000);
    EXPECT_EQ(ohlcv.close, 50000000);
    EXPECT_EQ(ohlcv.volume, 10000);
    EXPECT_EQ(ohlcv.trade_count, 1u);

    // Second trade at higher price.
    Trade t2;
    t2.price = 50050000;
    t2.quantity = 20000;
    t2.aggressor_id = 4;
    t2.resting_id = 3;
    t2.aggressor_side = Side::Buy;
    t2.ts = 2000;
    ds.on_event(RecordedEvent{t2});

    EXPECT_EQ(ohlcv.high, 50050000);
    EXPECT_EQ(ohlcv.low, 50000000);
    EXPECT_EQ(ohlcv.close, 50050000);
    EXPECT_EQ(ohlcv.volume, 30000);
    EXPECT_EQ(ohlcv.trade_count, 2u);
}

TEST(ExchangeDashboardTest, MarketStatusUpdatesSessionState) {
    TestDashboardState ds;
    EXPECT_EQ(ds.instruments[0].state, SessionState::Closed);

    MarketStatus ms;
    ms.state = SessionState::Continuous;
    ms.ts = 1000;
    ds.on_event(RecordedEvent{ms});

    EXPECT_EQ(ds.instruments[0].state, SessionState::Continuous);
}

TEST(ExchangeDashboardTest, MessageCounterIncrements) {
    TestDashboardState ds;
    EXPECT_EQ(ds.total_messages, 0u);

    OrderAccepted oa;
    oa.id = 1;
    oa.client_order_id = 1;
    oa.ts = 100;
    ds.on_event(RecordedEvent{oa});
    EXPECT_EQ(ds.total_messages, 1u);
    EXPECT_EQ(ds.messages_this_sec, 1u);

    ds.on_event(RecordedEvent{oa});
    ds.on_event(RecordedEvent{oa});
    EXPECT_EQ(ds.total_messages, 3u);
    EXPECT_EQ(ds.messages_this_sec, 3u);
}

TEST(ExchangeDashboardTest, InstrumentSelection) {
    TestDashboardState ds;
    // Add a second instrument.
    TestInstrumentView v2;
    v2.symbol = "NQ";
    ds.instruments.push_back(std::move(v2));

    EXPECT_EQ(ds.selected_instrument, 0);

    // Navigate down.
    int n = static_cast<int>(ds.instruments.size());
    ds.selected_instrument = (ds.selected_instrument + 1) % n;
    EXPECT_EQ(ds.selected_instrument, 1);
    EXPECT_EQ(ds.instruments[ds.selected_instrument].symbol, "NQ");

    // Navigate down wraps.
    ds.selected_instrument = (ds.selected_instrument + 1) % n;
    EXPECT_EQ(ds.selected_instrument, 0);

    // Navigate up wraps.
    ds.selected_instrument = (ds.selected_instrument - 1 + n) % n;
    EXPECT_EQ(ds.selected_instrument, 1);
}

TEST(ExchangeDashboardTest, DepthUpdateAppliedToBook) {
    TestDashboardState ds;

    DepthUpdate du;
    du.side = Side::Buy;
    du.price = 50000000;
    du.total_qty = 10000;
    du.order_count = 1;
    du.action = DepthUpdate::Add;
    du.ts = 1000;
    ds.on_event(RecordedEvent{du});

    const auto& bids = ds.instruments[0].book.bids();
    ASSERT_EQ(bids.size(), 1u);
    auto it = bids.begin();
    EXPECT_EQ(it->first, 50000000);
    EXPECT_EQ(it->second.total_qty, 10000);
    EXPECT_EQ(it->second.order_count, 1u);
}

TEST(ExchangeDashboardTest, TradeAppearsInRecentTrades) {
    TestDashboardState ds;

    Trade t;
    t.price = 50000000;
    t.quantity = 10000;
    t.aggressor_id = 2;
    t.resting_id = 1;
    t.aggressor_side = Side::Sell;
    t.ts = 1000;
    ds.on_event(RecordedEvent{t});

    const auto& trades = ds.instruments[0].book.recent_trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 50000000);
    EXPECT_EQ(trades[0].quantity, 10000);
}

TEST(ExchangeDashboardTest, OhlcvLowTracksMinPrice) {
    TestDashboardState ds;

    auto send_trade = [&](Price p, Quantity q, Timestamp ts) {
        Trade t;
        t.price = p;
        t.quantity = q;
        t.aggressor_id = 2;
        t.resting_id = 1;
        t.aggressor_side = Side::Sell;
        t.ts = ts;
        ds.on_event(RecordedEvent{t});
    };

    send_trade(50000000, 10000, 1000);
    send_trade(49500000, 10000, 2000);
    send_trade(50100000, 10000, 3000);

    auto& ohlcv = ds.instruments[0].ohlcv;
    EXPECT_EQ(ohlcv.open, 50000000);
    EXPECT_EQ(ohlcv.high, 50100000);
    EXPECT_EQ(ohlcv.low, 49500000);
    EXPECT_EQ(ohlcv.close, 50100000);
    EXPECT_EQ(ohlcv.trade_count, 3u);
}

}  // namespace exchange
