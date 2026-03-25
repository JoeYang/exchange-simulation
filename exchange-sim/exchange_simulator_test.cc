#include "exchange-sim/exchange_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Minimal CRTP exchange for testing -- uses all MatchingEngine defaults.
class TestEngine
    : public MatchingEngine<TestEngine, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<TestEngine, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
};

using TestSimulator =
    ExchangeSimulator<TestEngine, RecordingOrderListener, RecordingMdListener>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

InstrumentConfig make_instrument(InstrumentId id, const std::string& symbol,
                                 Price tick = 100, Quantity lot = 10000) {
    return InstrumentConfig{
        .id = id,
        .symbol = symbol,
        .engine_config = EngineConfig{.tick_size = tick,
                                      .lot_size = lot,
                                      .price_band_low = 0,
                                      .price_band_high = 0}};
}

OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                        Quantity qty, Timestamp ts) {
    return OrderRequest{.client_order_id = cl_ord_id,
                        .account_id = 1,
                        .side = side,
                        .type = OrderType::Limit,
                        .tif = TimeInForce::GTC,
                        .price = price,
                        .quantity = qty,
                        .stop_price = 0,
                        .timestamp = ts,
                        .gtd_expiry = 0};
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ExchangeSimulatorTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestSimulator sim_{order_listener_, md_listener_};

    void SetUp() override {
        // Start in Continuous for order matching by default.
        sim_.set_session_state(SessionState::Continuous, 0);
    }
};

// ---------------------------------------------------------------------------
// Instrument management
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, AddThreeInstrumentsAndVerifyCount) {
    // SetUp already transitions state; add 3 instruments.
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));
    sim_.add_instrument(make_instrument(3, "CLK6"));

    EXPECT_EQ(sim_.instrument_count(), 3u);
}

TEST_F(ExchangeSimulatorTest, GetEngineReturnsNullForUnknown) {
    EXPECT_EQ(sim_.get_engine(999), nullptr);
}

TEST_F(ExchangeSimulatorTest, GetConfigReturnsNullForUnknown) {
    EXPECT_EQ(sim_.get_config(999), nullptr);
}

TEST_F(ExchangeSimulatorTest, GetOhlcvReturnsNullForUnknown) {
    EXPECT_EQ(sim_.get_ohlcv(999), nullptr);
}

TEST_F(ExchangeSimulatorTest, GetEngineReturnsValidPointer) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    EXPECT_NE(sim_.get_engine(1), nullptr);
}

TEST_F(ExchangeSimulatorTest, GetConfigReturnsCorrectSymbol) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    const auto* cfg = sim_.get_config(1);
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->symbol, "ESH6");
}

TEST_F(ExchangeSimulatorTest, RemoveInstrument) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    EXPECT_EQ(sim_.instrument_count(), 1u);
    sim_.remove_instrument(1);
    EXPECT_EQ(sim_.instrument_count(), 0u);
    EXPECT_EQ(sim_.get_engine(1), nullptr);
}

// ---------------------------------------------------------------------------
// Failure: duplicate instrument
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, DuplicateInstrumentThrows) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    EXPECT_THROW(sim_.add_instrument(make_instrument(1, "ESH6")),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// Failure: remove unknown instrument
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, RemoveUnknownInstrumentThrows) {
    EXPECT_THROW(sim_.remove_instrument(999), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Order routing to correct instrument
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, RouteOrdersToCorrectInstrument) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));

    // Place a buy on instrument 1 and a sell on instrument 2.
    sim_.new_order(1, make_limit(100, Side::Buy, 10000, 10000, 1));
    sim_.new_order(2, make_limit(200, Side::Sell, 20000, 10000, 2));

    // Each engine should have exactly 1 active order.
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 1u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 1u);
}

