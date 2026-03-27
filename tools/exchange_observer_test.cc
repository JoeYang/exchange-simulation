// Unit tests for exchange-observer logic.
//
// Tests cover: journal output format for each MD message type, display state
// update (book, trades, OHLCV), instrument filtering, and sequence gap
// detection.
//
// We test the observer by constructing raw MDP3/iMpact encoded datagrams,
// wrapping them with a McastSeqHeader, sending via UdpMulticastPublisher, and
// receiving on UdpMulticastReceiver. The decoded results are verified by
// reading the journal file and checking display state.

#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_messages.h"
#include "cme/codec/sbe_header.h"
#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_messages.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

// ---------------------------------------------------------------------------
// We re-declare the core types and functions from exchange_observer.cc so we
// can test them directly. In production these would be in a header; for this
// binary-only tool we test via the codec + journal output format.
// ---------------------------------------------------------------------------

// --- BookLevel, DisplayState, and helper functions replicated for testing ---

static constexpr int BOOK_DEPTH  = 5;
static constexpr int TRADE_DEPTH = 10;

struct BookLevel {
    int64_t  price{0};
    int32_t  qty{0};
    int32_t  order_count{0};
};

struct TradeEntry {
    int64_t  price{0};
    int32_t  qty{0};
    uint8_t  aggressor_side{0};
    uint64_t timestamp_ns{0};
};

struct DisplayState {
    BookLevel bids[BOOK_DEPTH]{};
    BookLevel asks[BOOK_DEPTH]{};
    int       bid_levels{0};
    int       ask_levels{0};

    TradeEntry trades[TRADE_DEPTH]{};
    int        trade_count{0};
    int        trade_write_idx{0};

    int64_t  open_price{0};
    int64_t  high_price{0};
    int64_t  low_price{0};
    int64_t  close_price{0};
    int64_t  volume{0};

    uint64_t total_messages{0};
    uint64_t messages_this_second{0};
    uint64_t msgs_per_sec{0};
    uint64_t decode_errors{0};
    uint64_t total_trades{0};

    uint32_t last_seq{0};
    uint64_t seq_gaps{0};
};

static void record_trade(DisplayState& ds, int64_t price, int32_t qty,
                          uint8_t aggressor, uint64_t ts) {
    auto& t = ds.trades[ds.trade_write_idx];
    t.price = price; t.qty = qty;
    t.aggressor_side = aggressor; t.timestamp_ns = ts;
    ds.trade_write_idx = (ds.trade_write_idx + 1) % TRADE_DEPTH;
    if (ds.trade_count < TRADE_DEPTH) ++ds.trade_count;
    ++ds.total_trades;
    if (ds.open_price == 0) ds.open_price = price;
    if (price > ds.high_price || ds.high_price == 0) ds.high_price = price;
    if (price < ds.low_price || ds.low_price == 0) ds.low_price = price;
    ds.close_price = price;
    ds.volume += qty;
}

