#include "ice/ice_exchange.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace ice {
namespace {

// Small pool sizes for fast tests.
using TestIceExchange = IceExchange<
    RecordingOrderListener,
    RecordingMdListener,
    FifoMatch,
    /*MaxOrders=*/     200,
    /*MaxPriceLevels=*/100,
    /*MaxOrderIds=*/   2000>;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class IceExchangeTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener    md_listener_;

    // Brent-like config: tick=0.01 (100), lot=1 contract (10000), no static bands
    EngineConfig config_{
        .tick_size       = 100,
        .lot_size        = 10000,
        .price_band_low  = 0,
        .price_band_high = 0,
        .max_order_size  = 0,
    };

    TestIceExchange engine_{config_, order_listener_, md_listener_};

    // Helpers ---

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
            .quantity        = qty,
            .stop_price      = 0,
            .timestamp       = ts,
            .gtd_expiry      = 0,
        };
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
            .quantity        = qty,
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
};

// ===========================================================================
// SMP: tag-based self-match prevention
// ===========================================================================

TEST_F(IceExchangeTest, SmpSameAccountIsSelfMatch) {
    // Resting sell
    engine_.new_order(make_limit(1, 42, Side::Sell, 800000, 10000));
    order_listener_.clear();

    // Aggressor buy from same account — should trigger SMP.
    // Default SMP action is CancelNewest (aggressor).
    engine_.new_order(make_limit(2, 42, Side::Buy, 800000, 10000));

    // Expect: OrderAccepted then OrderCancelled (SMP) for aggressor.
    bool has_cancel = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(e)) {
            auto& c = std::get<OrderCancelled>(e);
            EXPECT_EQ(c.reason, CancelReason::SelfMatchPrevention);
            has_cancel = true;
        }
    }
    EXPECT_TRUE(has_cancel);
}

TEST_F(IceExchangeTest, SmpZeroAccountDoesNotMatch) {
    // account_id 0 = no SMP (untagged).
    engine_.new_order(make_limit(1, 0, Side::Sell, 800000, 10000));
    order_listener_.clear();

    engine_.new_order(make_limit(2, 0, Side::Buy, 800000, 10000));

    // Should produce a fill, not an SMP cancel.
    bool has_fill = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(e)) has_fill = true;
    }
    EXPECT_TRUE(has_fill);
}

// ===========================================================================
// SMP action: RTO (cancel resting), ATO (cancel aggressor), CABO (both)
// ===========================================================================

TEST_F(IceExchangeTest, SmpActionRtoCancelsResting) {
    engine_.set_smp_action(SmpAction::CancelOldest);  // RTO = cancel resting

    engine_.new_order(make_limit(1, 42, Side::Sell, 800000, 10000));
    order_listener_.clear();

    engine_.new_order(make_limit(2, 42, Side::Buy, 800000, 10000));

    // Resting order (id=1) should be cancelled.
    bool resting_cancelled = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(e)) {
            auto& c = std::get<OrderCancelled>(e);
            if (c.id == 1) resting_cancelled = true;
        }
    }
    EXPECT_TRUE(resting_cancelled);
}

TEST_F(IceExchangeTest, SmpActionCaboCancelsBoth) {
    engine_.set_smp_action(SmpAction::CancelBoth);  // CABO

    engine_.new_order(make_limit(1, 42, Side::Sell, 800000, 10000));
    order_listener_.clear();

    engine_.new_order(make_limit(2, 42, Side::Buy, 800000, 10000));

    // Both orders should be cancelled.
    size_t cancel_count = 0;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(e)) ++cancel_count;
    }
    EXPECT_GE(cancel_count, 2u);
}

// ===========================================================================
// IPL dynamic bands (fixed-point width)
// ===========================================================================

TEST_F(IceExchangeTest, IplBandsRejectOutOfRange) {
    engine_.set_ipl_width(5000);  // ±0.50 around last trade

    // Establish a last trade price by crossing two orders.
    engine_.new_order(make_limit(1, 1, Side::Sell, 800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 800000, 10000));
    order_listener_.clear();

    // Now last_trade_price_ = 800000.
    // Band: [795000, 805000].
    // Order outside band should be rejected.
    engine_.new_order(make_limit(3, 3, Side::Buy, 810000, 10000));

    bool rejected = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(e)) {
            auto& r = std::get<OrderRejected>(e);
            EXPECT_EQ(r.reason, RejectReason::PriceBandViolation);
            rejected = true;
        }
    }
    EXPECT_TRUE(rejected);
}

TEST_F(IceExchangeTest, IplBandsAcceptWithinRange) {
    engine_.set_ipl_width(5000);

    engine_.new_order(make_limit(1, 1, Side::Sell, 800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 800000, 10000));
    order_listener_.clear();

    // Within band [795000, 805000].
    engine_.new_order(make_limit(3, 3, Side::Buy, 804000, 10000));

    bool accepted = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(e)) accepted = true;
    }
    EXPECT_TRUE(accepted);
}

