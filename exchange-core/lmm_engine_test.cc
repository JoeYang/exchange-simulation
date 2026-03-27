#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// LMM exchange: 40% market-maker priority, uses FifoLmmMatch<40>
// ---------------------------------------------------------------------------

class LmmExchange
    : public MatchingEngine<LmmExchange, RecordingOrderListener,
                            RecordingMdListener, FifoLmmMatch<40>,
                            100, 50, 1000> {
public:
    using Base = MatchingEngine<LmmExchange, RecordingOrderListener,
                                RecordingMdListener, FifoLmmMatch<40>,
                                100, 50, 1000>;
    using Base::Base;

    // MM orders identified by the request flag
    bool is_market_maker(const OrderRequest& req) {
        return req.is_market_maker;
    }
};

// ---------------------------------------------------------------------------
// Default exchange: no LMM (FIFO), verifies backward compatibility
// ---------------------------------------------------------------------------

class DefaultExchange
    : public MatchingEngine<DefaultExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch,
                            100, 50, 1000> {
public:
    using Base = MatchingEngine<DefaultExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch,
                                100, 50, 1000>;
    using Base::Base;
    // is_market_maker() defaults to false
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class LmmEngineTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};

    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty, Timestamp ts,
                            bool mm = false) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id = 1,
                            .side = side,
                            .type = OrderType::Limit,
                            .tif = TimeInForce::GTC,
                            .price = price,
                            .quantity = qty,
                            .stop_price = 0,
                            .timestamp = ts,
                            .gtd_expiry = 0,
                            .display_qty = 0,
                            .is_market_maker = mm};
    }
};

// ---------------------------------------------------------------------------
// LMM exchange: MM order gets priority allocation
// ---------------------------------------------------------------------------

TEST_F(LmmEngineTest, MmOrderGetsPriorityAllocation) {
    LmmExchange engine(config_, order_listener_, md_listener_);

    // MM buy rests at 100.0000 with 100000 qty
    engine.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, true));
    // Regular buy rests at same price
    engine.new_order(make_limit(2, Side::Buy, 1000000, 100000, 2, false));

    order_listener_.clear();
    md_listener_.clear();

    // Aggressor sells 100000 at 100.0000
    engine.new_order(make_limit(3, Side::Sell, 1000000, 100000, 3));

    // MM priority = floor(100000 * 40 / 100) = 40000
    // Phase 1: MM (ord 1) gets 40000
    // Phase 2: FIFO: MM (ord 1) gets min(60000, 60000) = 60000? No --
    //   remaining=60000, FIFO order is mm(1), then regular(2).
    //   mm has 60000 remaining, gets min(60000, 60000) = 60000.
    // So MM total = 100000, regular = 0? That can't be right for the user.
    // Actually: after phase 1, mm remaining_qty = 60000. In phase 2,
    // FIFO walks from head: mm is at head (earlier timestamp), gets 60000.
    // Regular gets nothing.
    //
    // This is correct per FIFO-LMM: MM has time priority AND mm priority.
    // Let me verify with fills.

    // Expected fills: MM gets 40000 (phase 1) + 60000 (phase 2) = 100000
    // Regular gets 0
    // Aggressor fully filled
    Quantity mm_total = 0;
    Quantity reg_total = 0;
    for (auto& ev : order_listener_.events()) {
        if (auto* pf = std::get_if<OrderPartiallyFilled>(&ev)) {
            if (pf->resting_id == 1) mm_total += pf->quantity;
            if (pf->resting_id == 2) reg_total += pf->quantity;
        }
        if (auto* f = std::get_if<OrderFilled>(&ev)) {
            if (f->resting_id == 1) mm_total += f->quantity;
            if (f->resting_id == 2) reg_total += f->quantity;
        }
    }
    // Since MM has time priority, it gets everything
    EXPECT_EQ(mm_total, 100000);
    EXPECT_EQ(reg_total, 0);
}

// ---------------------------------------------------------------------------
// LMM exchange: MM behind regular in time priority still gets priority share
// ---------------------------------------------------------------------------

