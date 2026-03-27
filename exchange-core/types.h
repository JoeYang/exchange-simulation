#pragma once

#include <cstdint>

namespace exchange {

// --- Type aliases ---
using Price     = int64_t;   // fixed-point, 4 decimal places (100.5000 = 1005000)
using Quantity  = int64_t;   // fixed-point, 4 decimal places (1.0000 = 10000)
using OrderId   = uint64_t;  // engine-assigned, sequential starting at 1
using TradeId   = uint64_t;  // engine-assigned, sequential starting at 1
using Timestamp = int64_t;   // epoch nanoseconds

constexpr int64_t PRICE_SCALE = 10000;

// --- Enumerations ---
enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t { Limit, Market, Stop, StopLimit };

enum class TimeInForce : uint8_t { DAY, GTC, IOC, FOK, GTD };

enum class MatchAlgo : uint8_t { FIFO, ProRata };

enum class SmpAction : uint8_t {
    CancelNewest, CancelOldest, CancelBoth, Decrement, None
};

enum class ModifyPolicy : uint8_t {
    CancelReplace, AmendDown, RejectModify
};

enum class SessionState : uint8_t {
    Closed,
    PreOpen,
    OpeningAuction,
    Continuous,
    PreClose,
    ClosingAuction,
    Halt,
    VolatilityAuction,
    LockLimit,
};

enum class RejectReason : uint8_t {
    PoolExhausted, InvalidPrice, InvalidQuantity, InvalidTif, InvalidSide,
    UnknownOrder, PriceBandViolation, LevelPoolExhausted,
    MaxOrderSizeExceeded,  // quantity exceeds EngineConfig::max_order_size
    RateThrottled,         // per-account message rate limit exceeded
    LockLimitUp,           // order price at/above upper daily limit (locked)
    LockLimitDown,         // order price at/below lower daily limit (locked)
    PositionLimitExceeded, // would breach per-account position limit
    ExchangeSpecific
};

enum class CancelReason : uint8_t {
    UserRequested, IOCRemainder, FOKFailed, Expired,
    SelfMatchPrevention, LevelPoolExhausted,
    MassCancelled
};

enum class BustReason : uint8_t {
    ErroneousTrade,   // price or quantity was clearly erroneous
    SystemError,      // exchange system malfunction
    Regulatory,       // regulatory directive to bust
    ExchangeSpecific  // exchange-defined reason
};

// --- Core structs ---
struct PriceLevel;  // forward declaration

struct Order {
    OrderId id{0};
    uint64_t client_order_id{0};
    uint64_t account_id{0};
    Price price{0};
    Quantity quantity{0};
    Quantity filled_quantity{0};
    Quantity remaining_quantity{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    Timestamp timestamp{0};
    Timestamp gtd_expiry{0};

    // Iceberg fields (0 = fully visible, no iceberg)
    Quantity display_qty{0};    // visible quantity (0 = fully visible, no iceberg)
    Quantity total_qty{0};      // original total including hidden (same as quantity for non-iceberg)

    // Market maker (LMM) flag -- set by exchange-specific CRTP hook
    bool is_market_maker{false};

    // Intrusive doubly-linked list hooks (within a price level)
    Order* prev{nullptr};
    Order* next{nullptr};

    // Back-pointer to owning price level
    PriceLevel* level{nullptr};
};

struct PriceLevel {
    Price price{0};
    Quantity total_quantity{0};
    uint32_t order_count{0};

    Order* head{nullptr};
    Order* tail{nullptr};

    // Intrusive doubly-linked list hooks (within bid/ask side)
    PriceLevel* prev{nullptr};
    PriceLevel* next{nullptr};
};

struct FillResult {
    Order* resting_order{nullptr};
    Price price{0};
    Quantity quantity{0};
    Quantity resting_remaining{0};
};

}  // namespace exchange
