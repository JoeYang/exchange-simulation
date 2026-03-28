#include "krx/krx_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace krx {
namespace {

using TestKrxSimulator =
    KrxSimulator<RecordingOrderListener, RecordingMdListener>;

// ---------------------------------------------------------------------------
// Fixture: single KS (KOSPI200) instrument through the full simulator.
// ---------------------------------------------------------------------------

class KrxIntegrationTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestKrxSimulator sim_{order_listener_, md_listener_};

    static constexpr uint32_t KS_ID  = 1;  // KOSPI200 Futures
    static constexpr uint32_t KTB_ID = 9;  // KTB 3Y Futures

    // Reference price for KOSPI200 (350.0000 = 3500000 fixed-point).
    static constexpr Price KS_REF = 3500000;

    void SetUp() override {
        sim_.load_products(get_krx_products());
        sim_.set_prices(KS_ID, KS_REF, KS_REF);
        sim_.set_prices(KTB_ID, 1050000, 1050000);  // KTB ref 105.0000
    }

    OrderRequest make_limit(uint64_t cl_id, uint64_t account_id, Side side,
                            Price price, Quantity qty,
                            TimeInForce tif = TimeInForce::GTC,
                            Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id, .account_id = account_id,
            .side = side, .type = OrderType::Limit, .tif = tif,
            .price = price, .quantity = qty, .stop_price = 0,
            .timestamp = ts, .gtd_expiry = 0,
        };
    }

    template <typename T>
    size_t count_order_events() const {
        size_t n = 0;
        for (const auto& ev : order_listener_.events())
            if (std::holds_alternative<T>(ev)) ++n;
        return n;
    }

    template <typename T>
    size_t count_md_events() const {
        size_t n = 0;
        for (const auto& ev : md_listener_.events())
            if (std::holds_alternative<T>(ev)) ++n;
        return n;
    }

    bool has_reject(RejectReason reason) const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderRejected>(ev) &&
                std::get<OrderRejected>(ev).reason == reason)
                return true;
        }
        return false;
    }

    bool has_session(SessionState state) const {
        for (const auto& ev : md_listener_.events()) {
            if (std::holds_alternative<MarketStatus>(ev) &&
                std::get<MarketStatus>(ev).state == state)
                return true;
        }
        return false;
    }

    // Open regular session and establish an initial trade at ref price.
    void open_and_establish_trade(Timestamp ts = 1000) {
        sim_.start_regular_session(ts);
        sim_.open_regular_market(ts + 100);

        // Establish trade at reference price.
        sim_.new_order(KS_ID, make_limit(900, 1, Side::Sell, KS_REF, 10000,
                                         TimeInForce::GTC, ts + 200));
        sim_.new_order(KS_ID, make_limit(901, 2, Side::Buy, KS_REF, 10000,
                                         TimeInForce::GTC, ts + 201));
        order_listener_.clear();
        md_listener_.clear();
    }
};

// ===========================================================================
// Scenario 1: Sidecar rejects programme orders, allows non-programme
// ===========================================================================

TEST_F(KrxIntegrationTest, SidecarRejectProgrammeAcceptNonProgramme) {
    open_and_establish_trade();

    sim_.activate_sidecar();

    // Programme order (account >= 10000) — rejected.
    sim_.new_order(KS_ID, make_limit(1, 10000, Side::Buy, 3500000, 10000,
                                     TimeInForce::GTC, 2000));
    EXPECT_TRUE(has_reject(RejectReason::ExchangeSpecific))
        << "Programme order rejected during sidecar";

    order_listener_.clear();

    // Non-programme order (account < 10000) — accepted.
    sim_.new_order(KS_ID, make_limit(2, 5000, Side::Buy, 3500000, 10000,
                                     TimeInForce::GTC, 2001));
    EXPECT_EQ(count_order_events<OrderAccepted>(), 1u)
        << "Non-programme order accepted during sidecar";

    // Lift sidecar — programme orders accepted again.
    sim_.deactivate_sidecar();
    order_listener_.clear();

    sim_.new_order(KS_ID, make_limit(3, 10000, Side::Buy, 3500000, 10000,
                                     TimeInForce::GTC, 2002));
    EXPECT_EQ(count_order_events<OrderAccepted>(), 1u)
        << "Programme order accepted after sidecar lifted";
}

// ===========================================================================
// Scenario 2: Tiered daily limit — tier 1 breach widens to tier 2
// ===========================================================================

