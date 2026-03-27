#pragma once

#include "exchange-core/events.h"

#include <string>
#include <variant>

namespace exchange {

// RecordedEvent holds any callback event type produced by the matching engine.
// Used by test harness recording listeners to capture and compare event sequences.
using RecordedEvent = std::variant<
    OrderAccepted,
    OrderRejected,
    OrderFilled,
    OrderPartiallyFilled,
    OrderCancelled,
    OrderCancelRejected,
    OrderModified,
    OrderModifyRejected,
    TopOfBook,
    DepthUpdate,
    OrderBookAction,
    Trade,
    MarketStatus,
    IndicativePrice,
    TradeBusted,
    LockLimitTriggered
>;

// Equality comparisons for all event types.
// Required for EXPECT_EQ assertions in tests.
bool operator==(const OrderAccepted& a, const OrderAccepted& b);
bool operator==(const OrderRejected& a, const OrderRejected& b);
bool operator==(const OrderFilled& a, const OrderFilled& b);
bool operator==(const OrderPartiallyFilled& a, const OrderPartiallyFilled& b);
bool operator==(const OrderCancelled& a, const OrderCancelled& b);
bool operator==(const OrderCancelRejected& a, const OrderCancelRejected& b);
bool operator==(const OrderModified& a, const OrderModified& b);
bool operator==(const OrderModifyRejected& a, const OrderModifyRejected& b);
bool operator==(const TopOfBook& a, const TopOfBook& b);
bool operator==(const DepthUpdate& a, const DepthUpdate& b);
bool operator==(const OrderBookAction& a, const OrderBookAction& b);
bool operator==(const Trade& a, const Trade& b);
bool operator==(const MarketStatus& a, const MarketStatus& b);
bool operator==(const IndicativePrice& a, const IndicativePrice& b);
bool operator==(const TradeBusted& a, const TradeBusted& b);
bool operator==(const LockLimitTriggered& a, const LockLimitTriggered& b);

// Human-readable string representation for test assertion diffs.
// Format: TypeName{field=value, ...}
// Example: OrderFilled{aggressor=2, resting=1, price=1005000, qty=10000, ts=2000}
std::string to_string(const RecordedEvent& event);

}  // namespace exchange
