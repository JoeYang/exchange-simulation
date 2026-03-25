#include "cme/cme_exchange.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace cme {
namespace {

// ---------------------------------------------------------------------------
// Convenience alias: small pool sizes keep tests fast
// ---------------------------------------------------------------------------

using TestCmeExchange = CmeExchange<
    RecordingOrderListener,
    RecordingMdListener,
    FifoMatch,
    /*MaxOrders=*/     200,
    /*MaxPriceLevels=*/100,
    /*MaxOrderIds=*/   2000>;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class CmeExchangeTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener    md_listener_;

    // ES-like config: tick=0.25 (2500), lot=1 contract (10000), no static bands
    EngineConfig config_{
        .tick_size       = 2500,
        .lot_size        = 10000,
        .price_band_low  = 0,
        .price_band_high = 0,
        .max_order_size  = 0,
    };

    TestCmeExchange engine_{config_, order_listener_, md_listener_};

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
            .quantity        = qty,
            .stop_price      = 0,
            .timestamp       = ts,
            .gtd_expiry      = 0,
        };
    }
};

// ===========================================================================
// Basic order acceptance (continuous trading)
// ===========================================================================

// A limit order submitted in Continuous state (the default initial state)
// must be accepted and rest on the book.
TEST_F(CmeExchangeTest, LimitOrderAcceptedInContinuous) {
    engine_.new_order(make_limit(1, 42, Side::Buy, 50000000, 10000));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<OrderAccepted>(order_listener_.events()[0]));
    auto& accepted = std::get<OrderAccepted>(order_listener_.events()[0]);
    EXPECT_EQ(accepted.id, 1u);
    EXPECT_EQ(accepted.client_order_id, 1u);
}

// Two limit orders on opposite sides that cross must produce a trade.
TEST_F(CmeExchangeTest, CrossingLimitOrdersProduceTrade) {
    // Resting sell at 5000.0000
    engine_.new_order(make_limit(1, 1, Side::Sell, 50000000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy at 5000.0000 — crosses
    engine_.new_order(make_limit(2, 2, Side::Buy, 50000000, 10000));

    // Should see OrderFilled for the aggressor
    bool saw_filled = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) { saw_filled = true; break; }
    }
    EXPECT_TRUE(saw_filled);

    // Should see a Trade on the MD side
    bool saw_trade = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            saw_trade = true;
            auto& t = std::get<Trade>(ev);
            EXPECT_EQ(t.price,    50000000);
            EXPECT_EQ(t.quantity, 10000);
        }
    }
    EXPECT_TRUE(saw_trade);
}

// ===========================================================================
// Self-Match Prevention (SMP)
// ===========================================================================

// When the aggressor and resting order share the same account_id, the engine
// must apply SMP.  CME default = CancelNewest (aggressor cancelled), no trade.
TEST_F(CmeExchangeTest, SmpCancelNewest_SameAccountNoCross) {
    // Resting sell from account 99
    engine_.new_order(make_limit(1, 99, Side::Sell, 50000000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy from the same account 99 — should trigger SMP
    engine_.new_order(make_limit(2, 99, Side::Buy, 50000000, 10000));

    // No trade must be produced
    for (const auto& ev : md_listener_.events()) {
        EXPECT_FALSE(std::holds_alternative<Trade>(ev))
            << "SMP should prevent a trade";
    }

    // The aggressor (newest) must be cancelled with SelfMatchPrevention reason
    bool saw_smp_cancel = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(ev)) {
            auto& c = std::get<OrderCancelled>(ev);
            if (c.reason == CancelReason::SelfMatchPrevention) {
                saw_smp_cancel = true;
            }
        }
    }
    EXPECT_TRUE(saw_smp_cancel) << "Aggressor must be cancelled via SMP";
}

// Different accounts crossing must NOT trigger SMP.
TEST_F(CmeExchangeTest, SmpNotTriggered_DifferentAccounts) {
    engine_.new_order(make_limit(1, 1, Side::Sell, 50000000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    engine_.new_order(make_limit(2, 2, Side::Buy, 50000000, 10000));

    bool saw_trade = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) saw_trade = true;
    }
    EXPECT_TRUE(saw_trade) << "Different accounts must trade normally";
}

// ===========================================================================
// Phase validation — PreOpen (auction collection)
// ===========================================================================

// During PreOpen: IOC orders must be rejected.
TEST_F(CmeExchangeTest, IocRejectedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_ioc(10, 1, Side::Buy, 50000000, 10000));

    ASSERT_GE(order_listener_.size(), 1u);
    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_TRUE(saw_reject) << "IOC must be rejected in PreOpen";
}

