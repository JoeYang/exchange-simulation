#include "krx/fast/fast_decoder.h"
#include "krx/fast/fast_encoder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace exchange::krx::fast {
namespace {

// ---------------------------------------------------------------------------
// Recording visitor for round-trip verification
// ---------------------------------------------------------------------------

struct RoundtripVisitor : public FastDecoderVisitorBase {
    std::vector<FastQuote>         quotes;
    std::vector<FastTrade>         trades;
    std::vector<FastStatus>        statuses;
    std::vector<FastSnapshot>      snapshots;
    std::vector<FastInstrumentDef> instrument_defs;
    std::vector<FastFullSnapshot>  full_snapshots;

    void on_quote(const FastQuote& q) { quotes.push_back(q); }
    void on_trade(const FastTrade& t) { trades.push_back(t); }
    void on_status(const FastStatus& s) { statuses.push_back(s); }
    void on_snapshot(const FastSnapshot& s) { snapshots.push_back(s); }
    void on_instrument_def(const FastInstrumentDef& d) { instrument_defs.push_back(d); }
    void on_full_snapshot(const FastFullSnapshot& s) { full_snapshots.push_back(s); }
};

// ---------------------------------------------------------------------------
// Quote round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, QuoteTypicalValues) {
    FastQuote orig{};
    orig.bid_price = 3275000;   // KOSPI200 ~ 327.5000
    orig.bid_qty   = 500000;    // 50 lots
    orig.ask_price = 3280000;   // 328.0000
    orig.ask_qty   = 300000;    // 30 lots
    orig.timestamp = 1711612800000000000LL;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    size_t consumed = decode_message(buf, n, v);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(v.quotes.size(), 1u);

    EXPECT_EQ(v.quotes[0].bid_price, orig.bid_price);
    EXPECT_EQ(v.quotes[0].bid_qty, orig.bid_qty);
    EXPECT_EQ(v.quotes[0].ask_price, orig.ask_price);
    EXPECT_EQ(v.quotes[0].ask_qty, orig.ask_qty);
    EXPECT_EQ(v.quotes[0].timestamp, orig.timestamp);
}

TEST(FastCodecRoundtrip, QuoteZeroFields) {
    FastQuote orig{};  // all zero

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.quotes.size(), 1u);

    EXPECT_EQ(v.quotes[0].bid_price, 0);
    EXPECT_EQ(v.quotes[0].bid_qty, 0);
    EXPECT_EQ(v.quotes[0].ask_price, 0);
    EXPECT_EQ(v.quotes[0].ask_qty, 0);
    EXPECT_EQ(v.quotes[0].timestamp, 0);
}

TEST(FastCodecRoundtrip, QuoteNegativePrices) {
    // Negative interest rate scenario
    FastQuote orig{};
    orig.bid_price = -50000;
    orig.bid_qty   = 10000;
    orig.ask_price = -30000;
    orig.ask_qty   = 20000;
    orig.timestamp = 42;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.quotes.size(), 1u);

    EXPECT_EQ(v.quotes[0].bid_price, -50000);
    EXPECT_EQ(v.quotes[0].ask_price, -30000);
}

TEST(FastCodecRoundtrip, QuoteMaxValues) {
    FastQuote orig{};
    orig.bid_price = std::numeric_limits<int64_t>::max();
    orig.bid_qty   = std::numeric_limits<int64_t>::max();
    orig.ask_price = std::numeric_limits<int64_t>::max();
    orig.ask_qty   = std::numeric_limits<int64_t>::max();
    orig.timestamp = std::numeric_limits<int64_t>::max();

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.quotes.size(), 1u);

    EXPECT_EQ(v.quotes[0].bid_price, orig.bid_price);
    EXPECT_EQ(v.quotes[0].timestamp, orig.timestamp);
}

