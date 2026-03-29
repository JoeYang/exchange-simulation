#include "krx/fast/fast_encoder.h"
#include "krx/fast/fast_types.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace exchange::krx::fast {
namespace {

// ---------------------------------------------------------------------------
// Helper: decode template ID from encoded buffer.
// Reads PMAP, then template ID. Returns remaining buffer pointer.
// ---------------------------------------------------------------------------

const uint8_t* read_header(
    const uint8_t* buf, size_t len, TemplateId& tid)
{
    PresenceMap pmap{};
    const uint8_t* p = decode_pmap(buf, len, pmap);
    if (!p) return nullptr;
    size_t remaining = len - static_cast<size_t>(p - buf);

    uint64_t raw_tid = 0;
    p = decode_u64(p, remaining, raw_tid);
    if (!p) return nullptr;
    tid = static_cast<TemplateId>(raw_tid);
    return p;
}

// ---------------------------------------------------------------------------
// Quote encoding
// ---------------------------------------------------------------------------

TEST(FastEncoder, QuoteBasic) {
    FastQuote msg{};
    msg.bid_price = 1005000;   // 100.5000 in fixed-point
    msg.bid_qty   = 100000;    // 10.0000
    msg.ask_price = 1010000;   // 101.0000
    msg.ask_qty   = 50000;     // 5.0000
    msg.timestamp = 1000000000;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    // Verify template ID
    TemplateId tid{};
    const uint8_t* p = read_header(buf, n, tid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tid, TemplateId::Quote);

    // Decode fields in order
    int64_t bid_price = 0, bid_qty = 0, ask_price = 0, ask_qty = 0, ts = 0;
    size_t remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, bid_price);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, bid_qty);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ask_price);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ask_qty);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ts);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(bid_price, 1005000);
    EXPECT_EQ(bid_qty, 100000);
    EXPECT_EQ(ask_price, 1010000);
    EXPECT_EQ(ask_qty, 50000);
    EXPECT_EQ(ts, 1000000000);
}

TEST(FastEncoder, QuoteZeroPrices) {
    FastQuote msg{};  // all zeros

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    TemplateId tid{};
    read_header(buf, n, tid);
    EXPECT_EQ(tid, TemplateId::Quote);
}

// ---------------------------------------------------------------------------
// Trade encoding
// ---------------------------------------------------------------------------

TEST(FastEncoder, TradeBasic) {
    FastTrade msg{};
    msg.price          = 2500000;  // 250.0000
    msg.quantity        = 10000;    // 1.0000
    msg.aggressor_side = 1;        // Sell
    msg.timestamp      = 999999;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_trade(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    TemplateId tid{};
    const uint8_t* p = read_header(buf, n, tid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tid, TemplateId::Trade);

    int64_t price = 0, qty = 0, ts = 0;
    uint64_t side = 0;
    size_t remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, price);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, qty);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_u64(p, remaining, side);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ts);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(price, 2500000);
    EXPECT_EQ(qty, 10000);
    EXPECT_EQ(side, 1u);
    EXPECT_EQ(ts, 999999);
}

// ---------------------------------------------------------------------------
// Status encoding
// ---------------------------------------------------------------------------

TEST(FastEncoder, StatusBasic) {
    FastStatus msg{};
    msg.session_state = static_cast<uint8_t>(SessionState::Continuous);
    msg.timestamp = 500000;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_status(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    TemplateId tid{};
    const uint8_t* p = read_header(buf, n, tid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tid, TemplateId::Status);

    uint64_t state = 0;
    int64_t ts = 0;
    size_t remaining = n - static_cast<size_t>(p - buf);

    p = decode_u64(p, remaining, state);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ts);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(state, static_cast<uint64_t>(SessionState::Continuous));
    EXPECT_EQ(ts, 500000);
}

// ---------------------------------------------------------------------------
// Snapshot encoding
// ---------------------------------------------------------------------------