TEST_F(KrxIntegrationTest, TieredLimitWidenOnBreach) {
    // Disable dynamic band for this test (limits are at 8%, bands at 5%).
    sim_.get_regular_engine(KS_ID)->set_band_percentage(0);

    sim_.start_regular_session(1000);
    sim_.open_regular_market(1100);

    // Tier 1 upper limit: 3500000 * 1.08 = 3780000.
    // Trade at the upper limit.
    sim_.new_order(KS_ID, make_limit(1, 1, Side::Sell, 3780000, 10000,
                                     TimeInForce::GTC, 2000));
    sim_.new_order(KS_ID, make_limit(2, 2, Side::Buy, 3780000, 10000,
                                     TimeInForce::GTC, 2001));

    auto* engine = sim_.get_regular_engine(KS_ID);
    EXPECT_EQ(engine->current_limit_tier(), 2)
        << "Hitting tier 1 limit must widen to tier 2";
    EXPECT_EQ(engine->session_state(), SessionState::Continuous)
        << "After widening, must resume Continuous";

    // Tier 2 upper limit: 3500000 * 1.15 = 4025000.
    // An order at 4020000 (within tier 2, > tier 1) should be accepted.
    order_listener_.clear();
    sim_.new_order(KS_ID, make_limit(3, 3, Side::Buy, 4020000, 10000,
                                     TimeInForce::GTC, 2100));
    EXPECT_EQ(count_order_events<OrderAccepted>(), 1u)
        << "Order within tier 2 limits must be accepted";
}

// ===========================================================================
// Scenario 3: Tiered limits escalate through all 3 tiers
// ===========================================================================

TEST_F(KrxIntegrationTest, TieredLimitEscalateAllTiers) {
    sim_.get_regular_engine(KS_ID)->set_band_percentage(0);

    sim_.start_regular_session(1000);
    sim_.open_regular_market(1100);

    auto* engine = sim_.get_regular_engine(KS_ID);

    // Tier 1 → 2 (hit 8% = 3780000).
    sim_.new_order(KS_ID, make_limit(1, 1, Side::Sell, 3780000, 10000,
                                     TimeInForce::GTC, 2000));
    sim_.new_order(KS_ID, make_limit(2, 2, Side::Buy, 3780000, 10000,
                                     TimeInForce::GTC, 2001));
    EXPECT_EQ(engine->current_limit_tier(), 2);

    // Tier 2 → 3 (hit 15% = 4025000).
    sim_.new_order(KS_ID, make_limit(3, 3, Side::Sell, 4025000, 10000,
                                     TimeInForce::GTC, 2100));
    sim_.new_order(KS_ID, make_limit(4, 4, Side::Buy, 4025000, 10000,
                                     TimeInForce::GTC, 2101));
    EXPECT_EQ(engine->current_limit_tier(), 3);

    // Tier 3 is max (20% = 4200000). Hit it — no further widening.
    sim_.new_order(KS_ID, make_limit(5, 5, Side::Sell, 4200000, 10000,
                                     TimeInForce::GTC, 2200));
    sim_.new_order(KS_ID, make_limit(6, 6, Side::Buy, 4200000, 10000,
                                     TimeInForce::GTC, 2201));
    EXPECT_EQ(engine->current_limit_tier(), 3) << "Must stay at tier 3 (max)";
    EXPECT_EQ(engine->session_state(), SessionState::LockLimit)
        << "At max tier, must remain LockLimit";
}

// ===========================================================================
// Scenario 4: VI trigger — dynamic band breach triggers VolatilityAuction
// ===========================================================================

TEST_F(KrxIntegrationTest, ViDynamicBandTrigger) {
    sim_.start_regular_session(1000);

    // Place orders in PreOpen and execute auction to set VI reference.
    sim_.new_order(KS_ID, make_limit(100, 1, Side::Sell, KS_REF, 10000,
                                     TimeInForce::GTC, 1050));
    sim_.new_order(KS_ID, make_limit(101, 2, Side::Buy, KS_REF, 10000,
                                     TimeInForce::GTC, 1051));
    sim_.open_regular_market(1100);
    order_listener_.clear();
    md_listener_.clear();

    // VI dynamic threshold: 3% of 3500000 = 105000.
    // Order band: 5% of 3500000 = 175000 → max price 3675000.
    // Trade at 3606000 (deviation = 106000 > 105000) → triggers VI.
    sim_.new_order(KS_ID, make_limit(1, 3, Side::Sell, 3606000, 10000,
                                     TimeInForce::GTC, 2000));
    sim_.new_order(KS_ID, make_limit(2, 4, Side::Buy, 3606000, 10000,
                                     TimeInForce::GTC, 2001));

    EXPECT_TRUE(has_session(SessionState::VolatilityAuction))
        << "Trade breaching 3% dynamic band must trigger VolatilityAuction";
}