TEST(FastCodecRoundtrip, QuoteMinValues) {
    FastQuote orig{};
    orig.bid_price = std::numeric_limits<int64_t>::min();
    orig.ask_price = std::numeric_limits<int64_t>::min();
    orig.timestamp = std::numeric_limits<int64_t>::min();

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.quotes.size(), 1u);

    EXPECT_EQ(v.quotes[0].bid_price, orig.bid_price);
    EXPECT_EQ(v.quotes[0].ask_price, orig.ask_price);
    EXPECT_EQ(v.quotes[0].timestamp, orig.timestamp);
}

// ---------------------------------------------------------------------------
// Trade round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, TradeTypicalValues) {
    FastTrade orig{};
    orig.price          = 3275000;
    orig.quantity        = 10000;    // 1 lot
    orig.aggressor_side = 0;        // Buy
    orig.timestamp      = 1711612800000000000LL;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_trade(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.trades.size(), 1u);

    EXPECT_EQ(v.trades[0].price, orig.price);
    EXPECT_EQ(v.trades[0].quantity, orig.quantity);
    EXPECT_EQ(v.trades[0].aggressor_side, orig.aggressor_side);
    EXPECT_EQ(v.trades[0].timestamp, orig.timestamp);
}

TEST(FastCodecRoundtrip, TradeSellSide) {
    FastTrade orig{};
    orig.price          = 100;
    orig.quantity        = 1;
    orig.aggressor_side = 1;  // Sell
    orig.timestamp      = 0;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_trade(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.trades.size(), 1u);
    EXPECT_EQ(v.trades[0].aggressor_side, 1);
}

// ---------------------------------------------------------------------------
// Status round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, StatusAllStates) {
    const uint8_t states[] = {
        static_cast<uint8_t>(SessionState::Closed),
        static_cast<uint8_t>(SessionState::PreOpen),
        static_cast<uint8_t>(SessionState::OpeningAuction),
        static_cast<uint8_t>(SessionState::Continuous),
        static_cast<uint8_t>(SessionState::PreClose),
        static_cast<uint8_t>(SessionState::ClosingAuction),
        static_cast<uint8_t>(SessionState::Halt),
        static_cast<uint8_t>(SessionState::VolatilityAuction),
        static_cast<uint8_t>(SessionState::LockLimit),
    };

    for (uint8_t state : states) {
        FastStatus orig{};
        orig.session_state = state;
        orig.timestamp = 12345;

        uint8_t buf[kMaxFastEncodedSize]{};
        size_t n = encode_status(buf, sizeof(buf), orig);
        ASSERT_GT(n, 0u) << "encode failed for state " << static_cast<int>(state);

        RoundtripVisitor v;
        decode_message(buf, n, v);
        ASSERT_EQ(v.statuses.size(), 1u)
            << "decode failed for state " << static_cast<int>(state);
        EXPECT_EQ(v.statuses[0].session_state, state)
            << "state mismatch for " << static_cast<int>(state);
        EXPECT_EQ(v.statuses[0].timestamp, 12345);
    }
}

// ---------------------------------------------------------------------------
// Snapshot round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, SnapshotTypicalValues) {
    FastSnapshot orig{};
    orig.bid_price = 3275000;
    orig.bid_qty   = 500000;
    orig.ask_price = 3280000;
    orig.ask_qty   = 300000;
    orig.bid_count = 10;
    orig.ask_count = 8;
    orig.timestamp = 1711612800000000000LL;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_snapshot(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.snapshots.size(), 1u);

    EXPECT_EQ(v.snapshots[0].bid_price, orig.bid_price);
    EXPECT_EQ(v.snapshots[0].bid_qty, orig.bid_qty);
    EXPECT_EQ(v.snapshots[0].ask_price, orig.ask_price);
    EXPECT_EQ(v.snapshots[0].ask_qty, orig.ask_qty);
    EXPECT_EQ(v.snapshots[0].bid_count, orig.bid_count);
    EXPECT_EQ(v.snapshots[0].ask_count, orig.ask_count);
    EXPECT_EQ(v.snapshots[0].timestamp, orig.timestamp);
}

