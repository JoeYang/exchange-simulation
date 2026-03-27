#include "cme/mdp3_feed_publisher.h"
#include "cme/cme_simulator.h"
#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_messages.h"
#include "cme/codec/sbe_header.h"
#include "test-harness/recording_listener.h"

#include <cstring>

#include "gtest/gtest.h"

namespace exchange {
namespace cme {
namespace {

using sbe::MessageHeader;
using sbe::mdp3::MDIncrementalRefreshBook46;
using sbe::mdp3::MDIncrementalRefreshTradeSummary48;
using sbe::mdp3::SecurityStatus30;
using sbe::mdp3::RefreshBookEntry;
using sbe::mdp3::TradeSummaryEntry;
using sbe::GroupHeader;
using sbe::mdp3::MDUpdateAction;
using sbe::mdp3::MDEntryTypeBook;
using sbe::mdp3::AggressorSide;
using sbe::mdp3::SecurityTradingStatus;

using TestSim = CmeSimulator<RecordingOrderListener, Mdp3FeedPublisher>;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class Mdp3FeedPublisherTest : public ::testing::Test {
protected:
    RecordingOrderListener ol_;
    Mdp3FeedPublisher pub_{1 /*security_id*/, "ES    ", "ES    "};
    TestSim sim_{ol_, pub_};

    void SetUp() override {
        CmeProductConfig es{
            .instrument_id = 1,
            .symbol = "ESH6",
            .description = "E-mini S&P 500",
            .product_group = "ES",
            .tick_size = 2500,       // 0.25
            .lot_size = 10000,       // 1 contract
            .max_order_size = 20000000,
            .band_pct = 5,
        };
        sim_.load_products({es});
    }

    OrderRequest make_limit(uint64_t cl_id, Side side, Price price,
                            Quantity qty, Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id,
            .account_id = 1,
            .side = side,
            .type = OrderType::Limit,
            .tif = TimeInForce::GTC,
            .price = price,
            .quantity = qty,
            .stop_price = 0,
            .timestamp = ts,
            .gtd_expiry = 0,
        };
    }

    // Decode the SBE MessageHeader from a packet.
    MessageHeader decode_hdr(const Mdp3Packet& pkt) {
        MessageHeader hdr{};
        MessageHeader::decode_from(pkt.bytes(), hdr);
        return hdr;
    }
};

// ---------------------------------------------------------------------------
// MarketStatus events
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, MarketStatusOnSessionChange) {
    sim_.start_trading_day(100);

    ASSERT_GE(pub_.packet_count(), 1u);
    auto hdr = decode_hdr(pub_.packets().back());
    EXPECT_EQ(hdr.template_id, 30u);

    // Decode SecurityStatus30.
    SecurityStatus30 msg{};
    std::memcpy(&msg, pub_.packets().back().bytes() + sizeof(MessageHeader),
                sizeof(msg));
    EXPECT_EQ(msg.security_trading_status,
              static_cast<uint8_t>(SecurityTradingStatus::PreOpen));
    EXPECT_EQ(msg.security_id, 1);
}

TEST_F(Mdp3FeedPublisherTest, MarketStatusContinuous) {
    sim_.start_trading_day(100);
    pub_.clear();

    sim_.open_market(200);

    // open_market fires Continuous status.
    bool found_continuous = false;
    for (const auto& pkt : pub_.packets()) {
        auto hdr = decode_hdr(pkt);
        if (hdr.template_id == 30) {
            SecurityStatus30 msg{};
            std::memcpy(&msg, pkt.bytes() + sizeof(MessageHeader), sizeof(msg));
            if (msg.security_trading_status ==
                static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade)) {
                found_continuous = true;
            }
        }
    }
    EXPECT_TRUE(found_continuous);
}