// During PreOpen: FOK orders must be rejected.
TEST_F(CmeExchangeTest, FokRejectedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_fok(11, 1, Side::Buy, 50000000, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_TRUE(saw_reject) << "FOK must be rejected in PreOpen";
}

// During PreOpen: Market orders must be rejected.
TEST_F(CmeExchangeTest, MarketOrderRejectedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_market(12, 1, Side::Buy, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_TRUE(saw_reject) << "Market order must be rejected in PreOpen";
}

// During PreOpen: GTC limit orders must be accepted (collected for auction).
TEST_F(CmeExchangeTest, LimitGtcAcceptedDuringPreOpen) {
    engine_.set_session_state(SessionState::PreOpen, 2000);
    order_listener_.clear();

    engine_.new_order(make_limit(13, 1, Side::Buy, 50000000, 10000));

    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<OrderAccepted>(order_listener_.events()[0]))
        << "GTC limit must be accepted during PreOpen";
}

// During PreClose: IOC must also be rejected (same auction-collection rules).
TEST_F(CmeExchangeTest, IocRejectedDuringPreClose) {
    engine_.set_session_state(SessionState::PreClose, 3000);
    order_listener_.clear();

    engine_.new_order(make_ioc(14, 1, Side::Sell, 50000000, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_TRUE(saw_reject) << "IOC must be rejected in PreClose";
}

// During VolatilityAuction: IOC must be rejected.
TEST_F(CmeExchangeTest, IocRejectedDuringVolatilityAuction) {
    engine_.set_session_state(SessionState::VolatilityAuction, 4000);
    order_listener_.clear();

    engine_.new_order(make_ioc(15, 1, Side::Buy, 50000000, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_TRUE(saw_reject) << "IOC must be rejected in VolatilityAuction";
}

// During Continuous: IOC must be accepted (not blocked by phase validation).
TEST_F(CmeExchangeTest, IocAcceptedDuringContinuous) {
    // Place a resting order first so IOC has something to match against.
    engine_.new_order(make_limit(20, 2, Side::Sell, 50000000, 10000));
    order_listener_.clear();

    engine_.new_order(make_ioc(21, 1, Side::Buy, 50000000, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_FALSE(saw_reject) << "IOC must NOT be rejected in Continuous";
}

// ===========================================================================
// Dynamic price bands
// ===========================================================================

// After a trade establishes last_trade_price_, orders outside the ±5% band
// must be rejected with PriceBandViolation.
TEST_F(CmeExchangeTest, OrderOutsideDynamicBandRejected) {
    // Establish a reference trade at 5000.0000. Different accounts avoid SMP.
    engine_.new_order(make_limit(1, 1, Side::Sell, 50000000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy,  50000000, 10000));
    order_listener_.clear();
    md_listener_.clear();

    // ±5% of 50000000 = ±2500000 → band = [47500000, 52500000]
    // 40000000 is far below the lower bound.
    engine_.new_order(make_limit(3, 3, Side::Buy, 40000000, 10000));

    bool saw_band_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) {
            auto& r = std::get<OrderRejected>(ev);
            if (r.reason == RejectReason::PriceBandViolation)
                saw_band_reject = true;
        }
    }
    EXPECT_TRUE(saw_band_reject)
        << "Order below dynamic band must be rejected with PriceBandViolation";
}

// An order exactly at the band lower bound must be accepted.
TEST_F(CmeExchangeTest, OrderAtBandBoundaryAccepted) {
    engine_.new_order(make_limit(1, 1, Side::Sell, 50000000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy,  50000000, 10000));
    order_listener_.clear();

    // Band low = 50000000 - 2500000 = 47500000; 47500000 % 2500 == 0 → tick-aligned
    engine_.new_order(make_limit(3, 3, Side::Buy, 47500000, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) saw_reject = true;
    }
    EXPECT_FALSE(saw_reject)
        << "Order at exact band boundary must not be rejected";
}

// Before the first trade (last_trade_price_ == 0) bands return {0,0} and
// no band check is applied.
TEST_F(CmeExchangeTest, NoBandCheckBeforeFirstTrade) {
    engine_.new_order(make_limit(1, 1, Side::Buy, 2500, 10000));  // price = 0.25

    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<OrderAccepted>(order_listener_.events()[0]))
        << "With no reference price, band check must be skipped";
}

// band_pct == 0 disables bands entirely — even after a reference trade.
TEST_F(CmeExchangeTest, BandDisabledWhenPctIsZero) {
    engine_.set_band_percentage(0);

    engine_.new_order(make_limit(1, 1, Side::Sell, 50000000, 10000));
    engine_.new_order(make_limit(2, 2, Side::Buy,  50000000, 10000));
    order_listener_.clear();

    // With band_pct == 0, any tick-aligned price must pass.
    engine_.new_order(make_limit(3, 3, Side::Buy, 2500, 10000));

    bool saw_band_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) {
            auto& r = std::get<OrderRejected>(ev);
            if (r.reason == RejectReason::PriceBandViolation)
                saw_band_reject = true;
        }
    }
    EXPECT_FALSE(saw_band_reject)
        << "band_pct=0 must disable price band validation";
}

}  // namespace
}  // namespace cme
}  // namespace exchange
