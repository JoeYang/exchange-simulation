#include "tools/trading_strategy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

#include "gtest/gtest.h"

namespace exchange {
namespace {

// Fixed seed for deterministic tests
constexpr uint32_t kSeed = 42;

// Standard test configuration
ClientState MakeDefaultState() {
    ClientState s;
    s.ref_price = 5000 * PRICE_SCALE;      // 5000.0000
    s.spread = 10 * PRICE_SCALE;            // 10.0000 (5 ticks each side)
    s.max_position = 50 * PRICE_SCALE;      // 50 contracts
    s.lot_size = 1 * PRICE_SCALE;           // 1 contract
    s.tick_size = 1 * PRICE_SCALE;          // 1.0000
    s.next_cl_ord_id = 1;
    s.now = 1'000'000'000'000LL;  // 1000 seconds in nanos
    return s;
}

// ---------------------------------------------------------------------------
// snap_to_tick
// ---------------------------------------------------------------------------

TEST(SnapToTickTest, ExactMultiple) {
    EXPECT_EQ(snap_to_tick(100, 25), 100);
}

TEST(SnapToTickTest, RoundsDown) {
    // 110 / 25 = 4.4 -> 4*25 = 100
    EXPECT_EQ(snap_to_tick(110, 25), 100);
}

TEST(SnapToTickTest, RoundsUp) {
    // 113 / 25 = 4.52 -> 5*25 = 125
    EXPECT_EQ(snap_to_tick(113, 25), 125);
}

TEST(SnapToTickTest, ZeroTickSize) {
    EXPECT_EQ(snap_to_tick(113, 0), 113);
}

TEST(SnapToTickTest, NegativeTickSize) {
    EXPECT_EQ(snap_to_tick(113, -1), 113);
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: produces orders within spread
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, ProducesOrdersWithinSpread) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    // Must produce at least some New orders
    int new_count = 0;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            ++new_count;
            // Price must be within [ref - spread, ref + spread]
            EXPECT_GE(a.price, state.ref_price - state.spread)
                << "Order price below lower bound";
            EXPECT_LE(a.price, state.ref_price + state.spread)
                << "Order price above upper bound";
            // Price must be on tick grid
            EXPECT_EQ(a.price % state.tick_size, 0)
                << "Order price not snapped to tick: " << a.price;
            // Quantity must be lot_size
            EXPECT_EQ(a.qty, state.lot_size);
        }
    }
    EXPECT_GT(new_count, 0) << "Strategy produced no new orders";
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: respects max_position
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, RespectsMaxPosition) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    state.max_position = 1 * PRICE_SCALE;  // max 1 contract
    state.position = 1 * PRICE_SCALE;      // already at max long
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    // Should not produce any Buy orders (would exceed max position)
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            EXPECT_NE(a.side, Side::Buy)
                << "Strategy placed a Buy order when position is at max";
        }
    }
}

TEST(RandomWalkTest, RespectsMaxPositionShort) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    state.max_position = 1 * PRICE_SCALE;
    state.position = -1 * PRICE_SCALE;  // at max short
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            EXPECT_NE(a.side, Side::Sell)
                << "Strategy placed a Sell order when position is at max short";
        }
    }
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: cancels old orders
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, CancelsOldOrders) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    state.now = 10'000'000'000LL;  // 10 seconds

    // Add some old orders (created at t=0, now is 10s -> all are >5s old)
    state.open_orders[100] = OpenOrder{100, Side::Buy, 4995 * PRICE_SCALE,
                                       PRICE_SCALE, 0};
    state.open_orders[101] = OpenOrder{101, Side::Sell, 5005 * PRICE_SCALE,
                                       PRICE_SCALE, 0};
    state.next_cl_ord_id = 200;

    std::mt19937 rng(kSeed);
    auto actions = strategy(state, rng);

    // Should have Cancel or Modify actions targeting the old orders
    int cancel_modify_count = 0;
    std::unordered_set<uint64_t> targeted;
    for (const auto& a : actions) {
        if (a.type == OrderAction::Cancel) {
            targeted.insert(a.cl_ord_id);
            ++cancel_modify_count;
        } else if (a.type == OrderAction::Modify) {
            targeted.insert(a.orig_cl_ord_id);
            ++cancel_modify_count;
        }
    }
    EXPECT_GT(cancel_modify_count, 0) << "No cancel/modify for old orders";
}