TEST_F(ExchangeSimulatorTest, OrdersDoNotCrossInstruments) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));

    // Buy at 10000 on instrument 1, sell at 10000 on instrument 2 -- no match.
    sim_.new_order(1, make_limit(100, Side::Buy, 10000, 10000, 1));
    sim_.new_order(2, make_limit(200, Side::Sell, 10000, 10000, 2));

    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 1u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 1u);
}

// ---------------------------------------------------------------------------
// Failure: unknown instrument order routing
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, NewOrderUnknownInstrumentThrows) {
    EXPECT_THROW(
        sim_.new_order(999, make_limit(1, Side::Buy, 10000, 10000, 1)),
        std::runtime_error);
}

TEST_F(ExchangeSimulatorTest, CancelOrderUnknownInstrumentThrows) {
    EXPECT_THROW(sim_.cancel_order(999, 1, 1), std::runtime_error);
}

TEST_F(ExchangeSimulatorTest, ModifyOrderUnknownInstrumentThrows) {
    ModifyRequest req{.order_id = 1,
                      .client_order_id = 1,
                      .new_price = 10000,
                      .new_quantity = 10000,
                      .timestamp = 1};
    EXPECT_THROW(sim_.modify_order(999, req), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Exchange-wide session state transition
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, SessionStateTransitionAffectsAllInstruments) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));

    sim_.set_session_state(SessionState::PreOpen, 100);

    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);
    EXPECT_EQ(sim_.get_engine(1)->session_state(), SessionState::PreOpen);
    EXPECT_EQ(sim_.get_engine(2)->session_state(), SessionState::PreOpen);
}

TEST_F(ExchangeSimulatorTest, NewInstrumentInheritsCurrentState) {
    sim_.set_session_state(SessionState::PreOpen, 100);
    sim_.add_instrument(make_instrument(1, "ESH6"));

    EXPECT_EQ(sim_.get_engine(1)->session_state(), SessionState::PreOpen);
}

// ---------------------------------------------------------------------------
// Execute all auctions
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, ExecuteAllAuctionsUncrossesBooks) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));

    // Transition to PreOpen for order collection.
    sim_.set_session_state(SessionState::PreOpen, 0);

    // Place crossing orders on instrument 1: buy at 10100, sell at 10000.
    sim_.new_order(1, make_limit(100, Side::Buy, 10100, 10000, 1));
    sim_.new_order(1, make_limit(101, Side::Sell, 10000, 10000, 2));

    // Place crossing orders on instrument 2: buy at 20200, sell at 20000.
    sim_.new_order(2, make_limit(200, Side::Buy, 20200, 10000, 3));
    sim_.new_order(2, make_limit(201, Side::Sell, 20000, 10000, 4));

    // Both instruments should have 2 resting orders (no matching in PreOpen).
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 2u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 2u);

    // Execute auctions on all instruments.
    sim_.execute_all_auctions(10);

    // Both books should be uncrossed -- orders fully filled.
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 0u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Mass cancel all
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, MassCancelAllCancelsAllInstruments) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));
    sim_.add_instrument(make_instrument(3, "CLK6"));

    // Place one order per instrument.
    sim_.new_order(1, make_limit(100, Side::Buy, 10000, 10000, 1));
    sim_.new_order(2, make_limit(200, Side::Buy, 20000, 10000, 2));
    sim_.new_order(3, make_limit(300, Side::Buy, 30000, 10000, 3));

    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 1u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 1u);
    EXPECT_EQ(sim_.get_engine(3)->active_order_count(), 1u);

    sim_.mass_cancel_all(100);

    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 0u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 0u);
    EXPECT_EQ(sim_.get_engine(3)->active_order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Trigger expiry across all instruments
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, TriggerExpiryAcrossAllInstruments) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));

    // Place DAY orders on both instruments.
    OrderRequest day_order = make_limit(100, Side::Buy, 10000, 10000, 1);
    day_order.tif = TimeInForce::DAY;
    sim_.new_order(1, day_order);

    day_order.client_order_id = 200;
    sim_.new_order(2, day_order);

    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 1u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 1u);

    sim_.trigger_expiry(1000, TimeInForce::DAY);

    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 0u);
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 0u);
}

