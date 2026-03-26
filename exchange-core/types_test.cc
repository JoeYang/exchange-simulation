#include "exchange-core/types.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

TEST(TypesTest, PriceScaleIsCorrect) {
    EXPECT_EQ(PRICE_SCALE, 10000);
}

TEST(TypesTest, SideEnumCoversAllValues) {
    auto check = [](Side s) {
        switch (s) {
            case Side::Buy:  return;
            case Side::Sell: return;
        }
    };
    check(Side::Buy);
    check(Side::Sell);
}

TEST(TypesTest, OrderTypeEnumCoversAllValues) {
    auto check = [](OrderType t) {
        switch (t) {
            case OrderType::Limit:     return;
            case OrderType::Market:    return;
            case OrderType::Stop:      return;
            case OrderType::StopLimit: return;
        }
    };
    check(OrderType::Limit);
    check(OrderType::Market);
    check(OrderType::Stop);
    check(OrderType::StopLimit);
}

TEST(TypesTest, TimeInForceEnumCoversAllValues) {
    auto check = [](TimeInForce t) {
        switch (t) {
            case TimeInForce::DAY: return;
            case TimeInForce::GTC: return;
            case TimeInForce::IOC: return;
            case TimeInForce::FOK: return;
            case TimeInForce::GTD: return;
        }
    };
    check(TimeInForce::DAY);
    check(TimeInForce::GTC);
    check(TimeInForce::IOC);
    check(TimeInForce::FOK);
    check(TimeInForce::GTD);
}

TEST(TypesTest, MatchAlgoEnumCoversAllValues) {
    auto check = [](MatchAlgo m) {
        switch (m) {
            case MatchAlgo::FIFO:    return;
            case MatchAlgo::ProRata: return;
        }
    };
    check(MatchAlgo::FIFO);
    check(MatchAlgo::ProRata);
}

TEST(TypesTest, SmpActionEnumCoversAllValues) {
    auto check = [](SmpAction a) {
        switch (a) {
            case SmpAction::CancelNewest: return;
            case SmpAction::CancelOldest: return;
            case SmpAction::CancelBoth:   return;
            case SmpAction::Decrement:    return;
            case SmpAction::None:         return;
        }
    };
    check(SmpAction::CancelNewest);
    check(SmpAction::CancelOldest);
    check(SmpAction::CancelBoth);
    check(SmpAction::Decrement);
    check(SmpAction::None);
}

TEST(TypesTest, ModifyPolicyEnumCoversAllValues) {
    auto check = [](ModifyPolicy p) {
        switch (p) {
            case ModifyPolicy::CancelReplace: return;
            case ModifyPolicy::AmendDown:     return;
            case ModifyPolicy::RejectModify:  return;
        }
    };
    check(ModifyPolicy::CancelReplace);
    check(ModifyPolicy::AmendDown);
    check(ModifyPolicy::RejectModify);
}

TEST(TypesTest, RejectReasonEnumCoversAllValues) {
    auto check = [](RejectReason r) {
        switch (r) {
            case RejectReason::PoolExhausted:      return;
            case RejectReason::InvalidPrice:       return;
            case RejectReason::InvalidQuantity:    return;
            case RejectReason::InvalidTif:         return;
            case RejectReason::InvalidSide:        return;
            case RejectReason::UnknownOrder:       return;
            case RejectReason::PriceBandViolation: return;
            case RejectReason::LevelPoolExhausted:   return;
            case RejectReason::MaxOrderSizeExceeded: return;
            case RejectReason::RateThrottled:        return;
            case RejectReason::LockLimitUp:          return;
            case RejectReason::LockLimitDown:        return;
            case RejectReason::ExchangeSpecific:     return;
        }
    };
    check(RejectReason::PoolExhausted);
    check(RejectReason::InvalidPrice);
    check(RejectReason::InvalidQuantity);
    check(RejectReason::InvalidTif);
    check(RejectReason::InvalidSide);
    check(RejectReason::UnknownOrder);
    check(RejectReason::PriceBandViolation);
    check(RejectReason::LevelPoolExhausted);
    check(RejectReason::MaxOrderSizeExceeded);
    check(RejectReason::RateThrottled);
    check(RejectReason::LockLimitUp);
    check(RejectReason::LockLimitDown);
    check(RejectReason::ExchangeSpecific);
}