TEST(RandomWalkTest, DoesNotCancelFreshOrders) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    state.now = 1'000'000'000LL;  // 1 second

    // Add fresh orders (created at t=0.5s, so only 0.5s old -- below min 1s threshold)
    state.open_orders[100] = OpenOrder{100, Side::Buy, 4995 * PRICE_SCALE,
                                       PRICE_SCALE, 500'000'000LL};
    state.next_cl_ord_id = 200;

    std::mt19937 rng(kSeed);
    auto actions = strategy(state, rng);

    // Should not cancel/modify the fresh order
    for (const auto& a : actions) {
        if (a.type == OrderAction::Cancel) {
            EXPECT_NE(a.cl_ord_id, uint64_t{100})
                << "Cancelled a fresh order";
        }
        if (a.type == OrderAction::Modify) {
            EXPECT_NE(a.orig_cl_ord_id, uint64_t{100})
                << "Modified a fresh order";
        }
    }
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: adapts ref_price toward last fill
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, AdaptsRefPriceTowardFill) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    state.last_fill_price = 5100 * PRICE_SCALE;  // fill above ref
    Price original_ref = state.ref_price;
    std::mt19937 rng(kSeed);

    strategy(state, rng);

    // ref_price should have moved toward last_fill_price
    EXPECT_GT(state.ref_price, original_ref)
        << "ref_price should increase toward fill price above";
    // Expected: 0.9 * 5000 + 0.1 * 5100 = 5010
    Price expected = snap_to_tick(
        static_cast<Price>(0.9 * 5000 * PRICE_SCALE + 0.1 * 5100 * PRICE_SCALE),
        state.tick_size);
    EXPECT_EQ(state.ref_price, expected);
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: unique cl_ord_ids
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, UniqueClOrdIds) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    std::unordered_set<uint64_t> ids;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New || a.type == OrderAction::Modify) {
            EXPECT_TRUE(ids.insert(a.cl_ord_id).second)
                << "Duplicate cl_ord_id: " << a.cl_ord_id;
        }
    }
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: deterministic with same seed
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, DeterministicWithSeed) {
    auto strategy1 = random_walk_strategy();
    auto strategy2 = random_walk_strategy();
    ClientState state1 = MakeDefaultState();
    ClientState state2 = MakeDefaultState();
    std::mt19937 rng1(kSeed);
    std::mt19937 rng2(kSeed);

    auto actions1 = strategy1(state1, rng1);
    auto actions2 = strategy2(state2, rng2);

    ASSERT_EQ(actions1.size(), actions2.size());
    for (size_t i = 0; i < actions1.size(); ++i) {
        EXPECT_EQ(actions1[i].type, actions2[i].type);
        EXPECT_EQ(actions1[i].side, actions2[i].side);
        EXPECT_EQ(actions1[i].price, actions2[i].price);
        EXPECT_EQ(actions1[i].qty, actions2[i].qty);
        EXPECT_EQ(actions1[i].cl_ord_id, actions2[i].cl_ord_id);
    }
}

// ---------------------------------------------------------------------------
// Random-Walk Strategy: buy prices below ref, sell prices above ref
// ---------------------------------------------------------------------------

TEST(RandomWalkTest, BuyBelowRefSellAboveRef) {
    auto strategy = random_walk_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            if (a.side == Side::Buy) {
                EXPECT_LE(a.price, state.ref_price)
                    << "Buy order placed above ref_price";
            } else {
                EXPECT_GE(a.price, state.ref_price)
                    << "Sell order placed below ref_price";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: always has bid and ask
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, AlwaysHasBidAndAsk) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    bool has_bid = false, has_ask = false;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            if (a.side == Side::Buy) has_bid = true;
            if (a.side == Side::Sell) has_ask = true;
        }
    }
    EXPECT_TRUE(has_bid) << "Market maker did not place a bid";
    EXPECT_TRUE(has_ask) << "Market maker did not place an ask";
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: leans away from position
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, LeansAwayFromLongPosition) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    state.position = 10 * PRICE_SCALE;  // long 10 contracts
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    // With long position, lean > 0, so:
    //   bid_target = ref - spread/2 - lean (lower than neutral)
    //   ask_target = ref + spread/2 - lean (lower than neutral)
    // The bid should be lower than the neutral bid
    Price neutral_bid = snap_to_tick(
        state.ref_price - state.spread / 2, state.tick_size);

    for (const auto& a : actions) {
        if (a.type == OrderAction::New && a.side == Side::Buy) {
            EXPECT_LE(a.price, neutral_bid)
                << "Long position: bid should be <= neutral bid";
        }
    }
}

