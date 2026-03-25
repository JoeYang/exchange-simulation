#include "cme/codec/mdp3_encoder.h"

#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace exchange::cme::sbe::mdp3 {
namespace {

// Shared context for all tests.
Mdp3EncodeContext MakeCtx() {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 12345;
    ctx.rpt_seq = 0;
    std::memcpy(ctx.security_group, "ES    ", 6);
    std::memcpy(ctx.asset, "ES    ", 6);
    return ctx;
}

// Decode helpers — read wire bytes back into structs for verification.
struct DecodedHeader {
    MessageHeader hdr;
    const char* body;
};

DecodedHeader decode_header(const char* buf) {
    DecodedHeader d{};
    d.body = MessageHeader::decode_from(buf, d.hdr);
    return d;
}

// ---------------------------------------------------------------------------
// DepthUpdate encoding
// ---------------------------------------------------------------------------

TEST(Mdp3EncoderTest, EncodeDepthUpdate) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    DepthUpdate evt{};
    evt.side = Side::Buy;
    evt.price = 45002500;  // 4500.2500 in engine scale
    evt.total_qty = 100000; // 10 contracts
    evt.order_count = 5;
    evt.action = DepthUpdate::Add;
    evt.ts = 1000000000;

    size_t n = encode_depth_update(buf, evt, ctx);
    // MessageHeader(8) + root(11) + GroupHeader(3) + entry(32) + GroupHeader8Byte(8) = 62
    EXPECT_EQ(n, 62u);

    auto [hdr, body] = decode_header(buf);
    EXPECT_EQ(hdr.template_id, 46u);
    EXPECT_EQ(hdr.block_length, 11u);
    EXPECT_EQ(hdr.schema_id, MDP3_SCHEMA_ID);
    EXPECT_EQ(hdr.version, MDP3_VERSION);

    // Decode root block.
    MDIncrementalRefreshBook46 root{};
    std::memcpy(&root, body, sizeof(root));
    EXPECT_EQ(root.transact_time, 1000000000u);
    EXPECT_EQ(root.match_event_indicator, 0x80);

    // Decode NoMDEntries group header.
    const char* p = body + sizeof(root);
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    EXPECT_EQ(gh.block_length, 32u);
    EXPECT_EQ(gh.num_in_group, 1u);

    // Decode entry.
    RefreshBookEntry entry{};
    std::memcpy(&entry, p, sizeof(entry));
    EXPECT_EQ(entry.md_entry_px.mantissa, 45002500LL * ENGINE_TO_PRICE9_FACTOR);
    EXPECT_DOUBLE_EQ(entry.md_entry_px.to_double(), 4500.25);
    EXPECT_EQ(entry.md_entry_size, 10);
    EXPECT_EQ(entry.security_id, 12345);
    EXPECT_EQ(entry.rpt_seq, 1u);
    EXPECT_EQ(entry.number_of_orders, 5);
    EXPECT_EQ(entry.md_update_action,
              static_cast<uint8_t>(MDUpdateAction::New));
    EXPECT_EQ(entry.md_entry_type,
              static_cast<char>(MDEntryTypeBook::Bid));
    p += sizeof(entry);

    // Decode empty NoOrderIDEntries group.
    GroupHeader8Byte gh8{};
    GroupHeader8Byte::decode_from(p, gh8);
    EXPECT_EQ(gh8.block_length, 24u);
    EXPECT_EQ(gh8.num_in_group, 0u);

    // rpt_seq incremented.
    EXPECT_EQ(ctx.rpt_seq, 1u);
}

TEST(Mdp3EncoderTest, DepthUpdateSellSide) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    DepthUpdate evt{};
    evt.side = Side::Sell;
    evt.price = 45010000;
    evt.total_qty = 50000;
    evt.order_count = 3;
    evt.action = DepthUpdate::Remove;
    evt.ts = 2000000000;

    encode_depth_update(buf, evt, ctx);

    auto [hdr, body] = decode_header(buf);
    const char* p = body + sizeof(MDIncrementalRefreshBook46);
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);

    RefreshBookEntry entry{};
    std::memcpy(&entry, p, sizeof(entry));
    EXPECT_EQ(entry.md_entry_type,
              static_cast<char>(MDEntryTypeBook::Offer));
    EXPECT_EQ(entry.md_update_action,
              static_cast<uint8_t>(MDUpdateAction::Delete));
}

// ---------------------------------------------------------------------------
// TopOfBook encoding
// ---------------------------------------------------------------------------