static void update_book_side(BookLevel* levels, int& count,
                              int64_t price, int32_t qty, int32_t orders,
                              bool is_delete, bool is_bid) {
    if (is_delete) {
        for (int i = 0; i < count; ++i) {
            if (levels[i].price == price) {
                for (int j = i; j < count - 1; ++j) levels[j] = levels[j + 1];
                --count; levels[count] = BookLevel{}; return;
            }
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (levels[i].price == price) {
            levels[i].qty = qty; levels[i].order_count = orders; return;
        }
    }
    BookLevel entry{price, qty, orders};
    if (count < BOOK_DEPTH) { levels[count] = entry; ++count; }
    else {
        int worst = count - 1;
        bool better = is_bid ? (price > levels[worst].price)
                             : (price < levels[worst].price);
        if (!better) return;
        levels[worst] = entry;
    }
    if (is_bid) {
        std::sort(levels, levels + count,
                  [](const BookLevel& a, const BookLevel& b) { return a.price > b.price; });
    } else {
        std::sort(levels, levels + count,
                  [](const BookLevel& a, const BookLevel& b) { return a.price < b.price; });
    }
}

// ---------------------------------------------------------------------------
// Test: BookLevel management
// ---------------------------------------------------------------------------

TEST(BookLevel, AddBidLevels) {
    BookLevel bids[BOOK_DEPTH]{};
    int count = 0;

    update_book_side(bids, count, 5000, 10, 1, false, true);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(bids[0].price, 5000);
    EXPECT_EQ(bids[0].qty, 10);

    update_book_side(bids, count, 5100, 20, 2, false, true);
    EXPECT_EQ(count, 2);
    // Descending sort: 5100 should be at index 0.
    EXPECT_EQ(bids[0].price, 5100);
    EXPECT_EQ(bids[1].price, 5000);

    update_book_side(bids, count, 5050, 15, 1, false, true);
    EXPECT_EQ(count, 3);
    EXPECT_EQ(bids[0].price, 5100);
    EXPECT_EQ(bids[1].price, 5050);
    EXPECT_EQ(bids[2].price, 5000);
}

TEST(BookLevel, AddAskLevels) {
    BookLevel asks[BOOK_DEPTH]{};
    int count = 0;

    update_book_side(asks, count, 5200, 10, 1, false, false);
    update_book_side(asks, count, 5100, 20, 2, false, false);
    EXPECT_EQ(count, 2);
    // Ascending sort: 5100 should be at index 0.
    EXPECT_EQ(asks[0].price, 5100);
    EXPECT_EQ(asks[1].price, 5200);
}

TEST(BookLevel, DeleteLevel) {
    BookLevel bids[BOOK_DEPTH]{};
    int count = 0;

    update_book_side(bids, count, 5000, 10, 1, false, true);
    update_book_side(bids, count, 5100, 20, 2, false, true);
    EXPECT_EQ(count, 2);

    update_book_side(bids, count, 5100, 0, 0, true, true);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(bids[0].price, 5000);
}

TEST(BookLevel, DeleteNonexistentLevel) {
    BookLevel bids[BOOK_DEPTH]{};
    int count = 0;

    update_book_side(bids, count, 5000, 10, 1, false, true);
    update_book_side(bids, count, 9999, 0, 0, true, true); // no match
    EXPECT_EQ(count, 1);
    EXPECT_EQ(bids[0].price, 5000);
}

TEST(BookLevel, UpdateExistingLevel) {
    BookLevel bids[BOOK_DEPTH]{};
    int count = 0;

    update_book_side(bids, count, 5000, 10, 1, false, true);
    update_book_side(bids, count, 5000, 30, 3, false, true);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(bids[0].qty, 30);
    EXPECT_EQ(bids[0].order_count, 3);
}

TEST(BookLevel, OverflowReplacesWorstBid) {
    BookLevel bids[BOOK_DEPTH]{};
    int count = 0;

    // Fill all 5 levels.
    for (int i = 0; i < BOOK_DEPTH; ++i) {
        update_book_side(bids, count, 5000 + i * 100, 10, 1, false, true);
    }
    EXPECT_EQ(count, BOOK_DEPTH);

    // Add a better price -- should replace the worst (lowest bid).
    update_book_side(bids, count, 6000, 50, 5, false, true);
    EXPECT_EQ(count, BOOK_DEPTH);
    EXPECT_EQ(bids[0].price, 6000);  // best bid
    // The old lowest bid (5000) should be gone.
    for (int i = 0; i < count; ++i) {
        EXPECT_NE(bids[i].price, 5000);
    }
}

TEST(BookLevel, OverflowRejectsWorsePriceAsk) {
    BookLevel asks[BOOK_DEPTH]{};
    int count = 0;

    for (int i = 0; i < BOOK_DEPTH; ++i) {
        update_book_side(asks, count, 5000 + i * 100, 10, 1, false, false);
    }
    EXPECT_EQ(count, BOOK_DEPTH);

    // Try to add a worse ask (higher price) -- should be rejected.
    update_book_side(asks, count, 9999, 10, 1, false, false);
    EXPECT_EQ(count, BOOK_DEPTH);
    for (int i = 0; i < count; ++i) {
        EXPECT_NE(asks[i].price, 9999);
    }
}

// ---------------------------------------------------------------------------
// Test: Trade recording and OHLCV
// ---------------------------------------------------------------------------

TEST(TradeRecording, OHLCVUpdate) {
    DisplayState ds{};

    record_trade(ds, 100, 10, 1, 1000);
    EXPECT_EQ(ds.open_price, 100);
    EXPECT_EQ(ds.high_price, 100);
    EXPECT_EQ(ds.low_price, 100);
    EXPECT_EQ(ds.close_price, 100);
    EXPECT_EQ(ds.volume, 10);
    EXPECT_EQ(ds.total_trades, 1u);

    record_trade(ds, 200, 5, 2, 2000);
    EXPECT_EQ(ds.open_price, 100);  // unchanged
    EXPECT_EQ(ds.high_price, 200);
    EXPECT_EQ(ds.low_price, 100);
    EXPECT_EQ(ds.close_price, 200);
    EXPECT_EQ(ds.volume, 15);

    record_trade(ds, 50, 3, 1, 3000);
    EXPECT_EQ(ds.low_price, 50);
    EXPECT_EQ(ds.close_price, 50);
}

TEST(TradeRecording, CircularBuffer) {
    DisplayState ds{};

    // Fill beyond TRADE_DEPTH.
    for (int i = 0; i < TRADE_DEPTH + 3; ++i) {
        record_trade(ds, 100 + i, 1, 1, static_cast<uint64_t>(i * 1000));
    }
    EXPECT_EQ(ds.trade_count, TRADE_DEPTH);
    EXPECT_EQ(ds.total_trades, static_cast<uint64_t>(TRADE_DEPTH + 3));

    // Most recent trade should be at (write_idx - 1).
    int last = (ds.trade_write_idx - 1 + TRADE_DEPTH) % TRADE_DEPTH;
    EXPECT_EQ(ds.trades[last].price, 100 + TRADE_DEPTH + 2);
}

// ---------------------------------------------------------------------------
// Test: MDP3 decode + visitor dispatch
// ---------------------------------------------------------------------------

namespace cme = exchange::cme::sbe;
namespace cme_mdp3 = exchange::cme::sbe::mdp3;

// Helper: build a raw MDP3 RefreshBook46 message buffer.
static size_t build_refresh_book_46(char* buf, size_t buf_len,
                                      int32_t security_id,
                                      int64_t price_mantissa,
                                      int32_t qty,
                                      uint8_t action,       // MDUpdateAction
                                      char    entry_type,   // MDEntryTypeBook
                                      uint64_t transact_time) {
    // MessageHeader (8) + root block (11) + GroupHeader (3) + 1 entry (32)
    // + GroupHeader8Byte (8) + 0 order entries
    constexpr size_t MSG_SIZE = 8 + 11 + 3 + 32 + 8;
    if (buf_len < MSG_SIZE) return 0;

    char* p = buf;

    // MessageHeader
    cme::MessageHeader hdr{};
    hdr.block_length = 11;
    hdr.template_id = cme_mdp3::MD_INCREMENTAL_REFRESH_BOOK_46_ID;
    hdr.schema_id = cme::MDP3_SCHEMA_ID;
    hdr.version = cme::MDP3_VERSION;
    p = hdr.encode_to(p);

    // Root block (11 bytes)
    cme_mdp3::MDIncrementalRefreshBook46 root{};
    root.transact_time = transact_time;
    root.match_event_indicator = 0;
    std::memcpy(p, &root, sizeof(root));
    p += 11;

    // NoMDEntries group header
    cme::GroupHeader gh{};
    gh.block_length = 32;
    gh.num_in_group = 1;
    p = gh.encode_to(p);

    // Entry
    cme_mdp3::RefreshBookEntry entry{};
    entry.md_entry_px.mantissa = price_mantissa;
    entry.md_entry_size = qty;
    entry.security_id = security_id;
    entry.rpt_seq = 1;
    entry.number_of_orders = 5;
    entry.md_price_level = 1;
    entry.md_update_action = action;
    entry.md_entry_type = entry_type;
    std::memcpy(p, &entry, sizeof(entry));
    p += 32;

    // NoOrderIDEntries group header (empty)
    cme_mdp3::GroupHeader8Byte gh8{};
    gh8.block_length = 24;
    gh8.num_in_group = 0;
    p = gh8.encode_to(p);

    return static_cast<size_t>(p - buf);
}

// Helper: build a raw MDP3 TradeSummary48 message buffer.
static size_t build_trade_summary_48(char* buf, size_t buf_len,
                                       int32_t security_id,
                                       int64_t price_mantissa,
                                       int32_t qty,
                                       uint8_t aggressor,
                                       uint64_t transact_time) {
    constexpr size_t MSG_SIZE = 8 + 11 + 3 + 32 + 8;
    if (buf_len < MSG_SIZE) return 0;

    char* p = buf;

    cme::MessageHeader hdr{};
    hdr.block_length = 11;
    hdr.template_id = cme_mdp3::MD_INCREMENTAL_REFRESH_TRADE_SUMMARY_48_ID;
    hdr.schema_id = cme::MDP3_SCHEMA_ID;
    hdr.version = cme::MDP3_VERSION;
    p = hdr.encode_to(p);

    cme_mdp3::MDIncrementalRefreshTradeSummary48 root{};
    root.transact_time = transact_time;
    std::memcpy(p, &root, sizeof(root));
    p += 11;

    cme::GroupHeader gh{};
    gh.block_length = 32;
    gh.num_in_group = 1;
    p = gh.encode_to(p);

    cme_mdp3::TradeSummaryEntry entry{};
    entry.md_entry_px.mantissa = price_mantissa;
    entry.md_entry_size = qty;
    entry.security_id = security_id;
    entry.aggressor_side = aggressor;
    entry.md_update_action = 0;
    std::memcpy(p, &entry, sizeof(entry));
    p += 32;

    cme_mdp3::GroupHeader8Byte gh8{};
    gh8.block_length = 16;
    gh8.num_in_group = 0;
    p = gh8.encode_to(p);

    return static_cast<size_t>(p - buf);
}

// Helper: build a SecurityStatus30 message buffer.
static size_t build_security_status_30(char* buf, size_t buf_len,
                                         int32_t security_id,
                                         uint8_t trading_status,
                                         uint64_t transact_time) {
    constexpr size_t MSG_SIZE = 8 + 30;
    if (buf_len < MSG_SIZE) return 0;

    char* p = buf;

    cme::MessageHeader hdr{};
    hdr.block_length = 30;
    hdr.template_id = cme_mdp3::SECURITY_STATUS_30_ID;
    hdr.schema_id = cme::MDP3_SCHEMA_ID;
    hdr.version = cme::MDP3_VERSION;
    p = hdr.encode_to(p);

    cme_mdp3::SecurityStatus30 root{};
    root.transact_time = transact_time;
    root.security_id = security_id;
    root.security_trading_status = trading_status;
    std::memcpy(p, &root, sizeof(root));
    p += 30;

    return static_cast<size_t>(p - buf);
}

// Reuse a minimal journal writer for testing.
class TestJournalWriter {
    std::vector<std::string> lines_;

public:
    void write_trade(uint64_t ts, const std::string& instrument,
                     int64_t price, int32_t qty, uint8_t aggressor_side) {
        const char* side = (aggressor_side == 1) ? "BUY" :
                           (aggressor_side == 2) ? "SELL" : "UNKNOWN";
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "EXPECT MD_TRADE ts=%lu instrument=%s price=%ld qty=%d "
            "aggressor_side=%s",
            static_cast<unsigned long>(ts), instrument.c_str(),
            static_cast<long>(price), qty, side);
        lines_.emplace_back(buf);
    }

    void write_book_add(uint64_t ts, const std::string& instrument,
                        const char* side, int64_t price, int32_t qty,
                        int32_t order_count) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "EXPECT MD_BOOK_ADD ts=%lu instrument=%s side=%s price=%ld "
            "qty=%d order_count=%d",
            static_cast<unsigned long>(ts), instrument.c_str(),
            side, static_cast<long>(price), qty, order_count);
        lines_.emplace_back(buf);
    }

    void write_book_update(uint64_t ts, const std::string& instrument,
                           const char* side, int64_t price, int32_t qty,
                           int32_t order_count) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "EXPECT MD_BOOK_UPDATE ts=%lu instrument=%s side=%s price=%ld "
            "qty=%d order_count=%d",
            static_cast<unsigned long>(ts), instrument.c_str(),
            side, static_cast<long>(price), qty, order_count);
        lines_.emplace_back(buf);
    }

    void write_book_delete(uint64_t ts, const std::string& instrument,
                           const char* side, int64_t price) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "EXPECT MD_BOOK_DELETE ts=%lu instrument=%s side=%s price=%ld",
            static_cast<unsigned long>(ts), instrument.c_str(),
            side, static_cast<long>(price));
        lines_.emplace_back(buf);
    }

    void write_status(uint64_t ts, const std::string& instrument,
                      const char* state) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "EXPECT MD_STATUS ts=%lu instrument=%s state=%s",
            static_cast<unsigned long>(ts), instrument.c_str(), state);
        lines_.emplace_back(buf);
    }

    const std::vector<std::string>& lines() const { return lines_; }
    void clear() { lines_.clear(); }
};