// ===========================================================================
// Scenario 5: VI trigger + resume to Continuous
// ===========================================================================

TEST_F(KrxIntegrationTest, ViTriggerAndResume) {
    sim_.start_regular_session(1000);
    sim_.new_order(KS_ID, make_limit(100, 1, Side::Sell, KS_REF, 10000,
                                     TimeInForce::GTC, 1050));
    sim_.new_order(KS_ID, make_limit(101, 2, Side::Buy, KS_REF, 10000,
                                     TimeInForce::GTC, 1051));
    sim_.open_regular_market(1100);
    order_listener_.clear();
    md_listener_.clear();

    // Trigger VI.
    sim_.new_order(KS_ID, make_limit(1, 3, Side::Sell, 3606000, 10000,
                                     TimeInForce::GTC, 2000));
    sim_.new_order(KS_ID, make_limit(2, 4, Side::Buy, 3606000, 10000,
                                     TimeInForce::GTC, 2001));
    ASSERT_TRUE(has_session(SessionState::VolatilityAuction));

    // Resume to Continuous after VI period (simulated by direct transition).
    sim_.set_session_state(SessionState::Continuous, 3000);
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);

    // Verify normal trading resumes (order accepted).
    order_listener_.clear();
    md_listener_.clear();
    sim_.new_order(KS_ID, make_limit(10, 5, Side::Buy, 3606000, 10000,
                                     TimeInForce::GTC, 3100));
    EXPECT_EQ(count_order_events<OrderAccepted>(), 1u)
        << "Orders must be accepted after VI resume";
}

// ===========================================================================
// Scenario 6: Dual session — regular orders isolated from after-hours
// ===========================================================================

TEST_F(KrxIntegrationTest, DualSessionIsolation) {
    sim_.start_regular_session(1000);
    sim_.open_regular_market(1100);

    // Place resting order in regular session.
    sim_.new_order(KS_ID, make_limit(1, 1, Side::Buy, 3500000, 10000,
                                     TimeInForce::GTC, 2000));
    EXPECT_EQ(sim_.get_regular_engine(KS_ID)->active_order_count(), 1u);

    sim_.pre_close_regular(3000);
    sim_.close_regular_session(3100);

    // After-hours session.
    sim_.start_after_hours(4000);

    // Order in after-hours is on a separate book.
    sim_.new_order(KS_ID, make_limit(2, 2, Side::Sell, 3500000, 10000,
                                     TimeInForce::GTC, 4100));
    EXPECT_EQ(sim_.get_after_hours_engine(KS_ID)->active_order_count(), 1u);

    // Regular engine still has its order (not matched against after-hours).
    EXPECT_EQ(sim_.get_regular_engine(KS_ID)->active_order_count(), 1u)
        << "Regular session order must be isolated from after-hours";
}

// ===========================================================================
// Scenario 7: Multi-product isolation — KS fill does not affect KTB
// ===========================================================================

TEST_F(KrxIntegrationTest, MultiProductIsolation) {
    sim_.start_regular_session(1000);
    sim_.open_regular_market(1100);

    // Resting on KTB.
    sim_.new_order(KTB_ID, make_limit(100, 1, Side::Sell, 1050000, 10000,
                                      TimeInForce::GTC, 2000));

    // Cross on KS.
    sim_.new_order(KS_ID, make_limit(200, 1, Side::Sell, 3500000, 10000,
                                     TimeInForce::GTC, 2001));
    sim_.new_order(KS_ID, make_limit(201, 2, Side::Buy, 3500000, 10000,
                                     TimeInForce::GTC, 2002));

    EXPECT_EQ(sim_.get_regular_engine(KS_ID)->active_order_count(), 0u)
        << "KS orders should have filled";
    EXPECT_EQ(sim_.get_regular_engine(KTB_ID)->active_order_count(), 1u)
        << "KTB order must be unaffected by KS fill";
}

