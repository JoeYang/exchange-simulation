#include "krx/fast/fast_decoder.h"
#include "krx/fast/fast_encoder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace exchange::krx::fast {
namespace {

// ---------------------------------------------------------------------------
// Recording visitor for test verification
// ---------------------------------------------------------------------------

struct RecordingVisitor : public FastDecoderVisitorBase {
    std::vector<FastQuote>    quotes;
    std::vector<FastTrade>    trades;
    std::vector<FastStatus>   statuses;
    std::vector<FastSnapshot> snapshots;

    void on_quote(const FastQuote& q) { quotes.push_back(q); }
    void on_trade(const FastTrade& t) { trades.push_back(t); }
    void on_status(const FastStatus& s) { statuses.push_back(s); }
    void on_snapshot(const FastSnapshot& s) { snapshots.push_back(s); }
};

// ---------------------------------------------------------------------------
// Quote decode
// ---------------------------------------------------------------------------

TEST(FastDecoder, DecodeQuote) {
    FastQuote orig{};
    orig.bid_price = 1005000;
    orig.bid_qty   = 100000;
    orig.ask_price = 1010000;
    orig.ask_qty   = 50000;
    orig.timestamp = 1000000000;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    size_t consumed = decode_message(buf, n, visitor);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(visitor.quotes.size(), 1u);

    const auto& q = visitor.quotes[0];
    EXPECT_EQ(q.bid_price, orig.bid_price);
    EXPECT_EQ(q.bid_qty, orig.bid_qty);
    EXPECT_EQ(q.ask_price, orig.ask_price);
    EXPECT_EQ(q.ask_qty, orig.ask_qty);
    EXPECT_EQ(q.timestamp, orig.timestamp);
}

// ---------------------------------------------------------------------------
// Trade decode
// ---------------------------------------------------------------------------

TEST(FastDecoder, DecodeTrade) {
    FastTrade orig{};
    orig.price          = 2500000;
    orig.quantity        = 10000;
    orig.aggressor_side = 1;
    orig.timestamp      = 999999;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_trade(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    size_t consumed = decode_message(buf, n, visitor);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(visitor.trades.size(), 1u);

    const auto& t = visitor.trades[0];
    EXPECT_EQ(t.price, orig.price);
    EXPECT_EQ(t.quantity, orig.quantity);
    EXPECT_EQ(t.aggressor_side, orig.aggressor_side);
    EXPECT_EQ(t.timestamp, orig.timestamp);
}

// ---------------------------------------------------------------------------
// Status decode
// ---------------------------------------------------------------------------

TEST(FastDecoder, DecodeStatus) {
    FastStatus orig{};
    orig.session_state = static_cast<uint8_t>(SessionState::VolatilityAuction);
    orig.timestamp = 500000;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_status(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    size_t consumed = decode_message(buf, n, visitor);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(visitor.statuses.size(), 1u);

    const auto& s = visitor.statuses[0];
    EXPECT_EQ(s.session_state, orig.session_state);
    EXPECT_EQ(s.timestamp, orig.timestamp);
}

// ---------------------------------------------------------------------------
// Snapshot decode
// ---------------------------------------------------------------------------

TEST(FastDecoder, DecodeSnapshot) {
    FastSnapshot orig{};
    orig.bid_price = 900000;
    orig.bid_qty   = 200000;
    orig.ask_price = 910000;
    orig.ask_qty   = 150000;
    orig.bid_count = 3;
    orig.ask_count = 5;
    orig.timestamp = 1234567890;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_snapshot(buf, sizeof(buf), orig);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    size_t consumed = decode_message(buf, n, visitor);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(visitor.snapshots.size(), 1u);

    const auto& s = visitor.snapshots[0];
    EXPECT_EQ(s.bid_price, orig.bid_price);
    EXPECT_EQ(s.bid_qty, orig.bid_qty);
    EXPECT_EQ(s.ask_price, orig.ask_price);
    EXPECT_EQ(s.ask_qty, orig.ask_qty);
    EXPECT_EQ(s.bid_count, orig.bid_count);
    EXPECT_EQ(s.ask_count, orig.ask_count);
    EXPECT_EQ(s.timestamp, orig.timestamp);
}

// ---------------------------------------------------------------------------
// Multiple message decode
// ---------------------------------------------------------------------------

TEST(FastDecoder, DecodeMultipleMessages) {
    uint8_t buf[512]{};
    size_t offset = 0;

    // Encode a quote
    FastQuote q{};
    q.bid_price = 100;
    q.timestamp = 1;
    size_t n = encode_quote(buf + offset, sizeof(buf) - offset, q);
    ASSERT_GT(n, 0u);
    offset += n;

    // Encode a trade
    FastTrade t{};
    t.price = 200;
    t.quantity = 50;
    t.timestamp = 2;
    n = encode_trade(buf + offset, sizeof(buf) - offset, t);
    ASSERT_GT(n, 0u);
    offset += n;

    // Encode a status
    FastStatus s{};
    s.session_state = 3;
    s.timestamp = 3;
    n = encode_status(buf + offset, sizeof(buf) - offset, s);
    ASSERT_GT(n, 0u);
    offset += n;

    RecordingVisitor visitor;
    size_t consumed = decode_messages(buf, offset, visitor);
    EXPECT_EQ(consumed, offset);
    EXPECT_EQ(visitor.quotes.size(), 1u);
    EXPECT_EQ(visitor.trades.size(), 1u);
    EXPECT_EQ(visitor.statuses.size(), 1u);

    EXPECT_EQ(visitor.quotes[0].bid_price, 100);
    EXPECT_EQ(visitor.trades[0].price, 200);
    EXPECT_EQ(visitor.statuses[0].session_state, 3);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(FastDecoder, EmptyBuffer) {
    RecordingVisitor visitor;
    size_t consumed = decode_message(nullptr, 0, visitor);
    EXPECT_EQ(consumed, 0u);
}

TEST(FastDecoder, TruncatedBuffer) {
    FastQuote q{};
    q.bid_price = 1005000;
    q.timestamp = 42;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), q);
    ASSERT_GT(n, 0u);

    // Feed only half the bytes
    RecordingVisitor visitor;
    size_t consumed = decode_message(buf, n / 2, visitor);
    EXPECT_EQ(consumed, 0u);
    EXPECT_TRUE(visitor.quotes.empty());
}

TEST(FastDecoder, ZeroFieldValues) {
    FastQuote q{};  // all zeros

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), q);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    size_t consumed = decode_message(buf, n, visitor);
    EXPECT_EQ(consumed, n);
    ASSERT_EQ(visitor.quotes.size(), 1u);
    EXPECT_EQ(visitor.quotes[0].bid_price, 0);
    EXPECT_EQ(visitor.quotes[0].bid_qty, 0);
    EXPECT_EQ(visitor.quotes[0].ask_price, 0);
    EXPECT_EQ(visitor.quotes[0].ask_qty, 0);
    EXPECT_EQ(visitor.quotes[0].timestamp, 0);
}