TEST(Mdp3EncoderTest, EncodeTopOfBook) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    TopOfBook evt{};
    evt.best_bid = 45000000;   // 4500.0000
    evt.bid_qty = 200000;      // 20 contracts
    evt.best_ask = 45010000;   // 4501.0000
    evt.ask_qty = 150000;      // 15 contracts
    evt.ts = 3000000000;

    size_t n = encode_top_of_book(buf, evt, ctx);
    // MessageHeader(8) + root(11) + GroupHeader(3) + 2*entry(64) + GroupHeader8Byte(8) = 94
    EXPECT_EQ(n, 94u);

    auto [hdr, body] = decode_header(buf);
    EXPECT_EQ(hdr.template_id, 46u);

    const char* p = body + sizeof(MDIncrementalRefreshBook46);
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    EXPECT_EQ(gh.num_in_group, 2u);

    // Bid entry.
    RefreshBookEntry bid{};
    std::memcpy(&bid, p, sizeof(bid));
    EXPECT_DOUBLE_EQ(bid.md_entry_px.to_double(), 4500.0);
    EXPECT_EQ(bid.md_entry_size, 20);
    EXPECT_EQ(bid.md_entry_type,
              static_cast<char>(MDEntryTypeBook::Bid));
    p += sizeof(bid);

    // Ask entry.
    RefreshBookEntry ask{};
    std::memcpy(&ask, p, sizeof(ask));
    EXPECT_DOUBLE_EQ(ask.md_entry_px.to_double(), 4501.0);
    EXPECT_EQ(ask.md_entry_size, 15);
    EXPECT_EQ(ask.md_entry_type,
              static_cast<char>(MDEntryTypeBook::Offer));

    // rpt_seq should have incremented twice (once per entry).
    EXPECT_EQ(ctx.rpt_seq, 2u);
}

TEST(Mdp3EncoderTest, TopOfBookWithZeroBid) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    TopOfBook evt{};
    evt.best_bid = 0;       // empty book
    evt.bid_qty = 0;
    evt.best_ask = 45010000;
    evt.ask_qty = 50000;
    evt.ts = 4000000000;

    encode_top_of_book(buf, evt, ctx);

    auto [hdr, body] = decode_header(buf);
    const char* p = body + sizeof(MDIncrementalRefreshBook46);
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);

    // Bid entry should have null price.
    RefreshBookEntry bid{};
    std::memcpy(&bid, p, sizeof(bid));
    EXPECT_TRUE(bid.md_entry_px.is_null());
    EXPECT_EQ(bid.md_entry_size, 0);
}

// ---------------------------------------------------------------------------
// Trade encoding
// ---------------------------------------------------------------------------

TEST(Mdp3EncoderTest, EncodeTrade) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    Trade evt{};
    evt.price = 45005000;       // 4500.5000
    evt.quantity = 30000;       // 3 contracts
    evt.aggressor_id = 100;
    evt.resting_id = 50;
    evt.aggressor_side = Side::Buy;
    evt.ts = 5000000000;

    size_t n = encode_trade(buf, evt, ctx);
    // MessageHeader(8) + root(11) + GroupHeader(3) + entry(32) + GroupHeader8Byte(8) = 62
    EXPECT_EQ(n, 62u);

    auto [hdr, body] = decode_header(buf);
    EXPECT_EQ(hdr.template_id, 48u);
    EXPECT_EQ(hdr.block_length, 11u);

    // Decode root.
    MDIncrementalRefreshTradeSummary48 root{};
    std::memcpy(&root, body, sizeof(root));
    EXPECT_EQ(root.transact_time, 5000000000u);

    const char* p = body + sizeof(root);
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);
    EXPECT_EQ(gh.block_length, 32u);
    EXPECT_EQ(gh.num_in_group, 1u);

    TradeSummaryEntry entry{};
    std::memcpy(&entry, p, sizeof(entry));
    EXPECT_DOUBLE_EQ(entry.md_entry_px.to_double(), 4500.5);
    EXPECT_EQ(entry.md_entry_size, 3);
    EXPECT_EQ(entry.security_id, 12345);
    EXPECT_EQ(entry.aggressor_side,
              static_cast<uint8_t>(AggressorSide::Buy));
    EXPECT_EQ(entry.md_update_action,
              static_cast<uint8_t>(MDUpdateAction::New));
    EXPECT_EQ(entry.number_of_orders, 2);
    p += sizeof(entry);

    // Empty NoOrderIDEntries.
    GroupHeader8Byte gh8{};
    GroupHeader8Byte::decode_from(p, gh8);
    EXPECT_EQ(gh8.num_in_group, 0u);
}

TEST(Mdp3EncoderTest, TradeSellAggressor) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    Trade evt{};
    evt.price = 44990000;
    evt.quantity = 10000;
    evt.aggressor_side = Side::Sell;
    evt.ts = 6000000000;

    encode_trade(buf, evt, ctx);

    auto [hdr, body] = decode_header(buf);
    const char* p = body + sizeof(MDIncrementalRefreshTradeSummary48);
    GroupHeader gh{};
    p = GroupHeader::decode_from(p, gh);

    TradeSummaryEntry entry{};
    std::memcpy(&entry, p, sizeof(entry));
    EXPECT_EQ(entry.aggressor_side,
              static_cast<uint8_t>(AggressorSide::Sell));
}

