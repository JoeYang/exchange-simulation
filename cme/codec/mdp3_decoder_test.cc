#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_encoder.h"

#include <cstring>

#include "gtest/gtest.h"

namespace exchange::cme::sbe::mdp3 {
namespace {

Mdp3EncodeContext MakeCtx() {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 54321;
    ctx.rpt_seq = 0;
    std::memcpy(ctx.security_group, "NQ    ", 6);
    std::memcpy(ctx.asset, "NQ    ", 6);
    return ctx;
}

// ---------------------------------------------------------------------------
// SecurityStatus30 round-trip
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DecodeSecurityStatus30) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    MarketStatus evt{};
    evt.state = SessionState::Continuous;
    evt.ts = 9876543210;

    size_t n = encode_market_status(buf, evt, ctx);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, n, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedSecurityStatus30>) {
            visited = true;
            EXPECT_EQ(msg.root.transact_time, 9876543210u);
            EXPECT_EQ(msg.root.security_id, 54321);
            EXPECT_EQ(msg.root.security_trading_status,
                      static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade));
            EXPECT_EQ(std::string(msg.root.security_group, 6), "NQ    ");
            EXPECT_EQ(std::string(msg.root.asset, 6), "NQ    ");
            EXPECT_EQ(msg.root.halt_reason,
                      static_cast<uint8_t>(HaltReason::GroupSchedule));
            EXPECT_EQ(msg.root.security_trading_event,
                      static_cast<uint8_t>(SecurityTradingEvent::NoEvent));
        }
    });

    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// MDIncrementalRefreshBook46 round-trip (single entry via DepthUpdate)
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DecodeRefreshBook46SingleEntry) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    DepthUpdate evt{};
    evt.side = Side::Buy;
    evt.price = 15000 * PRICE_SCALE;  // 15000.0000
    evt.total_qty = 50 * PRICE_SCALE; // 50 contracts
    evt.order_count = 7;
    evt.action = DepthUpdate::Add;
    evt.ts = 1111111111;

    size_t n = encode_depth_update(buf, evt, ctx);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, n, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedRefreshBook46>) {
            visited = true;
            EXPECT_EQ(msg.root.transact_time, 1111111111u);
            EXPECT_EQ(msg.root.match_event_indicator, 0x80);

            ASSERT_EQ(msg.num_md_entries, 1u);
            const auto& e = msg.md_entries[0];
            EXPECT_DOUBLE_EQ(e.md_entry_px.to_double(), 15000.0);
            EXPECT_EQ(e.md_entry_size, 50);
            EXPECT_EQ(e.security_id, 54321);
            EXPECT_EQ(e.rpt_seq, 1u);
            EXPECT_EQ(e.number_of_orders, 7);
            EXPECT_EQ(e.md_update_action,
                      static_cast<uint8_t>(MDUpdateAction::New));
            EXPECT_EQ(e.md_entry_type,
                      static_cast<char>(MDEntryTypeBook::Bid));

            EXPECT_EQ(msg.num_order_entries, 0u);
        }
    });

    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// MDIncrementalRefreshBook46 round-trip (2 entries via TopOfBook)
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DecodeRefreshBook46TwoEntries) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    TopOfBook evt{};
    evt.best_bid = 14990 * PRICE_SCALE;
    evt.bid_qty = 20 * PRICE_SCALE;
    evt.best_ask = 15010 * PRICE_SCALE;
    evt.ask_qty = 30 * PRICE_SCALE;
    evt.ts = 2222222222;

    size_t n = encode_top_of_book(buf, evt, ctx);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, n, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedRefreshBook46>) {
            visited = true;
            ASSERT_EQ(msg.num_md_entries, 2u);

            // Bid entry.
            EXPECT_DOUBLE_EQ(msg.md_entries[0].md_entry_px.to_double(), 14990.0);
            EXPECT_EQ(msg.md_entries[0].md_entry_size, 20);
            EXPECT_EQ(msg.md_entries[0].md_entry_type,
                      static_cast<char>(MDEntryTypeBook::Bid));

            // Ask entry.
            EXPECT_DOUBLE_EQ(msg.md_entries[1].md_entry_px.to_double(), 15010.0);
            EXPECT_EQ(msg.md_entries[1].md_entry_size, 30);
            EXPECT_EQ(msg.md_entries[1].md_entry_type,
                      static_cast<char>(MDEntryTypeBook::Offer));

            EXPECT_EQ(msg.num_order_entries, 0u);
        }
    });

    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// MDIncrementalRefreshTradeSummary48 round-trip
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DecodeTradeSummary48) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    Trade evt{};
    evt.price = 15005 * PRICE_SCALE;  // 15005.0000
    evt.quantity = 10 * PRICE_SCALE;   // 10 contracts
    evt.aggressor_id = 200;
    evt.resting_id = 100;
    evt.aggressor_side = Side::Sell;
    evt.ts = 3333333333;

    size_t n = encode_trade(buf, evt, ctx);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, n, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedTradeSummary48>) {
            visited = true;
            EXPECT_EQ(msg.root.transact_time, 3333333333u);
            EXPECT_EQ(msg.root.match_event_indicator, 0x80);

            ASSERT_EQ(msg.num_md_entries, 1u);
            const auto& e = msg.md_entries[0];
            EXPECT_DOUBLE_EQ(e.md_entry_px.to_double(), 15005.0);
            EXPECT_EQ(e.md_entry_size, 10);
            EXPECT_EQ(e.security_id, 54321);
            EXPECT_EQ(e.aggressor_side,
                      static_cast<uint8_t>(AggressorSide::Sell));
            EXPECT_EQ(e.md_update_action,
                      static_cast<uint8_t>(MDUpdateAction::New));
            EXPECT_EQ(e.number_of_orders, 2);

            EXPECT_EQ(msg.num_order_entries, 0u);
        }
    });

    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, BufferTooShortForHeader) {
    char buf[4] = {};
    bool visited = false;
    auto rc = decode_mdp3_message(buf, 4, [&](auto&) { visited = true; });
    EXPECT_EQ(rc, DecodeResult::kBufferTooShort);
    EXPECT_FALSE(visited);
}