TEST(TypesTest, CancelReasonEnumCoversAllValues) {
    auto check = [](CancelReason r) {
        switch (r) {
            case CancelReason::UserRequested:       return;
            case CancelReason::IOCRemainder:        return;
            case CancelReason::FOKFailed:           return;
            case CancelReason::Expired:             return;
            case CancelReason::SelfMatchPrevention: return;
            case CancelReason::LevelPoolExhausted:  return;
            case CancelReason::MassCancelled:       return;
        }
    };
    check(CancelReason::UserRequested);
    check(CancelReason::IOCRemainder);
    check(CancelReason::FOKFailed);
    check(CancelReason::Expired);
    check(CancelReason::SelfMatchPrevention);
    check(CancelReason::LevelPoolExhausted);
    check(CancelReason::MassCancelled);
}

TEST(TypesTest, OrderDefaultConstruction) {
    Order order{};
    EXPECT_EQ(order.id, 0u);
    EXPECT_EQ(order.price, 0);
    EXPECT_EQ(order.quantity, 0);
    EXPECT_EQ(order.filled_quantity, 0);
    EXPECT_EQ(order.remaining_quantity, 0);
    EXPECT_EQ(order.side, Side::Buy);
    EXPECT_EQ(order.type, OrderType::Limit);
    EXPECT_EQ(order.tif, TimeInForce::GTC);
    EXPECT_EQ(order.client_order_id, 0u);
    EXPECT_EQ(order.account_id, 0u);
    EXPECT_EQ(order.timestamp, 0);
    EXPECT_EQ(order.gtd_expiry, 0);
    EXPECT_EQ(order.prev, nullptr);
    EXPECT_EQ(order.next, nullptr);
    EXPECT_EQ(order.level, nullptr);
}

TEST(TypesTest, OrderFieldAssignment) {
    Order o{};
    o.id = 42;
    o.price = 1005000;
    o.quantity = 10000;
    o.side = Side::Sell;
    o.type = OrderType::Market;
    o.tif = TimeInForce::IOC;
    o.client_order_id = 99;
    o.account_id = 200;
    o.timestamp = 1000000000;
    o.gtd_expiry = 2000000000;
    EXPECT_EQ(o.id, 42u);
    EXPECT_EQ(o.price, 1005000);
    EXPECT_EQ(o.quantity, 10000);
    EXPECT_EQ(o.side, Side::Sell);
    EXPECT_EQ(o.type, OrderType::Market);
    EXPECT_EQ(o.tif, TimeInForce::IOC);
    EXPECT_EQ(o.client_order_id, 99u);
    EXPECT_EQ(o.account_id, 200u);
    EXPECT_EQ(o.timestamp, 1000000000);
    EXPECT_EQ(o.gtd_expiry, 2000000000);
}

TEST(TypesTest, PriceLevelDefaultConstruction) {
    PriceLevel level{};
    EXPECT_EQ(level.price, 0);
    EXPECT_EQ(level.total_quantity, 0);
    EXPECT_EQ(level.order_count, 0u);
    EXPECT_EQ(level.head, nullptr);
    EXPECT_EQ(level.tail, nullptr);
    EXPECT_EQ(level.prev, nullptr);
    EXPECT_EQ(level.next, nullptr);
}

TEST(TypesTest, FillResultDefaultConstruction) {
    FillResult fill{};
    EXPECT_EQ(fill.resting_order, nullptr);
    EXPECT_EQ(fill.price, 0);
    EXPECT_EQ(fill.quantity, 0);
    EXPECT_EQ(fill.resting_remaining, 0);
}

TEST(TypesTest, PriceRepresentation) {
    // 100.5000 should be represented as 1005000
    Price p = 1005000;
    EXPECT_EQ(p / PRICE_SCALE, 100);
    EXPECT_EQ(p % PRICE_SCALE, 5000);
}

TEST(TypesTest, SessionStateEnumCoversAllValues) {
    auto check = [](SessionState s) {
        switch (s) {
            case SessionState::Closed:            return;
            case SessionState::PreOpen:           return;
            case SessionState::OpeningAuction:    return;
            case SessionState::Continuous:        return;
            case SessionState::PreClose:          return;
            case SessionState::ClosingAuction:    return;
            case SessionState::Halt:              return;
            case SessionState::VolatilityAuction: return;
            case SessionState::LockLimit:         return;
        }
    };
    check(SessionState::Closed);
    check(SessionState::PreOpen);
    check(SessionState::OpeningAuction);
    check(SessionState::Continuous);
    check(SessionState::PreClose);
    check(SessionState::ClosingAuction);
    check(SessionState::Halt);
    check(SessionState::VolatilityAuction);
    check(SessionState::LockLimit);
}

TEST(TypesTest, SessionStateUnderlyingType) {
    // Verify the underlying type is uint8_t (space-efficient for network protocols).
    static_assert(sizeof(SessionState) == sizeof(uint8_t),
                  "SessionState must fit in one byte");
}

}  // namespace
}  // namespace exchange