// ---------------------------------------------------------------------------
// MarketStatus encoding
// ---------------------------------------------------------------------------

TEST(Mdp3EncoderTest, EncodeMarketStatus) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    MarketStatus evt{};
    evt.state = SessionState::Continuous;
    evt.ts = 7000000000;

    size_t n = encode_market_status(buf, evt, ctx);
    // MessageHeader(8) + SecurityStatus30(30) = 38
    EXPECT_EQ(n, 38u);

    auto [hdr, body] = decode_header(buf);
    EXPECT_EQ(hdr.template_id, 30u);
    EXPECT_EQ(hdr.block_length, 30u);

    SecurityStatus30 msg{};
    std::memcpy(&msg, body, sizeof(msg));
    EXPECT_EQ(msg.transact_time, 7000000000u);
    EXPECT_EQ(msg.security_id, 12345);
    EXPECT_EQ(msg.security_trading_status,
              static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade));
    EXPECT_EQ(std::string(msg.security_group, 6), "ES    ");
    EXPECT_EQ(std::string(msg.asset, 6), "ES    ");
}

TEST(Mdp3EncoderTest, EncodeMarketStatusHalt) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    MarketStatus evt{};
    evt.state = SessionState::Halt;
    evt.ts = 8000000000;

    encode_market_status(buf, evt, ctx);

    auto [hdr, body] = decode_header(buf);
    SecurityStatus30 msg{};
    std::memcpy(&msg, body, sizeof(msg));
    EXPECT_EQ(msg.security_trading_status,
              static_cast<uint8_t>(SecurityTradingStatus::TradingHalt));
}

TEST(Mdp3EncoderTest, AllSessionStatesEncode) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    // Verify every SessionState maps to a valid SecurityTradingStatus.
    SessionState states[] = {
        SessionState::Closed,
        SessionState::PreOpen,
        SessionState::OpeningAuction,
        SessionState::Continuous,
        SessionState::PreClose,
        SessionState::ClosingAuction,
        SessionState::Halt,
        SessionState::VolatilityAuction,
    };

    for (auto state : states) {
        MarketStatus evt{};
        evt.state = state;
        evt.ts = 1000000000;
        size_t n = encode_market_status(buf, evt, ctx);
        EXPECT_EQ(n, 38u) << "state=" << static_cast<int>(state);

        auto [hdr, body] = decode_header(buf);
        SecurityStatus30 msg{};
        std::memcpy(&msg, body, sizeof(msg));
        // All states should produce a non-null trading status.
        EXPECT_NE(msg.security_trading_status, UINT8_NULL)
            << "state=" << static_cast<int>(state);
    }
}

// ---------------------------------------------------------------------------
// Price and quantity conversion edge cases
// ---------------------------------------------------------------------------

TEST(Mdp3EncoderTest, PriceConversion) {
    // 4500.2500 in engine scale = 45002500
    auto p9 = engine_price_to_price9(45002500);
    EXPECT_EQ(p9.mantissa, 45002500LL * 100000LL);
    EXPECT_DOUBLE_EQ(p9.to_double(), 4500.25);

    // Negative price.
    auto neg = engine_price_to_price9(-10000);
    EXPECT_EQ(neg.mantissa, -10000LL * 100000LL);
    EXPECT_DOUBLE_EQ(neg.to_double(), -1.0);
}

TEST(Mdp3EncoderTest, PriceNull9) {
    auto null_p = engine_price_to_pricenull9(0);
    EXPECT_TRUE(null_p.is_null());

    auto valid = engine_price_to_pricenull9(10000);
    EXPECT_FALSE(valid.is_null());
}

TEST(Mdp3EncoderTest, QuantityConversion) {
    EXPECT_EQ(engine_qty_to_wire(10000), 1);     // 1 contract
    EXPECT_EQ(engine_qty_to_wire(100000), 10);    // 10 contracts
    EXPECT_EQ(engine_qty_to_wire(0), 0);
}

// ---------------------------------------------------------------------------
// rpt_seq increments across multiple encodes
// ---------------------------------------------------------------------------

TEST(Mdp3EncoderTest, RptSeqIncrementsAcrossCalls) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();
    EXPECT_EQ(ctx.rpt_seq, 0u);

    DepthUpdate d{};
    d.ts = 1;
    encode_depth_update(buf, d, ctx);
    EXPECT_EQ(ctx.rpt_seq, 1u);

    Trade t{};
    t.ts = 2;
    encode_trade(buf, t, ctx);
    EXPECT_EQ(ctx.rpt_seq, 2u);

    TopOfBook tob{};
    tob.ts = 3;
    encode_top_of_book(buf, tob, ctx);
    // TopOfBook writes 2 entries, each incrementing rpt_seq.
    EXPECT_EQ(ctx.rpt_seq, 4u);
}

}  // namespace
}  // namespace exchange::cme::sbe::mdp3
