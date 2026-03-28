#include "krx/krx_exchange.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace krx {
namespace {

// Small pool sizes for fast tests.
using TestKrxExchange = KrxExchange<
    RecordingOrderListener,
    RecordingMdListener,
    FifoMatch,
    /*MaxOrders=*/     200,
    /*MaxPriceLevels=*/100,
    /*MaxOrderIds=*/   2000>;

class KrxExchangeTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener    md_listener_;

    // KOSPI200-like config: tick=0.05 (500), lot=1 (10000), no static bands.
    // Daily limits and VI are configured per-test via set_reference_price / etc.
    EngineConfig config_{
        .tick_size       = 500,
        .lot_size        = 10000,
        .price_band_low  = 0,
        .price_band_high = 0,
        .max_order_size  = 0,
    };

    TestKrxExchange engine_{config_, order_listener_, md_listener_};

    // --- Helpers ---

    OrderRequest make_limit(uint64_t cl_id, uint64_t account_id,
                            Side side, Price price, Quantity qty,
                            TimeInForce tif = TimeInForce::GTC,
                            Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id,
            .account_id      = account_id,
            .side            = side,
            .type            = OrderType::Limit,
            .tif             = tif,
            .price           = price,
            .quantity         = qty,
            .stop_price      = 0,
            .timestamp       = ts,
            .gtd_expiry      = 0,
        };
    }

    OrderRequest make_ioc(uint64_t cl_id, uint64_t account_id,
                          Side side, Price price, Quantity qty,
                          Timestamp ts = 1000) {
        return make_limit(cl_id, account_id, side, price, qty,
                          TimeInForce::IOC, ts);
    }

    OrderRequest make_fok(uint64_t cl_id, uint64_t account_id,
                          Side side, Price price, Quantity qty,
                          Timestamp ts = 1000) {
        return make_limit(cl_id, account_id, side, price, qty,
                          TimeInForce::FOK, ts);
    }

    OrderRequest make_market(uint64_t cl_id, uint64_t account_id,
                             Side side, Quantity qty,
                             Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id,
            .account_id      = account_id,
            .side            = side,
            .type            = OrderType::Market,
            .tif             = TimeInForce::GTC,
            .price           = 0,
            .quantity         = qty,
            .stop_price      = 0,
            .timestamp       = ts,
            .gtd_expiry      = 0,
        };
    }

    // Helpers to check for specific events in the recording listeners.

    bool has_reject(RejectReason reason) const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderRejected>(ev)) {
                if (std::get<OrderRejected>(ev).reason == reason)
                    return true;
            }
        }
        return false;
    }

    bool has_any_reject() const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderRejected>(ev)) return true;
        }
        return false;
    }

    bool has_accept() const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderAccepted>(ev)) return true;
        }
        return false;
    }

    bool has_trade() const {
        for (const auto& ev : md_listener_.events()) {
            if (std::holds_alternative<Trade>(ev)) return true;
        }
        return false;
    }

    bool has_smp_cancel() const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderCancelled>(ev)) {
                if (std::get<OrderCancelled>(ev).reason ==
                    CancelReason::SelfMatchPrevention)
                    return true;
            }
        }
        return false;
    }

    bool has_market_status(SessionState state) const {
        for (const auto& ev : md_listener_.events()) {
            if (std::holds_alternative<MarketStatus>(ev)) {
                if (std::get<MarketStatus>(ev).state == state)
                    return true;
            }
        }
        return false;
    }

    // Create a trade at a given price to establish reference.
    // Uses different accounts to avoid SMP.
    void establish_trade(Price price, Timestamp ts = 1000) {
        engine_.new_order(make_limit(900, 1, Side::Sell, price, 10000,
                                     TimeInForce::GTC, ts));
        engine_.new_order(make_limit(901, 2, Side::Buy, price, 10000,
                                     TimeInForce::GTC, ts));
        order_listener_.clear();
        md_listener_.clear();
    }
};

// ===========================================================================
// Basic order acceptance
// ===========================================================================