TEST_F(IceExchangeTest, IplBandsDisabledWhenWidthZero) {
    engine_.set_ipl_width(0);  // disabled

    engine_.new_order(make_limit(1, 1, Side::Sell, 800000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy, 800000, 10000));
    order_listener_.clear();

    // Any price should be accepted.
    engine_.new_order(make_limit(3, 3, Side::Buy, 9990000, 10000));

    bool accepted = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(e)) accepted = true;
    }
    EXPECT_TRUE(accepted);
}

// ===========================================================================
// VWAP settlement
// ===========================================================================

TEST_F(IceExchangeTest, VwapSettlementCalculation) {
    engine_.reset_vwap();

    // Trade 1: 10 lots @ 80.00 (800000 fixed-point)
    engine_.record_trade_for_vwap(800000, 100000);
    // Trade 2: 20 lots @ 81.00 (810000 fixed-point)
    engine_.record_trade_for_vwap(810000, 200000);

    // VWAP = (800000*100000 + 810000*200000) / (100000 + 200000)
    //      = (80000000000 + 162000000000) / 300000
    //      = 242000000000 / 300000
    //      = 806666 (truncated)
    Price vwap = engine_.calculate_settlement_price();
    EXPECT_EQ(vwap, 806666);
}

TEST_F(IceExchangeTest, VwapSettlementNoTrades) {
    engine_.reset_vwap();
    EXPECT_EQ(engine_.calculate_settlement_price(), 0);
}

TEST_F(IceExchangeTest, VwapResetClearsAccumulator) {
    engine_.record_trade_for_vwap(800000, 100000);
    engine_.reset_vwap();
    EXPECT_EQ(engine_.calculate_settlement_price(), 0);
}

// ===========================================================================
// Phase validation: reject Market/IOC/FOK in PreOpen
// ===========================================================================

TEST_F(IceExchangeTest, PreOpenRejectsMarketOrder) {
    engine_.set_session_state(SessionState::PreOpen, 500);
    order_listener_.clear();

    engine_.new_order(make_market(1, 1, Side::Buy, 10000, 1000));

    bool rejected = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(e)) rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST_F(IceExchangeTest, PreOpenRejectsIocOrder) {
    engine_.set_session_state(SessionState::PreOpen, 500);
    order_listener_.clear();

    engine_.new_order(make_ioc(1, 1, Side::Buy, 800000, 10000, 1000));

    bool rejected = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(e)) rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST_F(IceExchangeTest, PreOpenRejectsFokOrder) {
    engine_.set_session_state(SessionState::PreOpen, 500);
    order_listener_.clear();

    engine_.new_order(make_fok(1, 1, Side::Buy, 800000, 10000, 1000));

    bool rejected = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(e)) rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST_F(IceExchangeTest, PreOpenAcceptsLimitGtc) {
    engine_.set_session_state(SessionState::PreOpen, 500);
    order_listener_.clear();

    engine_.new_order(make_limit(1, 1, Side::Buy, 800000, 10000,
                                 TimeInForce::GTC, 1000));

    bool accepted = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(e)) accepted = true;
    }
    EXPECT_TRUE(accepted);
}

// ===========================================================================
// Circuit breaker: hold + resume
// ===========================================================================

TEST_F(IceExchangeTest, CircuitBreakerTransitionsToHalt) {
    engine_.trigger_circuit_breaker(/*hold_until=*/5000, /*ts=*/1000);

    EXPECT_EQ(engine_.session_state(), SessionState::Halt);
    EXPECT_TRUE(engine_.circuit_breaker_active());
    EXPECT_EQ(engine_.circuit_breaker_resume_ts(), 5000);
}

TEST_F(IceExchangeTest, CircuitBreakerResumeRestoresContinuous) {
    engine_.trigger_circuit_breaker(5000, 1000);
    engine_.resume_from_circuit_breaker(5000);

    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
    EXPECT_FALSE(engine_.circuit_breaker_active());
    EXPECT_EQ(engine_.circuit_breaker_resume_ts(), 0);
}

TEST_F(IceExchangeTest, CircuitBreakerResumeNoOpWhenNotActive) {
    // Should not crash or change state.
    engine_.resume_from_circuit_breaker(1000);
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
    EXPECT_FALSE(engine_.circuit_breaker_active());
}

TEST_F(IceExchangeTest, OrdersAcceptedDuringHalt) {
    // ICE allows order entry during halt (unlike some exchanges).
    engine_.trigger_circuit_breaker(5000, 1000);
    order_listener_.clear();

    engine_.new_order(make_limit(1, 1, Side::Buy, 800000, 10000,
                                 TimeInForce::GTC, 2000));

    bool accepted = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(e)) accepted = true;
    }
    EXPECT_TRUE(accepted);
}

// ===========================================================================
// Modify policy: cancel-replace
// ===========================================================================

TEST_F(IceExchangeTest, ModifyPolicyIsCancelReplace) {
    EXPECT_EQ(engine_.get_modify_policy(), ModifyPolicy::CancelReplace);
}

}  // namespace
}  // namespace ice
}  // namespace exchange