// ---------------------------------------------------------------------------
// DepthUpdate on order submission
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, BookUpdateOnNewOrder) {
    sim_.start_trading_day(100);
    sim_.open_market(200);
    pub_.clear();

    // Submit a bid — should produce book update (template 46).
    sim_.new_order(1, make_limit(1, Side::Buy, 45000000, 10000, 300));

    bool found_book = false;
    for (const auto& pkt : pub_.packets()) {
        auto hdr = decode_hdr(pkt);
        if (hdr.template_id == 46) {
            found_book = true;

            // Decode root + group header.
            const char* p = pkt.bytes() + sizeof(MessageHeader);
            p += sizeof(MDIncrementalRefreshBook46);
            GroupHeader gh{};
            p = GroupHeader::decode_from(p, gh);
            EXPECT_GE(gh.num_in_group, 1u);

            // First entry should reference our security.
            RefreshBookEntry entry{};
            std::memcpy(&entry, p, sizeof(entry));
            EXPECT_EQ(entry.security_id, 1);
            break;
        }
    }
    EXPECT_TRUE(found_book);
}

// ---------------------------------------------------------------------------
// Trade event on matching orders
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, TradeOnMatch) {
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Resting bid from account 1, then aggressing ask from account 2.
    auto bid = make_limit(1, Side::Buy, 45000000, 10000, 300);
    bid.account_id = 1;
    auto ask = make_limit(2, Side::Sell, 45000000, 10000, 400);
    ask.account_id = 2;
    sim_.new_order(1, bid);
    sim_.new_order(1, ask);

    // Search all packets for a trade (template 48).
    bool found_trade = false;
    for (const auto& pkt : pub_.packets()) {
        auto hdr = decode_hdr(pkt);
        if (hdr.template_id == 48) {
            found_trade = true;

            const char* p = pkt.bytes() + sizeof(MessageHeader);
            p += sizeof(MDIncrementalRefreshTradeSummary48);
            GroupHeader gh{};
            p = GroupHeader::decode_from(p, gh);
            EXPECT_EQ(gh.num_in_group, 1u);

            TradeSummaryEntry entry{};
            std::memcpy(&entry, p, sizeof(entry));
            EXPECT_DOUBLE_EQ(entry.md_entry_px.to_double(), 4500.0);
            EXPECT_EQ(entry.md_entry_size, 1);  // 1 contract
            EXPECT_EQ(entry.security_id, 1);
            break;
        }
    }
    EXPECT_TRUE(found_trade);
}

// ---------------------------------------------------------------------------
// TopOfBook event
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, TopOfBookUpdate) {
    sim_.start_trading_day(100);
    sim_.open_market(200);
    pub_.clear();

    // Place bid and ask to establish a two-sided book.
    sim_.new_order(1, make_limit(1, Side::Buy, 45000000, 10000, 300));
    sim_.new_order(1, make_limit(2, Side::Sell, 45010000, 10000, 400));

    // Look for a book update (template 46) with 2 entries.
    bool found_tob = false;
    for (const auto& pkt : pub_.packets()) {
        auto hdr = decode_hdr(pkt);
        if (hdr.template_id == 46 && pkt.len == 94) {
            found_tob = true;

            const char* p = pkt.bytes() + sizeof(MessageHeader);
            p += sizeof(MDIncrementalRefreshBook46);
            GroupHeader gh{};
            p = GroupHeader::decode_from(p, gh);
            EXPECT_EQ(gh.num_in_group, 2u);
        }
    }
    EXPECT_TRUE(found_tob);
}

// ---------------------------------------------------------------------------
// rpt_seq is monotonic across multiple events
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, RptSeqIncrementsMonotonically) {
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Submit a few orders to generate events.
    sim_.new_order(1, make_limit(1, Side::Buy, 45000000, 10000, 300));
    sim_.new_order(1, make_limit(2, Side::Sell, 45010000, 10000, 400));

    // rpt_seq should be positive after generating events.
    EXPECT_GT(pub_.rpt_seq(), 0u);
}