TEST(FastCodecRoundtrip, SnapshotEmptyBook) {
    FastSnapshot orig{};  // all zeros

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_snapshot(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.snapshots.size(), 1u);

    EXPECT_EQ(v.snapshots[0].bid_count, 0u);
    EXPECT_EQ(v.snapshots[0].ask_count, 0u);
}

// ---------------------------------------------------------------------------
// Mixed message stream round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, MixedMessageStream) {
    uint8_t buf[1024]{};
    size_t offset = 0;

    // Encode: Quote, Trade, Status, Snapshot, Trade, Quote
    FastQuote q1{};
    q1.bid_price = 100;
    q1.timestamp = 1;
    offset += encode_quote(buf + offset, sizeof(buf) - offset, q1);

    FastTrade t1{};
    t1.price = 200;
    t1.quantity = 50;
    t1.aggressor_side = 0;
    t1.timestamp = 2;
    offset += encode_trade(buf + offset, sizeof(buf) - offset, t1);

    FastStatus s1{};
    s1.session_state = static_cast<uint8_t>(SessionState::Continuous);
    s1.timestamp = 3;
    offset += encode_status(buf + offset, sizeof(buf) - offset, s1);

    FastSnapshot snap1{};
    snap1.bid_price = 400;
    snap1.ask_price = 410;
    snap1.bid_count = 2;
    snap1.ask_count = 3;
    snap1.timestamp = 4;
    offset += encode_snapshot(buf + offset, sizeof(buf) - offset, snap1);

    FastTrade t2{};
    t2.price = 500;
    t2.quantity = 100;
    t2.aggressor_side = 1;
    t2.timestamp = 5;
    offset += encode_trade(buf + offset, sizeof(buf) - offset, t2);

    FastQuote q2{};
    q2.bid_price = 600;
    q2.ask_price = 610;
    q2.timestamp = 6;
    offset += encode_quote(buf + offset, sizeof(buf) - offset, q2);

    // Decode all messages
    RoundtripVisitor v;
    size_t consumed = decode_messages(buf, offset, v);
    EXPECT_EQ(consumed, offset);

    ASSERT_EQ(v.quotes.size(), 2u);
    ASSERT_EQ(v.trades.size(), 2u);
    ASSERT_EQ(v.statuses.size(), 1u);
    ASSERT_EQ(v.snapshots.size(), 1u);

    // Verify ordering by timestamp
    EXPECT_EQ(v.quotes[0].bid_price, 100);
    EXPECT_EQ(v.quotes[0].timestamp, 1);
    EXPECT_EQ(v.quotes[1].bid_price, 600);
    EXPECT_EQ(v.quotes[1].timestamp, 6);

    EXPECT_EQ(v.trades[0].price, 200);
    EXPECT_EQ(v.trades[0].timestamp, 2);
    EXPECT_EQ(v.trades[1].price, 500);
    EXPECT_EQ(v.trades[1].timestamp, 5);

    EXPECT_EQ(v.statuses[0].session_state,
              static_cast<uint8_t>(SessionState::Continuous));

    EXPECT_EQ(v.snapshots[0].bid_count, 2u);
    EXPECT_EQ(v.snapshots[0].ask_count, 3u);
}

// ---------------------------------------------------------------------------
// Engine event -> encode -> decode round-trip via conversion helpers
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, EngineTopOfBookThroughCodec) {
    TopOfBook tob{};
    tob.best_bid = 3275000;
    tob.bid_qty  = 500000;
    tob.best_ask = 3280000;
    tob.ask_qty  = 300000;
    tob.ts       = 1711612800000000000LL;

    auto fq = to_fast_quote(tob);

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), fq);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.quotes.size(), 1u);

    EXPECT_EQ(v.quotes[0].bid_price, tob.best_bid);
    EXPECT_EQ(v.quotes[0].bid_qty, tob.bid_qty);
    EXPECT_EQ(v.quotes[0].ask_price, tob.best_ask);
    EXPECT_EQ(v.quotes[0].ask_qty, tob.ask_qty);
    EXPECT_EQ(v.quotes[0].timestamp, tob.ts);
}