TEST(FastDecoder, NegativePrice) {
    // Edge case: negative prices (e.g., negative interest rates)
    FastQuote q{};
    q.bid_price = -5000;
    q.ask_price = -3000;
    q.timestamp = 42;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), q);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    decode_message(buf, n, visitor);
    ASSERT_EQ(visitor.quotes.size(), 1u);
    EXPECT_EQ(visitor.quotes[0].bid_price, -5000);
    EXPECT_EQ(visitor.quotes[0].ask_price, -3000);
}

TEST(FastDecoder, LargeTimestamp) {
    FastTrade t{};
    t.price = 100;
    t.quantity = 1;
    t.timestamp = 1711612800000000000LL;  // realistic epoch nanos

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_trade(buf, sizeof(buf), t);
    ASSERT_GT(n, 0u);

    RecordingVisitor visitor;
    decode_message(buf, n, visitor);
    ASSERT_EQ(visitor.trades.size(), 1u);
    EXPECT_EQ(visitor.trades[0].timestamp, 1711612800000000000LL);
}

TEST(FastDecoder, NoOpVisitor) {
    // FastDecoderVisitorBase should compile and work as a no-op
    FastQuote q{};
    q.bid_price = 100;

    uint8_t buf[kMaxFastEncodedSize]{};
    size_t n = encode_quote(buf, sizeof(buf), q);
    ASSERT_GT(n, 0u);

    FastDecoderVisitorBase visitor;
    size_t consumed = decode_message(buf, n, visitor);
    EXPECT_EQ(consumed, n);  // should succeed even though visitor does nothing
}

}  // namespace
}  // namespace exchange::krx::fast