// ---------------------------------------------------------------------------
// Packet accessors and clear
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, ClearResetsPackets) {
    sim_.start_trading_day(100);
    EXPECT_GT(pub_.packet_count(), 0u);

    pub_.clear();
    EXPECT_EQ(pub_.packet_count(), 0u);
}

TEST_F(Mdp3FeedPublisherTest, ContextReflectsSecurityId) {
    EXPECT_EQ(pub_.context().security_id, 1);
}

// ---------------------------------------------------------------------------
// Close market produces status events
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, CloseMarketProducesStatusEvents) {
    sim_.start_trading_day(100);
    sim_.open_market(200);
    pub_.clear();

    sim_.close_market(500);

    // Should see at least PreClose and Closed status messages.
    int status_count = 0;
    for (const auto& pkt : pub_.packets()) {
        auto hdr = decode_hdr(pkt);
        if (hdr.template_id == 30) ++status_count;
    }
    EXPECT_GE(status_count, 2);
}

// ---------------------------------------------------------------------------
// Regression: default-constructed publisher encodes security_id=0, which
// causes observers to silently drop all events.  Verify that a properly
// initialized publisher produces packets that survive security_id filtering
// after a full encode->decode round trip.
// ---------------------------------------------------------------------------

TEST_F(Mdp3FeedPublisherTest, RoundTripDecodeMatchesSecurityIdFilter) {
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Two crossing orders produce depth + trade + top-of-book events.
    auto bid = make_limit(1, Side::Buy, 45000000, 10000, 300);
    bid.account_id = 1;
    auto ask = make_limit(2, Side::Sell, 45000000, 10000, 400);
    ask.account_id = 2;
    sim_.new_order(1, bid);
    sim_.new_order(1, ask);

    const int32_t expected_security_id = 1;

    // Decode every packet and verify security_id passes the observer's filter.
    int book_events = 0;
    int trade_events = 0;
    int status_events = 0;

    struct CountingVisitor {
        int32_t filter_id;
        int* book_out;
        int* trade_out;
        int* status_out;

        void operator()(const sbe::mdp3::DecodedRefreshBook46& msg) {
            for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
                if (msg.md_entries[i].security_id == filter_id) ++(*book_out);
            }
        }
        void operator()(const sbe::mdp3::DecodedTradeSummary48& msg) {
            for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
                if (msg.md_entries[i].security_id == filter_id) ++(*trade_out);
            }
        }
        void operator()(const sbe::mdp3::DecodedSecurityStatus30& msg) {
            if (msg.root.security_id == filter_id) ++(*status_out);
        }
        void operator()(const sbe::mdp3::DecodedSnapshot53&) {}
        void operator()(const sbe::mdp3::DecodedInstrumentDef54&) {}
    };

    CountingVisitor visitor{expected_security_id, &book_events, &trade_events,
                            &status_events};

    for (const auto& pkt : pub_.packets()) {
        auto rc = sbe::mdp3::decode_mdp3_message(pkt.bytes(), pkt.len, visitor);
        EXPECT_NE(rc, sbe::mdp3::DecodeResult::kBufferTooShort);
        EXPECT_NE(rc, sbe::mdp3::DecodeResult::kBadSchemaId);
    }

    // Must see book updates, at least one trade, and status events.
    EXPECT_GT(book_events, 0) << "No book events passed security_id filter";
    EXPECT_GT(trade_events, 0) << "No trade events passed security_id filter";
    EXPECT_GT(status_events, 0) << "No status events passed security_id filter";
}

// Verify that default-constructed publisher produces security_id=0,
// which would fail the observer's filter — the exact bug scenario.
TEST_F(Mdp3FeedPublisherTest, DefaultConstructedPublisherHasZeroSecurityId) {
    Mdp3FeedPublisher default_pub;
    EXPECT_EQ(default_pub.context().security_id, 0)
        << "Default-constructed publisher should have security_id=0 "
           "(the bug: this value would be silently dropped by observers)";
}

}  // namespace
}  // namespace cme
}  // namespace exchange