TEST_F(LmmEngineTest, MmBehindRegularStillGetsPriority) {
    LmmExchange engine(config_, order_listener_, md_listener_);

    // Regular rests first (time priority)
    engine.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, false));
    // MM rests second
    engine.new_order(make_limit(2, Side::Buy, 1000000, 100000, 2, true));

    order_listener_.clear();

    // Aggressor sells 100000
    engine.new_order(make_limit(3, Side::Sell, 1000000, 100000, 3));

    Quantity mm_total = 0;
    Quantity reg_total = 0;
    for (auto& ev : order_listener_.events()) {
        if (auto* pf = std::get_if<OrderPartiallyFilled>(&ev)) {
            if (pf->resting_id == 1) reg_total += pf->quantity;
            if (pf->resting_id == 2) mm_total += pf->quantity;
        }
        if (auto* f = std::get_if<OrderFilled>(&ev)) {
            if (f->resting_id == 1) reg_total += f->quantity;
            if (f->resting_id == 2) mm_total += f->quantity;
        }
    }
    // MM priority = floor(100000 * 40 / 100) = 40000
    // Phase 1: MM (ord 2) gets 40000
    // Phase 2: remaining=60000, FIFO: regular (ord 1) gets 60000
    EXPECT_EQ(mm_total, 40000);
    EXPECT_EQ(reg_total, 60000);
}

// ---------------------------------------------------------------------------
// Default exchange (no LMM): pure FIFO behavior preserved
// ---------------------------------------------------------------------------

TEST_F(LmmEngineTest, DefaultExchangePureFifo) {
    DefaultExchange engine(config_, order_listener_, md_listener_);

    // Even if request sets is_market_maker=true, default hook returns false
    engine.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, true));
    engine.new_order(make_limit(2, Side::Buy, 1000000, 100000, 2, false));

    order_listener_.clear();

    // Aggressor sells 80000
    engine.new_order(make_limit(3, Side::Sell, 1000000, 80000, 3));

    Quantity ord1_total = 0;
    Quantity ord2_total = 0;
    for (auto& ev : order_listener_.events()) {
        if (auto* pf = std::get_if<OrderPartiallyFilled>(&ev)) {
            if (pf->resting_id == 1) ord1_total += pf->quantity;
            if (pf->resting_id == 2) ord2_total += pf->quantity;
        }
        if (auto* f = std::get_if<OrderFilled>(&ev)) {
            if (f->resting_id == 1) ord1_total += f->quantity;
            if (f->resting_id == 2) ord2_total += f->quantity;
        }
    }
    // Pure FIFO: ord1 (first in time) gets all 80000
    EXPECT_EQ(ord1_total, 80000);
    EXPECT_EQ(ord2_total, 0);
}

// ---------------------------------------------------------------------------
// LMM exchange: MM flag preserved across modify (cancel-replace)
// ---------------------------------------------------------------------------

TEST_F(LmmEngineTest, MmFlagPreservedAfterModify) {
    LmmExchange engine(config_, order_listener_, md_listener_);

    // Regular rests first
    engine.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, false));
    // MM rests second
    engine.new_order(make_limit(2, Side::Buy, 1000000, 100000, 2, true));

    // Modify MM order (cancel-replace: loses time priority but keeps MM flag)
    engine.modify_order(ModifyRequest{.order_id = 2,
                                       .client_order_id = 20,
                                       .new_price = 1000000,
                                       .new_quantity = 100000,
                                       .timestamp = 3});

    order_listener_.clear();

    // Aggressor sells 100000
    engine.new_order(make_limit(3, Side::Sell, 1000000, 100000, 4));

    Quantity mm_total = 0;
    Quantity reg_total = 0;
    for (auto& ev : order_listener_.events()) {
        if (auto* pf = std::get_if<OrderPartiallyFilled>(&ev)) {
            if (pf->resting_id == 1) reg_total += pf->quantity;
            if (pf->resting_id == 2) mm_total += pf->quantity;
        }
        if (auto* f = std::get_if<OrderFilled>(&ev)) {
            if (f->resting_id == 1) reg_total += f->quantity;
            if (f->resting_id == 2) mm_total += f->quantity;
        }
    }
    // MM still gets priority even after modify (flag on order, not request)
    // MM allocation = floor(100000 * 40/100) = 40000
    // Phase 1: MM (ord 2) gets 40000
    // Phase 2: FIFO: regular (ord 1, earlier) gets 60000
    EXPECT_EQ(mm_total, 40000);
    EXPECT_EQ(reg_total, 60000);
}

}  // namespace
}  // namespace exchange
