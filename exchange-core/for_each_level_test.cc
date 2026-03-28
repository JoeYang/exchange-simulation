#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>
#include <vector>

namespace exchange {
namespace {

// Minimal CRTP exchange for testing.
class TestExchange
    : public MatchingEngine<TestExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<TestExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
};

class ForEachLevelTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};
    TestExchange engine_{config_, order_listener_, md_listener_};

    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id = 1,
                            .side = side,
                            .type = OrderType::Limit,
                            .tif = TimeInForce::GTC,
                            .price = price,
                            .quantity = qty,
                            .stop_price = 0,
                            .timestamp = 1000,
                            .gtd_expiry = 0};
    }
};

TEST_F(ForEachLevelTest, EmptyBookYieldsNoCallbacks) {
    int count = 0;
    engine_.for_each_level(Side::Buy, [&](Price, Quantity, uint32_t) {
        ++count;
    });
    engine_.for_each_level(Side::Sell, [&](Price, Quantity, uint32_t) {
        ++count;
    });
    EXPECT_EQ(count, 0);
}

TEST_F(ForEachLevelTest, BidsDescendingOrder) {
    // Insert 3 bid levels at different prices.
    engine_.new_order(make_limit(1, Side::Buy, 1000, 10000));
    engine_.new_order(make_limit(2, Side::Buy, 1200, 20000));
    engine_.new_order(make_limit(3, Side::Buy, 1100, 30000));

    std::vector<std::tuple<Price, Quantity, uint32_t>> levels;
    engine_.for_each_level(Side::Buy, [&](Price p, Quantity q, uint32_t n) {
        levels.emplace_back(p, q, n);
    });

    ASSERT_EQ(levels.size(), 3u);
    // Best bid first (highest price), descending.
    EXPECT_EQ(std::get<0>(levels[0]), 1200);
    EXPECT_EQ(std::get<1>(levels[0]), 20000);
    EXPECT_EQ(std::get<2>(levels[0]), 1u);

    EXPECT_EQ(std::get<0>(levels[1]), 1100);
    EXPECT_EQ(std::get<1>(levels[1]), 30000);

    EXPECT_EQ(std::get<0>(levels[2]), 1000);
    EXPECT_EQ(std::get<1>(levels[2]), 10000);
}

TEST_F(ForEachLevelTest, AsksAscendingOrder) {
    engine_.new_order(make_limit(1, Side::Sell, 1300, 10000));
    engine_.new_order(make_limit(2, Side::Sell, 1100, 20000));
    engine_.new_order(make_limit(3, Side::Sell, 1200, 30000));

    std::vector<std::tuple<Price, Quantity, uint32_t>> levels;
    engine_.for_each_level(Side::Sell, [&](Price p, Quantity q, uint32_t n) {
        levels.emplace_back(p, q, n);
    });

    ASSERT_EQ(levels.size(), 3u);
    // Best ask first (lowest price), ascending.
    EXPECT_EQ(std::get<0>(levels[0]), 1100);
    EXPECT_EQ(std::get<1>(levels[0]), 20000);

    EXPECT_EQ(std::get<0>(levels[1]), 1200);
    EXPECT_EQ(std::get<1>(levels[1]), 30000);

    EXPECT_EQ(std::get<0>(levels[2]), 1300);
    EXPECT_EQ(std::get<1>(levels[2]), 10000);
}

TEST_F(ForEachLevelTest, MultipleOrdersOnSameLevel) {
    engine_.new_order(make_limit(1, Side::Buy, 1000, 10000));
    engine_.new_order(make_limit(2, Side::Buy, 1000, 20000));

    std::vector<std::tuple<Price, Quantity, uint32_t>> levels;
    engine_.for_each_level(Side::Buy, [&](Price p, Quantity q, uint32_t n) {
        levels.emplace_back(p, q, n);
    });

    ASSERT_EQ(levels.size(), 1u);
    EXPECT_EQ(std::get<0>(levels[0]), 1000);
    EXPECT_EQ(std::get<1>(levels[0]), 30000);  // aggregated qty
    EXPECT_EQ(std::get<2>(levels[0]), 2u);     // 2 orders
}

}  // namespace
}  // namespace exchange