// ---------------------------------------------------------------------------
// OHLCV tracking per instrument
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, OhlcvTrackingPerInstrument) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));

    // Manually update OHLCV for each instrument.
    OhlcvStats* ohlcv1 = sim_.get_ohlcv(1);
    OhlcvStats* ohlcv2 = sim_.get_ohlcv(2);
    ASSERT_NE(ohlcv1, nullptr);
    ASSERT_NE(ohlcv2, nullptr);

    ohlcv1->on_trade(10000, 10000);
    ohlcv1->on_trade(10200, 20000);
    ohlcv1->on_trade(9800, 10000);

    ohlcv2->on_trade(50000, 5000);

    // Instrument 1: open=10000, high=10200, low=9800, close=9800
    EXPECT_EQ(ohlcv1->open, 10000);
    EXPECT_EQ(ohlcv1->high, 10200);
    EXPECT_EQ(ohlcv1->low, 9800);
    EXPECT_EQ(ohlcv1->close, 9800);
    EXPECT_EQ(ohlcv1->volume, 40000);
    EXPECT_EQ(ohlcv1->trade_count, 3u);

    // Instrument 2: single trade
    EXPECT_EQ(ohlcv2->open, 50000);
    EXPECT_EQ(ohlcv2->high, 50000);
    EXPECT_EQ(ohlcv2->low, 50000);
    EXPECT_EQ(ohlcv2->close, 50000);
    EXPECT_EQ(ohlcv2->volume, 5000);
    EXPECT_EQ(ohlcv2->trade_count, 1u);
}

TEST_F(ExchangeSimulatorTest, OhlcvResetOnPreOpen) {
    sim_.add_instrument(make_instrument(1, "ESH6"));

    OhlcvStats* ohlcv = sim_.get_ohlcv(1);
    ohlcv->on_trade(10000, 10000);
    EXPECT_EQ(ohlcv->trade_count, 1u);

    // Transition to PreOpen resets OHLCV.
    sim_.set_session_state(SessionState::PreOpen, 100);

    EXPECT_EQ(ohlcv->trade_count, 0u);
    EXPECT_EQ(ohlcv->volume, 0);
    EXPECT_FALSE(ohlcv->has_traded);
}

TEST_F(ExchangeSimulatorTest, OhlcvResetOnClosed) {
    sim_.add_instrument(make_instrument(1, "ESH6"));

    OhlcvStats* ohlcv = sim_.get_ohlcv(1);
    ohlcv->on_trade(10000, 10000);
    EXPECT_EQ(ohlcv->trade_count, 1u);

    sim_.set_session_state(SessionState::Closed, 200);

    EXPECT_EQ(ohlcv->trade_count, 0u);
    EXPECT_EQ(ohlcv->volume, 0);
    EXPECT_FALSE(ohlcv->has_traded);
}

// ---------------------------------------------------------------------------
// for_each_instrument iteration
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, ForEachInstrumentVisitsAll) {
    sim_.add_instrument(make_instrument(1, "ESH6"));
    sim_.add_instrument(make_instrument(2, "NQM6"));
    sim_.add_instrument(make_instrument(3, "CLK6"));

    size_t count = 0;
    sim_.for_each_instrument(
        [&](InstrumentId, const InstrumentConfig&, TestEngine&, OhlcvStats&) {
            ++count;
        });
    EXPECT_EQ(count, 3u);
}

// ---------------------------------------------------------------------------
// Initial session state is Closed
// ---------------------------------------------------------------------------

TEST_F(ExchangeSimulatorTest, InitialSessionStateBeforeSetUp) {
    // Create a fresh simulator without the fixture SetUp.
    RecordingOrderListener ol;
    RecordingMdListener ml;
    TestSimulator fresh_sim(ol, ml);

    EXPECT_EQ(fresh_sim.session_state(), SessionState::Closed);
}

}  // namespace
}  // namespace exchange
