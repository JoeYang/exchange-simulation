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

}  // namespace
}  // namespace exchange::cme::sbe::mdp3