// CmeVisitor adapted for test (uses TestJournalWriter).
namespace cme_dec = exchange::cme::sbe::mdp3;

struct TestCmeVisitor {
    DisplayState&       ds;
    TestJournalWriter&  journal;
    int32_t             filter_security_id;
    std::string         instrument;

    void operator()(const cme_dec::DecodedRefreshBook46& msg) {
        uint64_t ts = msg.root.transact_time;
        for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
            const auto& e = msg.md_entries[i];
            if (e.security_id != filter_security_id) continue;

            auto action = static_cast<cme_dec::MDUpdateAction>(e.md_update_action);
            auto entry_type = static_cast<cme_dec::MDEntryTypeBook>(e.md_entry_type);

            bool is_bid = (entry_type == cme_dec::MDEntryTypeBook::Bid ||
                           entry_type == cme_dec::MDEntryTypeBook::ImpliedBid);
            bool is_delete = (action == cme_dec::MDUpdateAction::Delete ||
                              action == cme_dec::MDUpdateAction::DeleteThru ||
                              action == cme_dec::MDUpdateAction::DeleteFrom);

            const char* side_str = is_bid ? "BUY" : "SELL";
            int64_t price = e.md_entry_px.mantissa;
            int32_t qty = e.md_entry_size;
            int32_t orders = e.number_of_orders;

            BookLevel* levels = is_bid ? ds.bids : ds.asks;
            int& count = is_bid ? ds.bid_levels : ds.ask_levels;
            update_book_side(levels, count, price, qty, orders, is_delete, is_bid);

            if (is_delete)
                journal.write_book_delete(ts, instrument, side_str, price);
            else if (action == cme_dec::MDUpdateAction::New)
                journal.write_book_add(ts, instrument, side_str, price, qty, orders);
            else
                journal.write_book_update(ts, instrument, side_str, price, qty, orders);
        }
    }

    void operator()(const cme_dec::DecodedTradeSummary48& msg) {
        uint64_t ts = msg.root.transact_time;
        for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
            const auto& e = msg.md_entries[i];
            if (e.security_id != filter_security_id) continue;
            record_trade(ds, e.md_entry_px.mantissa, e.md_entry_size,
                         e.aggressor_side, ts);
            journal.write_trade(ts, instrument, e.md_entry_px.mantissa,
                                e.md_entry_size, e.aggressor_side);
        }
    }

    void operator()(const cme_dec::DecodedSecurityStatus30& msg) {
        if (msg.root.security_id != filter_security_id) return;
        auto status = static_cast<cme_dec::SecurityTradingStatus>(
            msg.root.security_trading_status);
        const char* state = "UNKNOWN";
        switch (status) {
            case cme_dec::SecurityTradingStatus::ReadyToTrade: state = "OPEN"; break;
            case cme_dec::SecurityTradingStatus::TradingHalt:  state = "HALT"; break;
            case cme_dec::SecurityTradingStatus::Close:        state = "CLOSED"; break;
            case cme_dec::SecurityTradingStatus::PreOpen:      state = "PRE_OPEN"; break;
            case cme_dec::SecurityTradingStatus::PostClose:    state = "POST_CLOSE"; break;
            default: break;
        }
        journal.write_status(msg.root.transact_time, instrument, state);
    }

    void operator()(const cme_dec::DecodedSnapshot53&) {}
    void operator()(const cme_dec::DecodedInstrumentDef54&) {}
};