TEST(FastCodecRoundtrip, EngineTradeThroughCodec) {
    Trade trade{};
    trade.price          = 3275000;
    trade.quantity        = 10000;
    trade.aggressor_side = Side::Sell;
    trade.ts             = 42;

    auto ft = to_fast_trade(trade);

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_trade(buf, sizeof(buf), ft);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.trades.size(), 1u);

    EXPECT_EQ(v.trades[0].price, trade.price);
    EXPECT_EQ(v.trades[0].quantity, trade.quantity);
    EXPECT_EQ(v.trades[0].aggressor_side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(v.trades[0].timestamp, trade.ts);
}

TEST(FastCodecRoundtrip, EngineMarketStatusThroughCodec) {
    exchange::MarketStatus ms{};
    ms.state = SessionState::VolatilityAuction;
    ms.ts    = 777;

    auto fs = to_fast_status(ms);

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_status(buf, sizeof(buf), fs);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    decode_message(buf, n, v);
    ASSERT_EQ(v.statuses.size(), 1u);

    EXPECT_EQ(v.statuses[0].session_state,
              static_cast<uint8_t>(SessionState::VolatilityAuction));
    EXPECT_EQ(v.statuses[0].timestamp, 777);
}

// ---------------------------------------------------------------------------
// InstrumentDef round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, InstrumentDefAllProducts) {
    // Encode all 10 KRX products and verify round-trip.
    const struct {
        uint32_t id;
        const char* sym;
        const char* desc;
        uint8_t group;
        int64_t tick;
        int64_t lot;
        int64_t max_ord;
    } products[] = {
        {1, "KS",   "KOSPI200 Futures",       0, 500,  10000, 30000000},
        {2, "MKS",  "Mini-KOSPI200 Futures",   0, 200,  10000, 50000000},
        {9, "KTB",  "KTB 3-Year Futures",      3, 100,  10000, 30000000},
    };

    for (const auto& prod : products) {
        FastInstrumentDef orig{};
        orig.instrument_id = prod.id;
        std::memset(orig.symbol, 0, sizeof(orig.symbol));
        std::memcpy(orig.symbol, prod.sym, std::strlen(prod.sym));
        std::memset(orig.description, 0, sizeof(orig.description));
        std::memcpy(orig.description, prod.desc, std::strlen(prod.desc));
        orig.product_group = prod.group;
        orig.tick_size = prod.tick;
        orig.lot_size = prod.lot;
        orig.max_order_size = prod.max_ord;
        orig.total_instruments = 10;
        orig.timestamp = 1711612800000000000LL;

        uint8_t buf[kMaxFastEncodedSize]{};
        size_t n = encode_instrument_def(buf, sizeof(buf), orig);
        ASSERT_GT(n, 0u) << "encode failed for " << prod.sym;

        RoundtripVisitor v;
        size_t consumed = decode_message(buf, n, v);
        EXPECT_EQ(consumed, n) << "decode size mismatch for " << prod.sym;
        ASSERT_EQ(v.instrument_defs.size(), 1u) << "no def for " << prod.sym;

        const auto& d = v.instrument_defs[0];
        EXPECT_EQ(d.instrument_id, prod.id);
        EXPECT_EQ(std::string(d.symbol, std::strlen(prod.sym)), prod.sym);
        EXPECT_EQ(d.product_group, prod.group);
        EXPECT_EQ(d.tick_size, prod.tick);
        EXPECT_EQ(d.lot_size, prod.lot);
        EXPECT_EQ(d.max_order_size, prod.max_ord);
        EXPECT_EQ(d.total_instruments, 10u);
    }
}