TEST_F(KrxExchangeTest, LimitOrderAcceptedInContinuous) {
    engine_.new_order(make_limit(1, 42, Side::Buy, 3500000, 10000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(has_accept());
}

TEST_F(KrxExchangeTest, CrossingLimitOrdersProduceTrade) {
    engine_.new_order(make_limit(1, 1, Side::Sell, 3500000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    engine_.new_order(make_limit(2, 2, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_trade());
}

// ===========================================================================
// Self-Match Prevention (SMP) — account-based, CancelNewest
// ===========================================================================

TEST_F(KrxExchangeTest, SmpCancelNewest_SameAccountNoCross) {
    engine_.new_order(make_limit(1, 99, Side::Sell, 3500000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    engine_.new_order(make_limit(2, 99, Side::Buy, 3500000, 10000));

    EXPECT_FALSE(has_trade()) << "SMP should prevent trade";
    EXPECT_TRUE(has_smp_cancel()) << "Aggressor must be SMP-cancelled";
}

TEST_F(KrxExchangeTest, SmpNotTriggered_DifferentAccounts) {
    engine_.new_order(make_limit(1, 1, Side::Sell, 3500000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    engine_.new_order(make_limit(2, 2, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_trade()) << "Different accounts must trade";
}

// ===========================================================================
// Phase validation — PreOpen, PreClose, VolatilityAuction
// ===========================================================================

TEST_F(KrxExchangeTest, IocRejectedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_ioc(10, 1, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_any_reject()) << "IOC must be rejected in PreOpen";
}

TEST_F(KrxExchangeTest, FokRejectedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_fok(11, 1, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_any_reject()) << "FOK must be rejected in PreOpen";
}

TEST_F(KrxExchangeTest, MarketOrderRejectedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_market(12, 1, Side::Buy, 10000));
    EXPECT_TRUE(has_any_reject()) << "Market must be rejected in PreOpen";
}

TEST_F(KrxExchangeTest, LimitGtcAcceptedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_limit(13, 1, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_accept()) << "GTC limit must be accepted in PreOpen";
}

TEST_F(KrxExchangeTest, IocRejectedDuringPreClose) {
    engine_.set_session_state(SessionState::PreClose, 3000);
    order_listener_.clear();

    engine_.new_order(make_ioc(14, 1, Side::Sell, 3500000, 10000));
    EXPECT_TRUE(has_any_reject()) << "IOC must be rejected in PreClose";
}

TEST_F(KrxExchangeTest, MarketRejectedDuringPreClose) {
    engine_.set_session_state(SessionState::PreClose, 3000);
    order_listener_.clear();

    engine_.new_order(make_market(15, 1, Side::Buy, 10000));
    EXPECT_TRUE(has_any_reject()) << "Market must be rejected in PreClose";
}

TEST_F(KrxExchangeTest, IocRejectedDuringVolatilityAuction) {
    engine_.set_session_state(SessionState::VolatilityAuction, 4000);
    order_listener_.clear();

    engine_.new_order(make_ioc(16, 1, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_any_reject())
        << "IOC must be rejected during VolatilityAuction";
}

TEST_F(KrxExchangeTest, FokRejectedDuringVolatilityAuction) {
    engine_.set_session_state(SessionState::VolatilityAuction, 4000);
    order_listener_.clear();

    engine_.new_order(make_fok(17, 1, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_any_reject())
        << "FOK must be rejected during VolatilityAuction";
}

TEST_F(KrxExchangeTest, LimitAcceptedDuringVolatilityAuction) {
    engine_.set_session_state(SessionState::VolatilityAuction, 4000);
    order_listener_.clear();

    engine_.new_order(make_limit(18, 1, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_accept())
        << "GTC limit must be accepted during VolatilityAuction";
}

TEST_F(KrxExchangeTest, IocAcceptedDuringContinuous) {
    engine_.new_order(make_limit(20, 2, Side::Sell, 3500000, 10000));
    order_listener_.clear();

    engine_.new_order(make_ioc(21, 1, Side::Buy, 3500000, 10000));
    EXPECT_FALSE(has_any_reject()) << "IOC must be accepted in Continuous";
}

// ===========================================================================
// Dynamic price bands (±3% default)
// ===========================================================================

TEST_F(KrxExchangeTest, OrderOutsideDynamicBandRejected) {
    // Establish reference trade at 350.0000 (3500000).
    establish_trade(3500000);

    // ±5% of 3500000 = ±175000 → band = [3325000, 3675000]
    // 3000000 is far below.
    engine_.new_order(make_limit(3, 3, Side::Buy, 3000000, 10000));
    EXPECT_TRUE(has_reject(RejectReason::PriceBandViolation))
        << "Order below dynamic band must be rejected";
}

TEST_F(KrxExchangeTest, OrderAtBandBoundaryAccepted) {
    establish_trade(3500000);

    // Lower bound = 3500000 - 175000 = 3325000. Check tick alignment:
    // 3325000 % 500 == 0 ✓
    engine_.new_order(make_limit(3, 3, Side::Buy, 3325000, 10000));
    EXPECT_FALSE(has_any_reject()) << "Order at exact band boundary accepted";
}

TEST_F(KrxExchangeTest, OrderAboveDynamicBandRejected) {
    establish_trade(3500000);

    // Upper bound = 3500000 + 175000 = 3675000.
    // 3700000 is above.
    engine_.new_order(make_limit(3, 3, Side::Buy, 3700000, 10000));
    EXPECT_TRUE(has_reject(RejectReason::PriceBandViolation))
        << "Order above dynamic band must be rejected";
}

TEST_F(KrxExchangeTest, NoBandCheckBeforeFirstTrade) {
    engine_.new_order(make_limit(1, 1, Side::Buy, 500, 10000));
    EXPECT_TRUE(has_accept()) << "No reference → no band check";
}

TEST_F(KrxExchangeTest, BandDisabledWhenPctIsZero) {
    engine_.set_band_percentage(0);
    establish_trade(3500000);

    engine_.new_order(make_limit(3, 3, Side::Buy, 500, 10000));
    EXPECT_FALSE(has_reject(RejectReason::PriceBandViolation))
        << "band_pct=0 disables price band validation";
}

// ===========================================================================
// Sidecar (programme trading halt)
// ===========================================================================

TEST_F(KrxExchangeTest, SidecarRejectsProgrammeOrders) {
    engine_.activate_sidecar();

    // Programme order: account_id >= 10000
    engine_.new_order(make_limit(1, 10000, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_reject(RejectReason::ExchangeSpecific))
        << "Programme order must be rejected during sidecar";
}

TEST_F(KrxExchangeTest, SidecarAllowsNonProgrammeOrders) {
    engine_.activate_sidecar();

    // Non-programme order: account_id < 10000
    engine_.new_order(make_limit(1, 9999, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_accept())
        << "Non-programme order must be accepted during sidecar";
}

TEST_F(KrxExchangeTest, SidecarLiftAllowsProgrammeOrders) {
    engine_.activate_sidecar();
    engine_.deactivate_sidecar();

    engine_.new_order(make_limit(1, 10000, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_accept())
        << "Programme order accepted after sidecar lifted";
}

TEST_F(KrxExchangeTest, SidecarBoundaryAccountId) {
    engine_.activate_sidecar();

    // account_id == 9999 → NOT programme
    engine_.new_order(make_limit(1, 9999, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_accept()) << "account 9999 is not programme";

    order_listener_.clear();

    // account_id == 10000 → programme
    engine_.new_order(make_limit(2, 10000, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_reject(RejectReason::ExchangeSpecific))
        << "account 10000 is programme";
}

TEST_F(KrxExchangeTest, SidecarInactiveByDefault) {
    EXPECT_FALSE(engine_.sidecar_active());

    engine_.new_order(make_limit(1, 50000, Side::Buy, 3500000, 10000));
    EXPECT_TRUE(has_accept())
        << "Programme orders accepted when sidecar is inactive";
}

// ===========================================================================
// VI (Volatility Interruption)
// ===========================================================================

TEST_F(KrxExchangeTest, ViTriggeredByDynamicBandBreach) {
    // Set prior close so static band is wide enough to not trigger.
    engine_.set_prior_close_price(3500000);

    // Establish reference at 350.0000 and enter Continuous to set
    // vi_reference_price_.
    engine_.set_session_state(SessionState::PreOpen, 500);
    engine_.new_order(make_limit(100, 1, Side::Sell, 3500000, 10000));
    engine_.new_order(make_limit(101, 2, Side::Buy, 3500000, 10000));
    // Execute auction at reference price to set vi_reference_price_.
    engine_.execute_auction(3500000, 600);
    // Now transition to Continuous — vi_reference set to last_trade.
    engine_.set_session_state(SessionState::Continuous, 700);
    order_listener_.clear();
    md_listener_.clear();

    // 3% of 3500000 = 105000. Trade at 3606000 (deviation=106000 > 105000).
    engine_.new_order(make_limit(1, 3, Side::Sell, 3606000, 10000));
    engine_.new_order(make_limit(2, 4, Side::Buy, 3606000, 10000));

    EXPECT_TRUE(has_market_status(SessionState::VolatilityAuction))
        << "Trade breaching 3% dynamic band must trigger VI";
}

TEST_F(KrxExchangeTest, ViNotTriggeredWithinDynamicBand) {
    engine_.set_prior_close_price(3500000);

    engine_.set_session_state(SessionState::PreOpen, 500);
    engine_.new_order(make_limit(100, 1, Side::Sell, 3500000, 10000));
    engine_.new_order(make_limit(101, 2, Side::Buy, 3500000, 10000));
    engine_.execute_auction(3500000, 600);
    engine_.set_session_state(SessionState::Continuous, 700);
    order_listener_.clear();
    md_listener_.clear();

    // Trade at 3600000 (deviation = 100000 < 105000 threshold).
    engine_.new_order(make_limit(1, 3, Side::Sell, 3600000, 10000));
    engine_.new_order(make_limit(2, 4, Side::Buy, 3600000, 10000));

    EXPECT_FALSE(has_market_status(SessionState::VolatilityAuction))
        << "Trade within 3% band must NOT trigger VI";
}

TEST_F(KrxExchangeTest, ViTriggeredByStaticBandBreach) {
    // Prior close at 350.0000. Static band = 10% = 350000.
    // So static limit = [3150000, 3850000].
    engine_.set_prior_close_price(3500000);
    // Widen dynamic band so it does not trigger first.
    engine_.set_band_percentage(50);

    engine_.set_session_state(SessionState::PreOpen, 500);
    engine_.new_order(make_limit(100, 1, Side::Sell, 3500000, 10000));
    engine_.new_order(make_limit(101, 2, Side::Buy, 3500000, 10000));
    engine_.execute_auction(3500000, 600);
    engine_.set_session_state(SessionState::Continuous, 700);
    order_listener_.clear();
    md_listener_.clear();

    // Trade at 3860000 (deviation = 360000 > 350000 static threshold).
    engine_.new_order(make_limit(1, 3, Side::Sell, 3860000, 10000));
    engine_.new_order(make_limit(2, 4, Side::Buy, 3860000, 10000));

    EXPECT_TRUE(has_market_status(SessionState::VolatilityAuction))
        << "Trade breaching 10% static band must trigger VI";
}

// ===========================================================================
// Tiered daily price limits (±8% → ±15% → ±20%)
// ===========================================================================

TEST_F(KrxExchangeTest, TieredLimits_InitializesAtTier1) {
    // Disable dynamic price bands — these tests focus on daily limit tiers.
    engine_.set_band_percentage(0);
    // Reference price 1000.0000 = 10000000.
    engine_.set_reference_price(10000000);

    EXPECT_EQ(engine_.current_limit_tier(), 1);
    // ±8% of 10000000 = ±800000.
    // daily_limit_high = 10800000, daily_limit_low = 9200000.
    // An order above the upper limit must be rejected.
    engine_.new_order(make_limit(1, 1, Side::Buy, 10800500, 10000));
    EXPECT_TRUE(has_reject(RejectReason::LockLimitUp))
        << "Order above tier 1 upper limit must be rejected";
}

TEST_F(KrxExchangeTest, TieredLimits_OrderAtLimitAccepted) {
    engine_.set_band_percentage(0);
    engine_.set_reference_price(10000000);

    // Exactly at ±8% boundary should be accepted.
    engine_.new_order(make_limit(1, 1, Side::Buy, 10800000, 10000));
    EXPECT_TRUE(has_accept()) << "Order at exact upper limit accepted";
}

TEST_F(KrxExchangeTest, TieredLimits_WidensOnBreach) {
    engine_.set_band_percentage(0);
    engine_.set_reference_price(10000000);
    // Tier 1: ±8% → limits [9200000, 10800000].

    // Trade at the upper limit to trigger on_daily_limit_hit.
    engine_.new_order(make_limit(1, 1, Side::Sell, 10800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 10800000, 10000));

    // Should have widened to tier 2: ±15% → [8500000, 11500000].
    EXPECT_EQ(engine_.current_limit_tier(), 2);

    order_listener_.clear();
    md_listener_.clear();

    // Now an order at 11400000 (within tier 2 limits) should be accepted.
    engine_.new_order(make_limit(3, 3, Side::Buy, 11400000, 10000));
    EXPECT_FALSE(has_reject(RejectReason::LockLimitUp))
        << "Order within tier 2 limits must be accepted";
}

TEST_F(KrxExchangeTest, TieredLimits_WidensTwice) {
    engine_.set_band_percentage(0);
    engine_.set_reference_price(10000000);
    // Tier 1: ±8% → [9200000, 10800000]

    // Hit tier 1 upper limit.
    engine_.new_order(make_limit(1, 1, Side::Sell, 10800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 10800000, 10000));
    EXPECT_EQ(engine_.current_limit_tier(), 2);
    // Tier 2: ±15% → [8500000, 11500000]

    order_listener_.clear();
    md_listener_.clear();

    // Hit tier 2 upper limit.
    engine_.new_order(make_limit(3, 3, Side::Sell, 11500000, 10000));
    engine_.new_order(make_limit(4, 4, Side::Buy, 11500000, 10000));
    EXPECT_EQ(engine_.current_limit_tier(), 3);
    // Tier 3: ±20% → [8000000, 12000000]

    order_listener_.clear();
    md_listener_.clear();

    // Order within tier 3 accepted.
    engine_.new_order(make_limit(5, 5, Side::Buy, 11900000, 10000));
    EXPECT_FALSE(has_reject(RejectReason::LockLimitUp))
        << "Order within tier 3 limits must be accepted";
}

TEST_F(KrxExchangeTest, TieredLimits_StopsAtTier3) {
    engine_.set_band_percentage(0);
    engine_.set_reference_price(10000000);

    // Escalate through all 3 tiers.
    engine_.new_order(make_limit(1, 1, Side::Sell, 10800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 10800000, 10000));
    EXPECT_EQ(engine_.current_limit_tier(), 2);

    engine_.new_order(make_limit(3, 3, Side::Sell, 11500000, 10000));
    engine_.new_order(make_limit(4, 4, Side::Buy, 11500000, 10000));
    EXPECT_EQ(engine_.current_limit_tier(), 3);

    // Hit tier 3 limit — should NOT widen further, stays at LockLimit.
    order_listener_.clear();
    md_listener_.clear();

    engine_.new_order(make_limit(5, 5, Side::Sell, 12000000, 10000));
    engine_.new_order(make_limit(6, 6, Side::Buy, 12000000, 10000));

    EXPECT_EQ(engine_.current_limit_tier(), 3)
        << "Must stay at tier 3 (max)";
    EXPECT_EQ(engine_.session_state(), SessionState::LockLimit)
        << "At max tier, must remain in LockLimit";
}

TEST_F(KrxExchangeTest, TieredLimits_ResumesToContinuousAfterWiden) {
    engine_.set_band_percentage(0);
    engine_.set_reference_price(10000000);

    engine_.new_order(make_limit(1, 1, Side::Sell, 10800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 10800000, 10000));

    // After widening (tier 1 → 2), should be back in Continuous.
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous)
        << "After tier widening, engine must resume Continuous trading";
}

TEST_F(KrxExchangeTest, TieredLimits_LowerLimitBreach) {
    engine_.set_band_percentage(0);
    engine_.set_reference_price(10000000);
    // Tier 1 lower = 9200000.

    engine_.new_order(make_limit(1, 1, Side::Buy, 9200000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Sell, 9200000, 10000));

    EXPECT_EQ(engine_.current_limit_tier(), 2)
        << "Lower limit breach also widens to next tier";
}

// ===========================================================================
// Configuration accessors
// ===========================================================================

TEST_F(KrxExchangeTest, DefaultBandPercentage) {
    EXPECT_EQ(engine_.band_percentage(), 5);
}

TEST_F(KrxExchangeTest, DefaultViDynamicPercentage) {
    EXPECT_EQ(engine_.vi_dynamic_percentage(), 3);
}

TEST_F(KrxExchangeTest, DefaultStaticBandPercentage) {
    EXPECT_EQ(engine_.static_band_percentage(), 10);
}

TEST_F(KrxExchangeTest, SidecarDefaultInactive) {
    EXPECT_FALSE(engine_.sidecar_active());
}

TEST_F(KrxExchangeTest, SetBandPercentage) {
    engine_.set_band_percentage(5);
    EXPECT_EQ(engine_.band_percentage(), 5);
}

TEST_F(KrxExchangeTest, SetStaticBandPercentage) {
    engine_.set_static_band_percentage(15);
    EXPECT_EQ(engine_.static_band_percentage(), 15);
}

TEST_F(KrxExchangeTest, SetPriorClosePrice) {
    engine_.set_prior_close_price(5000000);
    EXPECT_EQ(engine_.prior_close_price(), 5000000);
}

}  // namespace
}  // namespace krx
}  // namespace exchange