TEST(FastEncoder, SnapshotBasic) {
    FastSnapshot msg{};
    msg.bid_price = 900000;
    msg.bid_qty   = 200000;
    msg.ask_price = 910000;
    msg.ask_qty   = 150000;
    msg.bid_count = 3;
    msg.ask_count = 5;
    msg.timestamp = 1234567890;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_snapshot(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    TemplateId tid{};
    const uint8_t* p = read_header(buf, n, tid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tid, TemplateId::Snapshot);

    int64_t bp = 0, bq = 0, ap = 0, aq = 0, ts = 0;
    uint64_t bc = 0, ac = 0;
    size_t remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, bp);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, bq);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ap);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, aq);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_u64(p, remaining, bc);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_u64(p, remaining, ac);
    ASSERT_NE(p, nullptr);
    remaining = n - static_cast<size_t>(p - buf);

    p = decode_i64(p, remaining, ts);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(bp, 900000);
    EXPECT_EQ(bq, 200000);
    EXPECT_EQ(ap, 910000);
    EXPECT_EQ(aq, 150000);
    EXPECT_EQ(bc, 3u);
    EXPECT_EQ(ac, 5u);
    EXPECT_EQ(ts, 1234567890);
}

// ---------------------------------------------------------------------------
// InstrumentDef encoding
// ---------------------------------------------------------------------------

