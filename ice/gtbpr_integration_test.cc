#include "ice/gtbpr_match.h"
#include "ice/ice_exchange.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace exchange {
namespace ice {
namespace {

// Engine type aliases for GTBPR and FIFO.
using GtbprEngine = IceExchange<
    RecordingOrderListener, RecordingMdListener, GtbprMatch,
    /*MaxOrders=*/200, /*MaxPriceLevels=*/100, /*MaxOrderIds=*/2000>;

using FifoEngine = IceExchange<
    RecordingOrderListener, RecordingMdListener, FifoMatch,
    /*MaxOrders=*/200, /*MaxPriceLevels=*/100, /*MaxOrderIds=*/2000>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Collect fills for a given resting order ID from the event stream.
struct FillInfo {
    OrderId aggressor_id;
    OrderId resting_id;
    Price price;
    Quantity quantity;
};

std::vector<FillInfo> collect_fills(const RecordingOrderListener& listener) {
    std::vector<FillInfo> fills;
    for (const auto& e : listener.events()) {
        if (std::holds_alternative<OrderFilled>(e)) {
            auto& f = std::get<OrderFilled>(e);
            fills.push_back({f.aggressor_id, f.resting_id, f.price, f.quantity});
        }
        if (std::holds_alternative<OrderPartiallyFilled>(e)) {
            auto& f = std::get<OrderPartiallyFilled>(e);
            fills.push_back({f.aggressor_id, f.resting_id, f.price, f.quantity});
        }
    }
    return fills;
}

Quantity total_fill_for(const std::vector<FillInfo>& fills, OrderId resting_id) {
    Quantity total = 0;
    for (const auto& f : fills) {
        if (f.resting_id == resting_id) total += f.quantity;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Test fixture: STIR-like product (Euribor) with GTBPR matching.
// tick=0.005 (50), lot=1 (10000), collar=5 lots (50000), cap=20 lots (200000)
// ---------------------------------------------------------------------------
class GtbprIntegrationTest : public ::testing::Test {
protected:
    RecordingOrderListener ol_;
    RecordingMdListener    ml_;

    EngineConfig config_{
        .tick_size       = 50,
        .lot_size        = 10000,
        .price_band_low  = 0,
        .price_band_high = 0,
        .max_order_size  = 0,
    };

    GtbprEngine engine_{config_, ol_, ml_};

    OrderRequest limit(uint64_t cl_id, Side side, Price price,
                       Quantity qty, Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id, .account_id = 0,
            .side = side, .type = OrderType::Limit,
            .tif = TimeInForce::GTC, .price = price,
            .quantity = qty, .stop_price = 0,
            .timestamp = ts, .gtd_expiry = 0,
        };
    }
};

// ===========================================================================
// 1. Priority fill through engine
// ===========================================================================
TEST_F(GtbprIntegrationTest, PriorityOrderFillsFirst) {
    // Resting: o1 = 10 lots (qualifies for priority), o2 = 10 lots
    engine_.new_order(limit(1, Side::Sell, 1000000, 100000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 100000, 200));
    ol_.clear();

    // Aggressor: buy 15 lots — not enough for both
    engine_.new_order(limit(3, Side::Buy, 1000000, 150000, 300));

    auto fills = collect_fills(ol_);
    Quantity o1_fill = total_fill_for(fills, 1);  // engine OrderId=1
    Quantity o2_fill = total_fill_for(fills, 2);  // engine OrderId=2

    // o1 gets priority fill (up to cap=200000, pool=150000) = all 100000
    // Remaining 50000 goes to o2 via pro-rata
    EXPECT_EQ(o1_fill, 100000);
    EXPECT_EQ(o2_fill, 50000);
}

// ===========================================================================
// 2. Pro-rata distribution (no priority — all below collar)
// ===========================================================================
TEST_F(GtbprIntegrationTest, ProRataDistributionBelowCollar) {
    // 3 resting orders, all below collar (3 lots each < 5 lots collar)
    engine_.new_order(limit(1, Side::Sell, 1000000, 30000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 30000, 200));
    engine_.new_order(limit(3, Side::Sell, 1000000, 30000, 300));
    ol_.clear();

    // Aggressor buys entire level
    engine_.new_order(limit(4, Side::Buy, 1000000, 90000, 400));

    auto fills = collect_fills(ol_);
    Quantity o1_fill = total_fill_for(fills, 1);
    Quantity o2_fill = total_fill_for(fills, 2);
    Quantity o3_fill = total_fill_for(fills, 3);

    // All get filled (entire level consumed)
    EXPECT_EQ(o1_fill + o2_fill + o3_fill, 90000);
    EXPECT_EQ(o1_fill, 30000);
    EXPECT_EQ(o2_fill, 30000);
    EXPECT_EQ(o3_fill, 30000);
}

// ===========================================================================
// 3. Collar boundary — exactly at collar
// ===========================================================================
TEST_F(GtbprIntegrationTest, CollarBoundaryExactlyAtCollar) {
    // o1 has exactly collar qty (5 lots = 50000), should get priority
    engine_.new_order(limit(1, Side::Sell, 1000000, 50000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 50000, 200));
    ol_.clear();

    // Aggressor buy 30000 (3 lots)
    engine_.new_order(limit(3, Side::Buy, 1000000, 30000, 300));

    auto fills = collect_fills(ol_);
    Quantity o1_fill = total_fill_for(fills, 1);
    Quantity o2_fill = total_fill_for(fills, 2);

    // o1 qualifies for priority, gets all 30000
    EXPECT_EQ(o1_fill, 30000);
    EXPECT_EQ(o2_fill, 0);
}

// ===========================================================================
// 4. Collar boundary — one below collar
// ===========================================================================
TEST_F(GtbprIntegrationTest, CollarBoundaryOneBelowCollar) {
    // o1 has collar-1 qty (49999) — does NOT qualify for priority.
    // This is not a valid lot size though (not multiple of 10000).
    // Use 40000 (4 lots, below collar of 5 lots).
    engine_.new_order(limit(1, Side::Sell, 1000000, 40000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 40000, 200));
    ol_.clear();

    // Aggressor buy 40000
    engine_.new_order(limit(3, Side::Buy, 1000000, 40000, 300));

    auto fills = collect_fills(ol_);
    Quantity o1_fill = total_fill_for(fills, 1);
    Quantity o2_fill = total_fill_for(fills, 2);

    // No priority — pure pro-rata. With now=0, both have age=0, tw=1.0.
    // Equal qty + equal weight → each gets 20000.
    EXPECT_EQ(o1_fill, 20000);
    EXPECT_EQ(o2_fill, 20000);
}

// ===========================================================================
// 5. Cap exhaustion: priority order >> cap
// ===========================================================================
TEST_F(GtbprIntegrationTest, CapExhaustion) {
    // o1 has 50 lots (500000), well above collar. cap = 20 lots (200000).
    engine_.new_order(limit(1, Side::Sell, 1000000, 500000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 500000, 200));
    ol_.clear();

    // Aggressor buy all 100 lots
    engine_.new_order(limit(3, Side::Buy, 1000000, 1000000, 300));

    auto fills = collect_fills(ol_);
    Quantity o1_fill = total_fill_for(fills, 1);
    Quantity o2_fill = total_fill_for(fills, 2);

    // o1 gets 200000 priority, then pro-rata on remaining 800000.
    // With now=0 both have tw=1.0. o1 effective=300000, o2 effective=500000.
    // o1 pro-rata = floor(300000/800000 * 800000) = 300000
    // o2 pro-rata = floor(500000/800000 * 800000) = 500000
    // o1 total = 200000 + 300000 = 500000, o2 = 500000
    EXPECT_EQ(o1_fill + o2_fill, 1000000);
    // o1 must have gotten at least the cap as priority
    EXPECT_GE(o1_fill, 200000);
}

// ===========================================================================
// 6. All orders same age, equal size — equal pro-rata + FIFO remainder
// ===========================================================================
TEST_F(GtbprIntegrationTest, AllOrdersSameAgeSameSize) {
    // 4 resting orders below collar (3 lots each)
    engine_.new_order(limit(1, Side::Sell, 1000000, 30000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 30000, 100));
    engine_.new_order(limit(3, Side::Sell, 1000000, 30000, 100));
    engine_.new_order(limit(4, Side::Sell, 1000000, 30000, 100));
    ol_.clear();

    // Aggressor buys 7 lots (70000) — not divisible by 4
    engine_.new_order(limit(5, Side::Buy, 1000000, 70000, 200));

    auto fills = collect_fills(ol_);
    Quantity total = 0;
    for (const auto& f : fills) total += f.quantity;
    EXPECT_EQ(total, 70000);

    // Each should get floor(70000/4) = 17500, total 70000. No remainder.
    // (With now=0, all have tw=1.0, equal qty → equal weights)
    Quantity o1_fill = total_fill_for(fills, 1);
    Quantity o2_fill = total_fill_for(fills, 2);
    Quantity o3_fill = total_fill_for(fills, 3);
    Quantity o4_fill = total_fill_for(fills, 4);
    EXPECT_EQ(o1_fill, 17500);
    EXPECT_EQ(o2_fill, 17500);
    EXPECT_EQ(o3_fill, 17500);
    EXPECT_EQ(o4_fill, 17500);
}

// ===========================================================================
// 7. Single order at level — gets full fill
// ===========================================================================
TEST_F(GtbprIntegrationTest, SingleOrderAtLevel) {
    engine_.new_order(limit(1, Side::Sell, 1000000, 100000, 100));
    ol_.clear();

    engine_.new_order(limit(2, Side::Buy, 1000000, 50000, 200));

    auto fills = collect_fills(ol_);
    Quantity o1_fill = total_fill_for(fills, 1);
    EXPECT_EQ(o1_fill, 50000);
}

// ===========================================================================
// 8. FIFO vs GTBPR comparison — different allocation behavior
// ===========================================================================
TEST(FifoVsGtbprTest, DifferentAllocationBehavior) {
    EngineConfig config{
        .tick_size = 50, .lot_size = 10000,
        .price_band_low = 0, .price_band_high = 0,
        .max_order_size = 0,
    };

    // --- FIFO engine ---
    RecordingOrderListener fifo_ol;
    RecordingMdListener    fifo_ml;
    FifoEngine fifo{config, fifo_ol, fifo_ml};

    // --- GTBPR engine ---
    RecordingOrderListener gtbpr_ol;
    RecordingMdListener    gtbpr_ml;
    GtbprEngine gtbpr{config, gtbpr_ol, gtbpr_ml};

    // Submit identical order sequences to both.
    // 3 resting sells, then an aggressor buy that partially fills.
    auto make = [](uint64_t cl_id, Side side, Price price,
                   Quantity qty, Timestamp ts) {
        return OrderRequest{
            .client_order_id = cl_id, .account_id = 0,
            .side = side, .type = OrderType::Limit,
            .tif = TimeInForce::GTC, .price = price,
            .quantity = qty, .stop_price = 0,
            .timestamp = ts, .gtd_expiry = 0,
        };
    };

    // 3 resting sells: 10 lots each
    for (auto* eng : {static_cast<void*>(&fifo), static_cast<void*>(&gtbpr)}) {
        if (eng == &fifo) {
            fifo.new_order(make(1, Side::Sell, 1000000, 100000, 100));
            fifo.new_order(make(2, Side::Sell, 1000000, 100000, 200));
            fifo.new_order(make(3, Side::Sell, 1000000, 100000, 300));
        } else {
            gtbpr.new_order(make(1, Side::Sell, 1000000, 100000, 100));
            gtbpr.new_order(make(2, Side::Sell, 1000000, 100000, 200));
            gtbpr.new_order(make(3, Side::Sell, 1000000, 100000, 300));
        }
    }
    fifo_ol.clear();
    gtbpr_ol.clear();

    // Aggressor buy: 15 lots (partial fill of level)
    fifo.new_order(make(4, Side::Buy, 1000000, 150000, 400));
    gtbpr.new_order(make(4, Side::Buy, 1000000, 150000, 400));

    auto fifo_fills = collect_fills(fifo_ol);
    auto gtbpr_fills = collect_fills(gtbpr_ol);

    // FIFO: fills in time order. o1 gets 100000, o2 gets 50000, o3 gets 0.
    EXPECT_EQ(total_fill_for(fifo_fills, 1), 100000);
    EXPECT_EQ(total_fill_for(fifo_fills, 2), 50000);
    EXPECT_EQ(total_fill_for(fifo_fills, 3), 0);

    // GTBPR: o1 gets priority (all 100000 since pool=150000, cap=200000),
    // then remaining 50000 distributed to o2 and o3 via pro-rata.
    // o2 and o3 have equal qty and age relative to now=0 → each gets 25000.
    Quantity g1 = total_fill_for(gtbpr_fills, 1);
    Quantity g2 = total_fill_for(gtbpr_fills, 2);
    Quantity g3 = total_fill_for(gtbpr_fills, 3);

    EXPECT_EQ(g1, 100000);   // priority order gets same as FIFO first order
    EXPECT_EQ(g2 + g3, 50000);  // remainder distributed
    // GTBPR distributes to BOTH o2 and o3 — unlike FIFO which only fills o2
    EXPECT_GT(g3, 0);
}

// ===========================================================================
// 9. Multiple aggressors sequential — priority state maintained
// ===========================================================================
TEST_F(GtbprIntegrationTest, MultipleAggressorsSequential) {
    // o1 = 20 lots (qualifies for priority), o2 = 20 lots
    engine_.new_order(limit(1, Side::Sell, 1000000, 200000, 100));
    engine_.new_order(limit(2, Side::Sell, 1000000, 200000, 200));
    ol_.clear();

    // First aggressor: 10 lots
    engine_.new_order(limit(3, Side::Buy, 1000000, 100000, 300));

    auto fills1 = collect_fills(ol_);
    Quantity o1_fill1 = total_fill_for(fills1, 1);
    Quantity o2_fill1 = total_fill_for(fills1, 2);

    // o1 should get priority fill from first aggressor
    EXPECT_GT(o1_fill1, 0);
    EXPECT_EQ(o1_fill1 + o2_fill1, 100000);

    ol_.clear();

    // Second aggressor: 10 lots
    engine_.new_order(limit(4, Side::Buy, 1000000, 100000, 400));

    auto fills2 = collect_fills(ol_);
    Quantity o1_fill2 = total_fill_for(fills2, 1);
    Quantity o2_fill2 = total_fill_for(fills2, 2);

    // Second aggressor should also match correctly
    EXPECT_EQ(o1_fill2 + o2_fill2, 100000);
    // Total across both: all 200000 from each side accounted for
    EXPECT_EQ(o1_fill1 + o1_fill2 + o2_fill1 + o2_fill2, 200000);
}

// ===========================================================================
// 10. Verify callbacks fired: accepted, partially filled, filled
// ===========================================================================
TEST_F(GtbprIntegrationTest, CallbackOrdering) {
    engine_.new_order(limit(1, Side::Sell, 1000000, 100000, 100));
    ol_.clear();

    // Aggressor buys all 10 lots
    engine_.new_order(limit(2, Side::Buy, 1000000, 100000, 200));

    // Should have: OrderAccepted (for aggressor) + fills
    bool has_accepted = false;
    bool has_fill = false;
    for (const auto& e : ol_.events()) {
        if (std::holds_alternative<OrderAccepted>(e)) has_accepted = true;
        if (std::holds_alternative<OrderFilled>(e)) has_fill = true;
    }
    EXPECT_TRUE(has_accepted);
    EXPECT_TRUE(has_fill);

    // Trade callback should also fire on md listener
    bool has_trade = false;
    for (const auto& e : ml_.events()) {
        if (std::holds_alternative<Trade>(e)) has_trade = true;
    }
    EXPECT_TRUE(has_trade);
}

}  // namespace
}  // namespace ice
}  // namespace exchange