TEST(Mdp3DecoderTest, BufferTooShortForBody) {
    // Valid header but body truncated.
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();
    MarketStatus evt{};
    evt.state = SessionState::Closed;
    evt.ts = 1;
    encode_market_status(buf, evt, ctx);

    // Truncate body: provide header but only 5 bytes of the 30-byte root.
    bool visited = false;
    auto rc = decode_mdp3_message(buf, sizeof(MessageHeader) + 5, [&](auto&) {
        visited = true;
    });
    EXPECT_EQ(rc, DecodeResult::kBufferTooShort);
    EXPECT_FALSE(visited);
}

TEST(Mdp3DecoderTest, UnknownTemplateId) {
    char buf[16] = {};
    MessageHeader hdr{};
    hdr.block_length = 0;
    hdr.template_id = 999;
    hdr.schema_id = MDP3_SCHEMA_ID;
    hdr.version = MDP3_VERSION;
    hdr.encode_to(buf);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, sizeof(buf), [&](auto&) {
        visited = true;
    });
    EXPECT_EQ(rc, DecodeResult::kUnknownTemplateId);
    EXPECT_FALSE(visited);
}

TEST(Mdp3DecoderTest, TruncatedGroupHeader) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();
    DepthUpdate evt{};
    evt.ts = 1;
    encode_depth_update(buf, evt, ctx);

    // Truncate right after root block (before GroupHeader).
    // Header(8) + root(11) = 19 bytes.
    bool visited = false;
    auto rc = decode_mdp3_message(buf, 19, [&](auto&) { visited = true; });
    EXPECT_EQ(rc, DecodeResult::kBufferTooShort);
    EXPECT_FALSE(visited);
}

// ---------------------------------------------------------------------------
// Dispatch correctness: verify each message type dispatches to the right overload
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DispatchCorrectOverload) {
    char buf[MAX_MDP3_ENCODED_SIZE];
    auto ctx = MakeCtx();

    // SecurityStatus30.
    {
        MarketStatus evt{};
        evt.state = SessionState::PreOpen;
        evt.ts = 1;
        size_t n = encode_market_status(buf, evt, ctx);

        int which = -1;
        decode_mdp3_message(buf, n, [&](auto& msg) {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, DecodedSecurityStatus30>) which = 30;
            else if constexpr (std::is_same_v<T, DecodedRefreshBook46>) which = 46;
            else if constexpr (std::is_same_v<T, DecodedTradeSummary48>) which = 48;
        });
        EXPECT_EQ(which, 30);
    }

    // RefreshBook46.
    {
        DepthUpdate evt{};
        evt.ts = 2;
        size_t n = encode_depth_update(buf, evt, ctx);

        int which = -1;
        decode_mdp3_message(buf, n, [&](auto& msg) {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, DecodedSecurityStatus30>) which = 30;
            else if constexpr (std::is_same_v<T, DecodedRefreshBook46>) which = 46;
            else if constexpr (std::is_same_v<T, DecodedTradeSummary48>) which = 48;
        });
        EXPECT_EQ(which, 46);
    }

    // TradeSummary48.
    {
        Trade evt{};
        evt.ts = 3;
        size_t n = encode_trade(buf, evt, ctx);

        int which = -1;
        decode_mdp3_message(buf, n, [&](auto& msg) {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, DecodedSecurityStatus30>) which = 30;
            else if constexpr (std::is_same_v<T, DecodedRefreshBook46>) which = 46;
            else if constexpr (std::is_same_v<T, DecodedTradeSummary48>) which = 48;
        });
        EXPECT_EQ(which, 48);
    }
}