// ===========================================================================
// Scenario 8: Full KRX trading day lifecycle
// ===========================================================================

TEST_F(KrxIntegrationTest, FullTradingDayLifecycle) {
    // 1. PreOpen.
    sim_.start_regular_session(1000);
    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);

    // Crossing orders collected, no matching.
    sim_.new_order(KS_ID, make_limit(1, 1, Side::Buy, 3500500, 10000,
                                     TimeInForce::GTC, 1100));
    sim_.new_order(KS_ID, make_limit(2, 2, Side::Sell, 3500000, 10000,
                                     TimeInForce::GTC, 1101));
    EXPECT_EQ(sim_.get_regular_engine(KS_ID)->active_order_count(), 2u);
    EXPECT_EQ(count_md_events<Trade>(), 0u) << "No trades in PreOpen";

    // 2. Opening auction → Continuous.
    order_listener_.clear();
    md_listener_.clear();
    sim_.open_regular_market(2000);
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);
    EXPECT_EQ(sim_.get_regular_engine(KS_ID)->active_order_count(), 0u)
        << "Opening auction must uncross";

    // 3. Continuous trading.
    order_listener_.clear();
    md_listener_.clear();
    sim_.new_order(KS_ID, make_limit(10, 10, Side::Sell, 3500000, 20000,
                                     TimeInForce::GTC, 3000));
    sim_.new_order(KS_ID, make_limit(11, 11, Side::Buy, 3500000, 10000,
                                     TimeInForce::GTC, 3001));
    EXPECT_GE(count_md_events<Trade>(), 1u) << "Continuous must produce trades";

    // 4. PreClose.
    sim_.pre_close_regular(4000);
    EXPECT_EQ(sim_.session_state(), SessionState::PreClose);

    // 5. Closing auction → Closed.
    sim_.close_regular_session(4100);
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);

    // 6. After-hours.
    sim_.start_after_hours(5000);
    EXPECT_TRUE(sim_.is_after_hours());
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);

    sim_.close_after_hours(6000);
    EXPECT_FALSE(sim_.is_after_hours());
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);

    // 7. End of day.
    sim_.end_of_day(7000);
}

// ===========================================================================
// Scenario 9: Phase validation — IOC rejected in PreOpen and PreClose
// ===========================================================================

TEST_F(KrxIntegrationTest, PhaseValidationIocRejected) {
    sim_.start_regular_session(1000);

    // IOC in PreOpen → rejected.
    sim_.new_order(KS_ID, make_limit(1, 1, Side::Buy, 3500000, 10000,
                                     TimeInForce::IOC, 1100));
    EXPECT_TRUE(has_reject(RejectReason::InvalidTif))
        << "IOC must be rejected in PreOpen";

    sim_.open_regular_market(2000);
    sim_.pre_close_regular(3000);

    order_listener_.clear();

    // IOC in PreClose → rejected.
    sim_.new_order(KS_ID, make_limit(2, 1, Side::Buy, 3500000, 10000,
                                     TimeInForce::IOC, 3100));
    EXPECT_TRUE(has_reject(RejectReason::InvalidTif))
        << "IOC must be rejected in PreClose";
}

// ===========================================================================
// Scenario 10: Daily limit boundary — exact limit price accepted
// ===========================================================================

TEST_F(KrxIntegrationTest, DailyLimitBoundaryAccepted) {
    sim_.get_regular_engine(KS_ID)->set_band_percentage(0);

    sim_.start_regular_session(1000);
    sim_.open_regular_market(1100);

    // Tier 1 upper limit: 3500000 + 8% = 3780000.
    // Order at exactly the limit — must be accepted.
    sim_.new_order(KS_ID, make_limit(1, 1, Side::Buy, 3780000, 10000,
                                     TimeInForce::GTC, 2000));
    EXPECT_EQ(count_order_events<OrderAccepted>(), 1u)
        << "Order at exact tier 1 upper limit must be accepted";

    // Order above the limit — rejected.
    order_listener_.clear();
    sim_.new_order(KS_ID, make_limit(2, 2, Side::Buy, 3780500, 10000,
                                     TimeInForce::GTC, 2001));
    EXPECT_TRUE(has_reject(RejectReason::LockLimitUp))
        << "Order above tier 1 upper limit must be rejected";
}

}  // namespace
}  // namespace krx
}  // namespace exchange