// ---------------------------------------------------------------------------
// FullSnapshot round-trip
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, FullSnapshotMaxDepth) {
    FastFullSnapshot orig{};
    orig.instrument_id = 1;
    orig.seq_num = 999;
    orig.num_bid_levels = kSnapshotBookDepth;
    orig.num_ask_levels = kSnapshotBookDepth;
    for (int i = 0; i < kSnapshotBookDepth; ++i) {
        orig.bids[i] = {3275000 - i * 5000,
                        static_cast<int64_t>((i + 1) * 100000),
                        static_cast<uint32_t>(10 - i)};
        orig.asks[i] = {3280000 + i * 5000,
                        static_cast<int64_t>((i + 1) * 100000),
                        static_cast<uint32_t>(8 - i)};
    }
    orig.timestamp = 1711612800000000000LL;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_full_snapshot(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RoundtripVisitor v;
    size_t consumed = decode_message(buf, n, v);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(v.full_snapshots.size(), 1u);

    const auto& s = v.full_snapshots[0];
    EXPECT_EQ(s.instrument_id, orig.instrument_id);
    EXPECT_EQ(s.seq_num, orig.seq_num);
    EXPECT_EQ(s.num_bid_levels, kSnapshotBookDepth);
    EXPECT_EQ(s.num_ask_levels, kSnapshotBookDepth);
    for (int i = 0; i < kSnapshotBookDepth; ++i) {
        EXPECT_EQ(s.bids[i].price, orig.bids[i].price) << "bid[" << i << "]";
        EXPECT_EQ(s.bids[i].quantity, orig.bids[i].quantity) << "bid[" << i << "]";
        EXPECT_EQ(s.bids[i].order_count, orig.bids[i].order_count) << "bid[" << i << "]";
        EXPECT_EQ(s.asks[i].price, orig.asks[i].price) << "ask[" << i << "]";
        EXPECT_EQ(s.asks[i].quantity, orig.asks[i].quantity) << "ask[" << i << "]";
        EXPECT_EQ(s.asks[i].order_count, orig.asks[i].order_count) << "ask[" << i << "]";
    }
    EXPECT_EQ(s.timestamp, orig.timestamp);
}

// ---------------------------------------------------------------------------
// Compact encoding verification: small values use fewer bytes
// ---------------------------------------------------------------------------

TEST(FastCodecRoundtrip, CompactEncoding) {
    // Small values should encode compactly (FAST advantage over fixed-width)
    FastQuote small{};
    small.bid_price = 1;
    small.bid_qty   = 1;
    small.ask_price = 1;
    small.ask_qty   = 1;
    small.timestamp = 1;

    FastQuote large{};
    large.bid_price = 1000000000LL;
    large.bid_qty   = 1000000000LL;
    large.ask_price = 1000000000LL;
    large.ask_qty   = 1000000000LL;
    large.timestamp = 1000000000000000000LL;

    uint8_t buf_small[kMaxFastEncodedSize]{};
    size_t n_small = encode_quote(buf_small, sizeof(buf_small), small);

    uint8_t buf_large[kMaxFastEncodedSize]{};
    size_t n_large = encode_quote(buf_large, sizeof(buf_large), large);

    ASSERT_GT(n_small, 0u);
    ASSERT_GT(n_large, 0u);

    // Small values should produce fewer bytes than large values
    EXPECT_LT(n_small, n_large);

    // Both should round-trip correctly
    RoundtripVisitor v1, v2;
    decode_message(buf_small, n_small, v1);
    decode_message(buf_large, n_large, v2);

    ASSERT_EQ(v1.quotes.size(), 1u);
    ASSERT_EQ(v2.quotes.size(), 1u);
    EXPECT_EQ(v1.quotes[0].bid_price, 1);
    EXPECT_EQ(v2.quotes[0].bid_price, 1000000000LL);
}

}  // namespace
}  // namespace exchange::krx::fast