// ---------------------------------------------------------------------------
// SnapshotFullRefreshOrderBook53 (manually constructed wire bytes)
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DecodeSnapshot53) {
    // Wire layout: Header(8) + root(28) + GroupHeader(3) + 2*entry(29) = 97 bytes.
    char buf[128] = {};
    char* p = buf;

    // MessageHeader.
    MessageHeader hdr = make_header<SnapshotFullRefreshOrderBook53>();
    p = hdr.encode_to(p);

    // Root block.
    SnapshotFullRefreshOrderBook53 root{};
    root.last_msg_seq_num_processed = 100;
    root.tot_num_reports = 1;
    root.security_id = 99999;
    root.no_chunks = 1;
    root.current_chunk = 1;
    root.transact_time = 5555555555;
    std::memcpy(p, &root, sizeof(root));
    p += sizeof(root);

    // NoMDEntries GroupHeader: 2 entries, blockLength=29.
    GroupHeader gh{};
    gh.block_length = SnapshotOrderBookEntry::BLOCK_LENGTH;
    gh.num_in_group = 2;
    p = gh.encode_to(p);

    // Entry 0: Bid.
    SnapshotOrderBookEntry e0{};
    e0.order_id = 1001;
    e0.md_order_priority = 500;
    e0.md_entry_px = PRICE9::from_double(4500.25);
    e0.md_display_qty = 10;
    e0.md_entry_type = static_cast<char>(MDEntryTypeBook::Bid);
    std::memcpy(p, &e0, sizeof(e0));
    p += sizeof(e0);

    // Entry 1: Offer.
    SnapshotOrderBookEntry e1{};
    e1.order_id = 1002;
    e1.md_order_priority = 600;
    e1.md_entry_px = PRICE9::from_double(4501.00);
    e1.md_display_qty = 20;
    e1.md_entry_type = static_cast<char>(MDEntryTypeBook::Offer);
    std::memcpy(p, &e1, sizeof(e1));
    p += sizeof(e1);

    size_t total = static_cast<size_t>(p - buf);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, total, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedSnapshot53>) {
            visited = true;
            EXPECT_EQ(msg.root.security_id, 99999);
            EXPECT_EQ(msg.root.transact_time, 5555555555u);
            EXPECT_EQ(msg.root.last_msg_seq_num_processed, 100u);

            ASSERT_EQ(msg.num_md_entries, 2u);
            EXPECT_EQ(msg.md_entries[0].order_id, 1001u);
            EXPECT_DOUBLE_EQ(msg.md_entries[0].md_entry_px.to_double(), 4500.25);
            EXPECT_EQ(msg.md_entries[0].md_display_qty, 10);
            EXPECT_EQ(msg.md_entries[0].md_entry_type,
                      static_cast<char>(MDEntryTypeBook::Bid));

            EXPECT_EQ(msg.md_entries[1].order_id, 1002u);
            EXPECT_DOUBLE_EQ(msg.md_entries[1].md_entry_px.to_double(), 4501.0);
            EXPECT_EQ(msg.md_entries[1].md_display_qty, 20);
            EXPECT_EQ(msg.md_entries[1].md_entry_type,
                      static_cast<char>(MDEntryTypeBook::Offer));
        }
    });

    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// MDInstrumentDefinitionFuture54 (manually constructed wire bytes)
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, DecodeInstrumentDef54) {
    // Wire: Header(8) + root(224) + 4 groups with GroupHeaders.
    char buf[512] = {};
    char* p = buf;

    MessageHeader hdr = make_header<MDInstrumentDefinitionFuture54>();
    p = hdr.encode_to(p);

    // Root block.
    MDInstrumentDefinitionFuture54 root{};
    std::memset(&root, 0, sizeof(root));
    root.security_id = 77777;
    root.match_event_indicator = 0x80;
    std::memcpy(root.symbol, "ESH6                ", 20);
    root.min_trade_vol = 1;
    root.max_trade_vol = 10000;
    root.min_price_increment = PRICE9::from_double(0.25);
    std::memcpy(p, &root, sizeof(root));
    p += sizeof(root);

    // NoEvents: 1 entry (blockLength=9).
    GroupHeader gh_events{};
    gh_events.block_length = InstrDefEventEntry::BLOCK_LENGTH;
    gh_events.num_in_group = 1;
    p = gh_events.encode_to(p);

    InstrDefEventEntry event{};
    event.event_type = static_cast<uint8_t>(EventType::Activation);
    event.event_time = 1234567890;
    std::memcpy(p, &event, sizeof(event));
    p += sizeof(event);

    // NoMDFeedTypes: 1 entry (blockLength=4).
    GroupHeader gh_feeds{};
    gh_feeds.block_length = InstrDefFeedTypeEntry::BLOCK_LENGTH;
    gh_feeds.num_in_group = 1;
    p = gh_feeds.encode_to(p);

    InstrDefFeedTypeEntry feed{};
    std::memcpy(feed.md_feed_type, "GBX", 3);
    feed.market_depth = 10;
    std::memcpy(p, &feed, sizeof(feed));
    p += sizeof(feed);

    // NoInstAttrib: 0 entries.
    GroupHeader gh_attrib{};
    gh_attrib.block_length = InstrDefAttribEntry::BLOCK_LENGTH;
    gh_attrib.num_in_group = 0;
    p = gh_attrib.encode_to(p);

    // NoLotTypeRules: 1 entry (blockLength=5).
    GroupHeader gh_lots{};
    gh_lots.block_length = InstrDefLotTypeEntry::BLOCK_LENGTH;
    gh_lots.num_in_group = 1;
    p = gh_lots.encode_to(p);

    InstrDefLotTypeEntry lot{};
    lot.lot_type = 1;
    lot.min_lot_size = 10000;  // 1.0 in DecimalQty (exp=-4)
    std::memcpy(p, &lot, sizeof(lot));
    p += sizeof(lot);

    size_t total = static_cast<size_t>(p - buf);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, total, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedInstrumentDef54>) {
            visited = true;
            EXPECT_EQ(msg.root.security_id, 77777);
            EXPECT_EQ(std::string(msg.root.symbol, 4), "ESH6");

            ASSERT_EQ(msg.num_events, 1u);
            EXPECT_EQ(msg.events[0].event_type,
                      static_cast<uint8_t>(EventType::Activation));
            EXPECT_EQ(msg.events[0].event_time, 1234567890u);

            ASSERT_EQ(msg.num_feed_types, 1u);
            EXPECT_EQ(std::string(msg.feed_types[0].md_feed_type, 3), "GBX");
            EXPECT_EQ(msg.feed_types[0].market_depth, 10);

            EXPECT_EQ(msg.num_attribs, 0u);

            ASSERT_EQ(msg.num_lot_types, 1u);
            EXPECT_EQ(msg.lot_types[0].lot_type, 1);
            EXPECT_EQ(msg.lot_types[0].min_lot_size, 10000);
        }
    });

    EXPECT_EQ(rc, DecodeResult::kOk);
    EXPECT_TRUE(visited);
}

// ---------------------------------------------------------------------------
// Schema ID validation
// ---------------------------------------------------------------------------

TEST(Mdp3DecoderTest, BadSchemaIdReturnsError) {
    char buf[16] = {};
    MessageHeader hdr{};
    hdr.block_length = 0;
    hdr.template_id = SECURITY_STATUS_30_ID;
    hdr.schema_id = 99;  // Wrong schema.
    hdr.version = MDP3_VERSION;
    hdr.encode_to(buf);

    bool visited = false;
    auto rc = decode_mdp3_message(buf, sizeof(buf), [&](auto&) {
        visited = true;
    });
    EXPECT_EQ(rc, DecodeResult::kBadSchemaId);
    EXPECT_FALSE(visited);
}

}  // namespace
}  // namespace exchange::cme::sbe::mdp3