TEST(FastEncoder, InstrumentDefBasic) {
    FastInstrumentDef msg{};
    msg.instrument_id = 1;
    std::memcpy(msg.symbol, "KS\0\0\0\0\0\0", 8);
    std::memcpy(msg.description, "KOSPI200 Futures", 16);
    msg.product_group = 0;  // Futures
    msg.tick_size = 500;
    msg.lot_size = 10000;
    msg.max_order_size = 30000000;
    msg.total_instruments = 10;
    msg.timestamp = 1711612800000000000LL;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_instrument_def(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    // Verify template ID
    TemplateId tid{};
    const uint8_t* p = read_header(buf, n, tid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tid, TemplateId::InstrumentDef);

    // Decode fields in order
    size_t remaining = n - static_cast<size_t>(p - buf);

    uint64_t instrument_id = 0;
    p = decode_u64(p, remaining, instrument_id);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(instrument_id, 1u);
    remaining = n - static_cast<size_t>(p - buf);

    // symbol: 8 raw bytes
    char symbol[8]{};
    p = decode_bytes(p, remaining, symbol, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(std::string(symbol, 2), "KS");
    remaining = n - static_cast<size_t>(p - buf);

    // description: 32 raw bytes
    char desc[32]{};
    p = decode_bytes(p, remaining, desc, 32);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(std::string(desc, 16), "KOSPI200 Futures");
    remaining = n - static_cast<size_t>(p - buf);

    uint64_t pg = 0;
    p = decode_u64(p, remaining, pg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pg, 0u);
    remaining = n - static_cast<size_t>(p - buf);

    int64_t tick = 0;
    p = decode_i64(p, remaining, tick);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tick, 500);
    remaining = n - static_cast<size_t>(p - buf);

    int64_t lot = 0;
    p = decode_i64(p, remaining, lot);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(lot, 10000);
    remaining = n - static_cast<size_t>(p - buf);

    int64_t max_ord = 0;
    p = decode_i64(p, remaining, max_ord);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(max_ord, 30000000);
    remaining = n - static_cast<size_t>(p - buf);

    uint64_t total = 0;
    p = decode_u64(p, remaining, total);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(total, 10u);
    remaining = n - static_cast<size_t>(p - buf);

    int64_t ts = 0;
    p = decode_i64(p, remaining, ts);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(ts, 1711612800000000000LL);
}

// ---------------------------------------------------------------------------
// FullSnapshot encoding
// ---------------------------------------------------------------------------

TEST(FastEncoder, FullSnapshotBasic) {
    FastFullSnapshot msg{};
    msg.instrument_id = 1;
    msg.seq_num = 42;
    msg.num_bid_levels = 3;
    msg.num_ask_levels = 2;
    msg.bids[0] = {3275000, 500000, 10};
    msg.bids[1] = {3270000, 300000, 5};
    msg.bids[2] = {3265000, 100000, 2};
    msg.asks[0] = {3280000, 400000, 8};
    msg.asks[1] = {3285000, 200000, 3};
    msg.timestamp = 1711612800000000000LL;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_full_snapshot(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    // Verify template ID
    TemplateId tid{};
    const uint8_t* p = read_header(buf, n, tid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(tid, TemplateId::FullSnapshot);
}

TEST(FastEncoder, FullSnapshotEmptyBook) {
    FastFullSnapshot msg{};
    msg.instrument_id = 5;
    msg.seq_num = 1;
    msg.num_bid_levels = 0;
    msg.num_ask_levels = 0;
    msg.timestamp = 100;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_full_snapshot(buf, sizeof(buf), msg);
    ASSERT_GT(n, 0u);

    TemplateId tid{};
    read_header(buf, n, tid);
    EXPECT_EQ(tid, TemplateId::FullSnapshot);
}

TEST(FastEncoder, FullSnapshotBufferTooSmall) {
    FastFullSnapshot msg{};
    msg.instrument_id = 1;
    msg.num_bid_levels = 5;
    msg.num_ask_levels = 5;

    uint8_t buf[4]{};
    size_t n = encode_full_snapshot(buf, sizeof(buf), msg);
    EXPECT_EQ(n, 0u);
}

TEST(FastEncoder, InstrumentDefBufferTooSmall) {
    FastInstrumentDef msg{};
    msg.instrument_id = 1;
    msg.tick_size = 500;

    uint8_t buf[4]{};
    size_t n = encode_instrument_def(buf, sizeof(buf), msg);
    EXPECT_EQ(n, 0u);
}

// ---------------------------------------------------------------------------
// Engine event conversion helpers
// ---------------------------------------------------------------------------

TEST(FastEncoder, ToFastQuoteFromTopOfBook) {
    TopOfBook tob{};
    tob.best_bid = 1005000;
    tob.bid_qty  = 100000;
    tob.best_ask = 1010000;
    tob.ask_qty  = 50000;
    tob.ts       = 42;

    auto q = to_fast_quote(tob);
    EXPECT_EQ(q.bid_price, 1005000);
    EXPECT_EQ(q.bid_qty, 100000);
    EXPECT_EQ(q.ask_price, 1010000);
    EXPECT_EQ(q.ask_qty, 50000);
    EXPECT_EQ(q.timestamp, 42);
}

TEST(FastEncoder, ToFastTradeFromTrade) {
    Trade t{};
    t.price          = 2500000;
    t.quantity        = 10000;
    t.aggressor_side = Side::Sell;
    t.ts             = 99;

    auto ft = to_fast_trade(t);
    EXPECT_EQ(ft.price, 2500000);
    EXPECT_EQ(ft.quantity, 10000);
    EXPECT_EQ(ft.aggressor_side, static_cast<uint8_t>(Side::Sell));
    EXPECT_EQ(ft.timestamp, 99);
}

TEST(FastEncoder, ToFastStatusFromMarketStatus) {
    exchange::MarketStatus ms{};
    ms.state = SessionState::Halt;
    ms.ts    = 777;

    auto fs = to_fast_status(ms);
    EXPECT_EQ(fs.session_state, static_cast<uint8_t>(SessionState::Halt));
    EXPECT_EQ(fs.timestamp, 777);
}

TEST(FastEncoder, ToFastSnapshotFromTopOfBook) {
    TopOfBook tob{};
    tob.best_bid = 500000;
    tob.bid_qty  = 100000;
    tob.best_ask = 510000;
    tob.ask_qty  = 200000;
    tob.ts       = 123;

    auto snap = to_fast_snapshot(tob);
    EXPECT_EQ(snap.bid_price, 500000);
    EXPECT_EQ(snap.bid_qty, 100000);
    EXPECT_EQ(snap.ask_price, 510000);
    EXPECT_EQ(snap.ask_qty, 200000);
    EXPECT_EQ(snap.bid_count, 1u);
    EXPECT_EQ(snap.ask_count, 1u);
    EXPECT_EQ(snap.timestamp, 123);
}

TEST(FastEncoder, ToFastSnapshotEmptyBook) {
    TopOfBook tob{};  // all zeros
    auto snap = to_fast_snapshot(tob);
    EXPECT_EQ(snap.bid_count, 0u);
    EXPECT_EQ(snap.ask_count, 0u);
}

// ---------------------------------------------------------------------------
// Buffer overflow protection
// ---------------------------------------------------------------------------

TEST(FastEncoder, QuoteBufferTooSmall) {
    FastQuote msg{};
    msg.bid_price = 1005000;
    msg.timestamp = 1000000000;

    uint8_t buf[4]{};  // way too small
    size_t n = encode_quote(buf, sizeof(buf), msg);
    EXPECT_EQ(n, 0u);
}

TEST(FastEncoder, TradeBufferTooSmall) {
    FastTrade msg{};
    msg.price = 2500000;

    uint8_t buf[4]{};
    size_t n = encode_trade(buf, sizeof(buf), msg);
    EXPECT_EQ(n, 0u);
}

}  // namespace
}  // namespace exchange::krx::fast