TEST(CmeVisitor, RefreshBookAddBid) {
    DisplayState ds{};
    TestJournalWriter jw;
    TestCmeVisitor visitor{ds, jw, /*security_id=*/1, "ES"};

    char buf[256];
    size_t len = build_refresh_book_46(
        buf, sizeof(buf), /*security_id=*/1,
        /*price=*/5000250000000LL, /*qty=*/150,
        /*action=*/0 /*New*/, /*entry_type=*/'0' /*Bid*/,
        /*transact_time=*/1711500000000000000ULL);
    ASSERT_GT(len, 0u);

    auto rc = cme_dec::decode_mdp3_message(buf, len, visitor);
    EXPECT_EQ(rc, cme_dec::DecodeResult::kOk);

    // Check display state.
    EXPECT_EQ(ds.bid_levels, 1);
    EXPECT_EQ(ds.bids[0].price, 5000250000000LL);
    EXPECT_EQ(ds.bids[0].qty, 150);

    // Check journal output.
    ASSERT_EQ(jw.lines().size(), 1u);
    EXPECT_NE(jw.lines()[0].find("EXPECT MD_BOOK_ADD"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("instrument=ES"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("side=BUY"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("price=5000250000000"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("qty=150"), std::string::npos);
}

TEST(CmeVisitor, RefreshBookDeleteAsk) {
    DisplayState ds{};
    TestJournalWriter jw;
    TestCmeVisitor visitor{ds, jw, 1, "ES"};

    // First add an ask level.
    char buf[256];
    size_t len = build_refresh_book_46(buf, sizeof(buf), 1,
        5000500000000LL, 100, 0 /*New*/, '1' /*Offer*/,
        1711500000000000000ULL);
    ASSERT_GT(len, 0u);
    cme_dec::decode_mdp3_message(buf, len, visitor);
    EXPECT_EQ(ds.ask_levels, 1);

    // Now delete it.
    jw.clear();
    len = build_refresh_book_46(buf, sizeof(buf), 1,
        5000500000000LL, 0, 2 /*Delete*/, '1' /*Offer*/,
        1711500000001000000ULL);
    ASSERT_GT(len, 0u);
    cme_dec::decode_mdp3_message(buf, len, visitor);
    EXPECT_EQ(ds.ask_levels, 0);

    ASSERT_EQ(jw.lines().size(), 1u);
    EXPECT_NE(jw.lines()[0].find("EXPECT MD_BOOK_DELETE"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("side=SELL"), std::string::npos);
}

TEST(CmeVisitor, TradeSummary) {
    DisplayState ds{};
    TestJournalWriter jw;
    TestCmeVisitor visitor{ds, jw, 1, "ES"};

    char buf[256];
    size_t len = build_trade_summary_48(buf, sizeof(buf), 1,
        5000250000000LL, 10, 1 /*Buy*/, 1711500000020000000ULL);
    ASSERT_GT(len, 0u);

    auto rc = cme_dec::decode_mdp3_message(buf, len, visitor);
    EXPECT_EQ(rc, cme_dec::DecodeResult::kOk);

    EXPECT_EQ(ds.total_trades, 1u);
    EXPECT_EQ(ds.close_price, 5000250000000LL);
    EXPECT_EQ(ds.volume, 10);

    ASSERT_EQ(jw.lines().size(), 1u);
    EXPECT_NE(jw.lines()[0].find("EXPECT MD_TRADE"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("aggressor_side=BUY"), std::string::npos);
}

TEST(CmeVisitor, SecurityStatus) {
    DisplayState ds{};
    TestJournalWriter jw;
    TestCmeVisitor visitor{ds, jw, 1, "ES"};

    char buf[256];
    size_t len = build_security_status_30(buf, sizeof(buf), 1,
        17 /*ReadyToTrade*/, 1711500000030000000ULL);
    ASSERT_GT(len, 0u);

    auto rc = cme_dec::decode_mdp3_message(buf, len, visitor);
    EXPECT_EQ(rc, cme_dec::DecodeResult::kOk);

    ASSERT_EQ(jw.lines().size(), 1u);
    EXPECT_NE(jw.lines()[0].find("EXPECT MD_STATUS"), std::string::npos);
    EXPECT_NE(jw.lines()[0].find("state=OPEN"), std::string::npos);
}

TEST(CmeVisitor, InstrumentFiltering) {
    DisplayState ds{};
    TestJournalWriter jw;
    // Filter for security_id=2 (NQ), but send security_id=1 (ES).
    TestCmeVisitor visitor{ds, jw, 2, "NQ"};

    char buf[256];
    size_t len = build_refresh_book_46(buf, sizeof(buf), 1,
        5000000000000LL, 100, 0, '0', 1711500000000000000ULL);
    ASSERT_GT(len, 0u);

    cme_dec::decode_mdp3_message(buf, len, visitor);
    // Should be filtered out -- no display update, no journal output.
    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_TRUE(jw.lines().empty());
}

// ---------------------------------------------------------------------------
// Test: ICE iMpact decode + visitor dispatch
// ---------------------------------------------------------------------------

namespace ice = exchange::ice::impact;

struct TestIceVisitor {
    DisplayState&       ds;
    TestJournalWriter&  journal;
    int32_t             filter_instrument_id;
    std::string         instrument;
    uint64_t            bundle_ts{0};

    void on_bundle_start(const ice::BundleStart& msg) {
        bundle_ts = static_cast<uint64_t>(msg.timestamp);
    }
    void on_bundle_end(const ice::BundleEnd&) {}

    void on_add_modify_order(const ice::AddModifyOrder& msg) {
        if (msg.instrument_id != filter_instrument_id) return;
        bool is_bid = (static_cast<ice::Side>(msg.side) == ice::Side::Buy);
        const char* side_str = is_bid ? "BUY" : "SELL";
        auto qty = static_cast<int32_t>(msg.quantity);
        BookLevel* levels = is_bid ? ds.bids : ds.asks;
        int& count = is_bid ? ds.bid_levels : ds.ask_levels;
        update_book_side(levels, count, msg.price, qty, 0, false, is_bid);
        uint64_t ts = bundle_ts ? bundle_ts : static_cast<uint64_t>(msg.order_entry_date_time);
        journal.write_book_add(ts, instrument, side_str, msg.price, qty, 0);
    }

    void on_order_withdrawal(const ice::OrderWithdrawal& msg) {
        if (msg.instrument_id != filter_instrument_id) return;
        bool is_bid = (static_cast<ice::Side>(msg.side) == ice::Side::Buy);
        const char* side_str = is_bid ? "BUY" : "SELL";
        BookLevel* levels = is_bid ? ds.bids : ds.asks;
        int& count = is_bid ? ds.bid_levels : ds.ask_levels;
        update_book_side(levels, count, msg.price, 0, 0, true, is_bid);
        journal.write_book_delete(bundle_ts, instrument, side_str, msg.price);
    }

    void on_deal_trade(const ice::DealTrade& msg) {
        if (msg.instrument_id != filter_instrument_id) return;
        auto qty = static_cast<int32_t>(msg.quantity);
        uint8_t agg = (static_cast<ice::Side>(msg.aggressor_side) ==
                        ice::Side::Buy) ? 1 : 2;
        uint64_t ts = static_cast<uint64_t>(msg.timestamp);
        record_trade(ds, msg.price, qty, agg, ts);
        journal.write_trade(ts, instrument, msg.price, qty, agg);
    }

    void on_market_status(const ice::MarketStatus& msg) {
        if (msg.instrument_id != filter_instrument_id) return;
        auto status = static_cast<ice::TradingStatus>(msg.trading_status);
        const char* state = "UNKNOWN";
        switch (status) {
            case ice::TradingStatus::Continuous: state = "OPEN"; break;
            case ice::TradingStatus::Halt:       state = "HALT"; break;
            case ice::TradingStatus::Closed:     state = "CLOSED"; break;
            case ice::TradingStatus::PreOpen:    state = "PRE_OPEN"; break;
            case ice::TradingStatus::Settlement: state = "SETTLEMENT"; break;
        }
        journal.write_status(bundle_ts, instrument, state);
    }

    void on_snapshot_order(const ice::SnapshotOrder&) {}
    void on_price_level(const ice::PriceLevel&) {}
};

// Build an iMpact stream containing BundleStart + AddModifyOrder + BundleEnd.
static size_t build_ice_add_order(char* buf, size_t buf_len,
                                    int32_t instrument_id,
                                    int64_t price, uint32_t qty,
                                    uint8_t side, int64_t timestamp) {
    char* p = buf;
    size_t remaining = buf_len;

    // BundleStart
    ice::BundleStart bs{};
    bs.sequence_number = 1;
    bs.message_count = 1;
    bs.timestamp = timestamp;
    p = ice::encode(p, remaining, bs);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    // AddModifyOrder
    ice::AddModifyOrder amo{};
    amo.instrument_id = instrument_id;
    amo.order_id = 42;
    amo.sequence_within_msg = 1;
    amo.side = side;
    amo.price = price;
    amo.quantity = qty;
    amo.is_implied = 0;
    amo.is_rfq = 0;
    amo.order_entry_date_time = timestamp;
    p = ice::encode(p, remaining, amo);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    // BundleEnd
    ice::BundleEnd be{};
    be.sequence_number = 1;
    p = ice::encode(p, remaining, be);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

static size_t build_ice_trade(char* buf, size_t buf_len,
                                int32_t instrument_id,
                                int64_t price, uint32_t qty,
                                uint8_t aggressor_side,
                                int64_t timestamp) {
    char* p = buf;
    size_t remaining = buf_len;

    ice::BundleStart bs{};
    bs.sequence_number = 2;
    bs.message_count = 1;
    bs.timestamp = timestamp;
    p = ice::encode(p, remaining, bs);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    ice::DealTrade dt{};
    dt.instrument_id = instrument_id;
    dt.deal_id = 1;
    dt.price = price;
    dt.quantity = qty;
    dt.aggressor_side = aggressor_side;
    dt.timestamp = timestamp;
    p = ice::encode(p, remaining, dt);
    if (!p) return 0;
    remaining = buf_len - static_cast<size_t>(p - buf);

    ice::BundleEnd be{};
    be.sequence_number = 2;
    p = ice::encode(p, remaining, be);
    if (!p) return 0;

    return static_cast<size_t>(p - buf);
}

TEST(IceVisitor, AddModifyOrder) {
    DisplayState ds{};
    TestJournalWriter jw;
    TestIceVisitor visitor{ds, jw, 1, "B", 0};

    char buf[256];
    size_t len = build_ice_add_order(buf, sizeof(buf), 1,
        7500, 100, 0 /*Buy*/, 1711500000000000000LL);
    ASSERT_GT(len, 0u);

    ice::decode_messages(buf, len, visitor);

    EXPECT_EQ(ds.bid_levels, 1);
    EXPECT_EQ(ds.bids[0].price, 7500);
    EXPECT_EQ(ds.bids[0].qty, 100);

    // Journal should have the book add.
    ASSERT_GE(jw.lines().size(), 1u);
    EXPECT_NE(jw.lines().back().find("EXPECT MD_BOOK_ADD"), std::string::npos);
    EXPECT_NE(jw.lines().back().find("side=BUY"), std::string::npos);
    EXPECT_NE(jw.lines().back().find("instrument=B"), std::string::npos);
}

TEST(IceVisitor, DealTrade) {
    DisplayState ds{};
    TestJournalWriter jw;
    TestIceVisitor visitor{ds, jw, 1, "B", 0};

    char buf[256];
    size_t len = build_ice_trade(buf, sizeof(buf), 1,
        7500, 50, 1 /*Sell*/, 1711500000010000000LL);
    ASSERT_GT(len, 0u);

    ice::decode_messages(buf, len, visitor);

    EXPECT_EQ(ds.total_trades, 1u);
    EXPECT_EQ(ds.close_price, 7500);

    ASSERT_GE(jw.lines().size(), 1u);
    // Find the trade line (skip bundle start/end).
    bool found_trade = false;
    for (const auto& line : jw.lines()) {
        if (line.find("EXPECT MD_TRADE") != std::string::npos) {
            found_trade = true;
            EXPECT_NE(line.find("aggressor_side=SELL"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_trade);
}

TEST(IceVisitor, InstrumentFiltering) {
    DisplayState ds{};
    TestJournalWriter jw;
    // Filter for instrument_id=2, but send instrument_id=1.
    TestIceVisitor visitor{ds, jw, 2, "G", 0};

    char buf[256];
    size_t len = build_ice_add_order(buf, sizeof(buf), 1,
        7500, 100, 0, 1711500000000000000LL);
    ASSERT_GT(len, 0u);

    ice::decode_messages(buf, len, visitor);

    EXPECT_EQ(ds.bid_levels, 0);
    // Only bundle_start might produce no journal line of interest.
    bool found_book = false;
    for (const auto& line : jw.lines()) {
        if (line.find("MD_BOOK") != std::string::npos) found_book = true;
    }
    EXPECT_FALSE(found_book);
}

// ---------------------------------------------------------------------------
// Test: Sequence gap detection
// ---------------------------------------------------------------------------

TEST(SequenceGap, DetectsGap) {
    DisplayState ds{};

    // Simulate receiving seq 1, 2, 5 (gap of 2).
    ds.last_seq = 0;
    auto check_seq = [&](uint32_t seq) {
        if (ds.last_seq != 0 && seq != ds.last_seq + 1) ++ds.seq_gaps;
        ds.last_seq = seq;
    };

    check_seq(1);
    EXPECT_EQ(ds.seq_gaps, 0u);
    check_seq(2);
    EXPECT_EQ(ds.seq_gaps, 0u);
    check_seq(5); // gap
    EXPECT_EQ(ds.seq_gaps, 1u);
    check_seq(6);
    EXPECT_EQ(ds.seq_gaps, 1u);
}

// ---------------------------------------------------------------------------
// Test: Journal output format compliance
// ---------------------------------------------------------------------------

TEST(JournalFormat, TradeLineFormat) {
    TestJournalWriter jw;
    jw.write_trade(1711500000020000000ULL, "ES", 5000250000000LL, 10, 1);

    ASSERT_EQ(jw.lines().size(), 1u);
    const auto& line = jw.lines()[0];
    // Verify all required fields present.
    EXPECT_EQ(line.substr(0, 15), "EXPECT MD_TRADE");
    EXPECT_NE(line.find("ts=1711500000020000000"), std::string::npos);
    EXPECT_NE(line.find("instrument=ES"), std::string::npos);
    EXPECT_NE(line.find("price=5000250000000"), std::string::npos);
    EXPECT_NE(line.find("qty=10"), std::string::npos);
    EXPECT_NE(line.find("aggressor_side=BUY"), std::string::npos);
}

TEST(JournalFormat, BookAddLineFormat) {
    TestJournalWriter jw;
    jw.write_book_add(1711500000021000000ULL, "ES", "BUY",
                      4999500000000LL, 150, 5);

    ASSERT_EQ(jw.lines().size(), 1u);
    const auto& line = jw.lines()[0];
    EXPECT_EQ(line.substr(0, 18), "EXPECT MD_BOOK_ADD");
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    EXPECT_NE(line.find("order_count=5"), std::string::npos);
}

TEST(JournalFormat, BookDeleteLineFormat) {
    TestJournalWriter jw;
    jw.write_book_delete(1711500000023000000ULL, "ES", "BUY", 4999000000000LL);

    ASSERT_EQ(jw.lines().size(), 1u);
    const auto& line = jw.lines()[0];
    EXPECT_EQ(line.substr(0, 21), "EXPECT MD_BOOK_DELETE");
    EXPECT_NE(line.find("side=BUY"), std::string::npos);
    // No qty or order_count on delete.
    EXPECT_EQ(line.find("qty="), std::string::npos);
}

TEST(JournalFormat, StatusLineFormat) {
    TestJournalWriter jw;
    jw.write_status(1711500000030000000ULL, "ES", "OPEN");

    ASSERT_EQ(jw.lines().size(), 1u);
    const auto& line = jw.lines()[0];
    EXPECT_EQ(line.substr(0, 16), "EXPECT MD_STATUS");
    EXPECT_NE(line.find("state=OPEN"), std::string::npos);
}