TEST(MarketMakerTest, LeansAwayFromShortPosition) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    state.position = -10 * PRICE_SCALE;  // short 10 contracts
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    // With short position, lean < 0, so:
    //   ask_target = ref + spread/2 - lean (higher than neutral)
    Price neutral_ask = snap_to_tick(
        state.ref_price + state.spread / 2, state.tick_size);

    for (const auto& a : actions) {
        if (a.type == OrderAction::New && a.side == Side::Sell) {
            EXPECT_GE(a.price, neutral_ask)
                << "Short position: ask should be >= neutral ask";
        }
    }
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: empty state produces initial quotes
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, EmptyStateProducesInitialQuotes) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    state.open_orders.clear();
    state.position = 0;
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    int new_buys = 0, new_sells = 0;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            if (a.side == Side::Buy) ++new_buys;
            else ++new_sells;
        }
    }
    EXPECT_EQ(new_buys, 1) << "Expected exactly 1 initial bid";
    EXPECT_EQ(new_sells, 1) << "Expected exactly 1 initial ask";
    // No cancels expected (nothing to cancel)
    for (const auto& a : actions) {
        EXPECT_NE(a.type, OrderAction::Cancel) << "Unexpected cancel on empty state";
    }
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: re-quotes on fill
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, RequotesOnFill) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();

    // First tick: place initial quotes
    std::mt19937 rng(kSeed);
    auto actions1 = strategy(state, rng);

    // Simulate: add the placed orders to open_orders
    for (const auto& a : actions1) {
        if (a.type == OrderAction::New) {
            state.open_orders[a.cl_ord_id] =
                OpenOrder{a.cl_ord_id, a.side, a.price, a.qty, state.now};
        }
    }

    // Simulate a fill: increment fill_count
    state.fill_count = 1;
    state.last_fill_price = state.ref_price;

    auto actions2 = strategy(state, rng);

    // Should cancel existing orders and place new ones
    int cancels = 0, news = 0;
    for (const auto& a : actions2) {
        if (a.type == OrderAction::Cancel) ++cancels;
        if (a.type == OrderAction::New) ++news;
    }
    EXPECT_GT(cancels, 0) << "Should cancel old quotes after fill";
    EXPECT_GT(news, 0) << "Should place new quotes after fill";
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: bid < ask invariant
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, BidBelowAsk) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    Price bid_price = 0, ask_price = 0;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            if (a.side == Side::Buy) bid_price = a.price;
            if (a.side == Side::Sell) ask_price = a.price;
        }
    }
    if (bid_price > 0 && ask_price > 0) {
        EXPECT_LT(bid_price, ask_price)
            << "Market maker bid must be below ask";
    }
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: prices on tick grid
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, PricesOnTickGrid) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            EXPECT_EQ(a.price % state.tick_size, 0)
                << "Price not on tick grid: " << a.price;
        }
    }
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: respects position limits
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, RespectsMaxPositionLong) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    state.max_position = 1 * PRICE_SCALE;
    state.position = 1 * PRICE_SCALE;  // at max long
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    // Should still place an ask (to reduce position) but not a buy
    bool has_new_buy = false;
    bool has_new_sell = false;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            if (a.side == Side::Buy) has_new_buy = true;
            if (a.side == Side::Sell) has_new_sell = true;
        }
    }
    EXPECT_FALSE(has_new_buy) << "Should not buy at max long position";
    EXPECT_TRUE(has_new_sell) << "Should still sell to reduce long position";
}

TEST(MarketMakerTest, RespectsMaxPositionShort) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    state.max_position = 1 * PRICE_SCALE;
    state.position = -1 * PRICE_SCALE;  // at max short
    std::mt19937 rng(kSeed);

    auto actions = strategy(state, rng);

    bool has_new_buy = false;
    bool has_new_sell = false;
    for (const auto& a : actions) {
        if (a.type == OrderAction::New) {
            if (a.side == Side::Buy) has_new_buy = true;
            if (a.side == Side::Sell) has_new_sell = true;
        }
    }
    EXPECT_TRUE(has_new_buy) << "Should still buy to reduce short position";
    EXPECT_FALSE(has_new_sell) << "Should not sell at max short position";
}

// ---------------------------------------------------------------------------
// Market-Maker Strategy: no-op when quotes are already correct
// ---------------------------------------------------------------------------

TEST(MarketMakerTest, NoOpWhenQuotesCorrect) {
    auto strategy = market_maker_strategy();
    ClientState state = MakeDefaultState();
    std::mt19937 rng(kSeed);

    // First tick to establish quotes
    auto actions1 = strategy(state, rng);

    // Place the orders in state
    for (const auto& a : actions1) {
        if (a.type == OrderAction::New) {
            state.open_orders[a.cl_ord_id] =
                OpenOrder{a.cl_ord_id, a.side, a.price, a.qty, state.now};
        }
    }

    // Second tick with no fills, same state -- should produce no actions
    auto actions2 = strategy(state, rng);
    EXPECT_TRUE(actions2.empty())
        << "Market maker should not re-quote when prices are correct and no fills";
}

}  // namespace
}  // namespace exchange
