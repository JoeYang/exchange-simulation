// exchange-observer: market data subscriber with ANSI terminal display and
// journal output for post-trade reconciliation.
//
// Joins a UDP multicast group, decodes CME MDP3 or ICE iMpact messages,
// displays a 5-level order book with trade tape and OHLCV stats, and
// optionally writes EXPECT-style journal lines for each decoded event.

#include "cme/cme_products.h"
#include "cme/codec/mdp3_decoder.h"
#include "ice/ice_products.h"
#include "ice/impact/impact_decoder.h"
#include "tools/cme_secdef.h"
#include "tools/ice_secdef.h"
#include "tools/instrument_info.h"
#include "tools/udp_multicast.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int /*sig*/) { g_running = 0; }

// ---------------------------------------------------------------------------
// CLI configuration
// ---------------------------------------------------------------------------

struct ObserverConfig {
    std::string exchange;     // "cme" or "ice"
    std::string group;        // multicast group address
    uint16_t    port{0};      // multicast port
    std::string instrument;   // symbol filter (e.g. "ES", "B")
    std::string journal_path; // output journal file (empty = no journal)
    bool        transitions{false}; // print book transitions

    // Secdef auto-discovery (optional).
    std::string secdef_group{"239.0.31.3"};  // CME secdef multicast group
    uint16_t    secdef_port{14312};          // CME secdef multicast port
    bool        auto_discover{false};        // enable secdef discovery
};

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --exchange cme|ice --group ADDR --port PORT\n"
        "          [--instrument SYMBOL] [--journal FILE]\n"
        "          [--auto-discover] [--secdef-group ADDR] [--secdef-port PORT]\n"
        "\n"
        "  --exchange       cme or ice\n"
        "  --group          Multicast group address (e.g. 239.0.31.1)\n"
        "  --port           Multicast port\n"
        "  --instrument     Instrument symbol to filter (e.g. ES, B)\n"
        "  --journal        Output journal file path (optional)\n"
        "  --transitions    Print real-time book transitions\n"
        "  --auto-discover  Discover instruments via secdef before observing\n"
        "  --secdef-group   Secdef multicast group (default: 239.0.31.3)\n"
        "  --secdef-port    Secdef multicast port (default: 14312)\n"
        "  -h, --help       Show this message\n",
        prog);
}

static bool parse_args(int argc, char* argv[], ObserverConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--exchange") == 0) && i + 1 < argc) {
            cfg.exchange = argv[++i];
        } else if ((std::strcmp(argv[i], "--group") == 0) && i + 1 < argc) {
            cfg.group = argv[++i];
        } else if ((std::strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((std::strcmp(argv[i], "--instrument") == 0) && i + 1 < argc) {
            cfg.instrument = argv[++i];
        } else if ((std::strcmp(argv[i], "--journal") == 0) && i + 1 < argc) {
            cfg.journal_path = argv[++i];
        } else if (std::strcmp(argv[i], "--transitions") == 0) {
            cfg.transitions = true;
        } else if (std::strcmp(argv[i], "--auto-discover") == 0) {
            cfg.auto_discover = true;
        } else if ((std::strcmp(argv[i], "--secdef-group") == 0) && i + 1 < argc) {
            cfg.secdef_group = argv[++i];
        } else if ((std::strcmp(argv[i], "--secdef-port") == 0) && i + 1 < argc) {
            cfg.secdef_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-h") == 0 ||
                   std::strcmp(argv[i], "--help") == 0) {
            return false;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }
    }

    if (cfg.exchange.empty() || cfg.group.empty() || cfg.port == 0) {
        std::fprintf(stderr, "Error: --exchange, --group, and --port "
                             "are required.\n");
        return false;
    }
    // --instrument is required unless --auto-discover is set.
    if (cfg.instrument.empty() && !cfg.auto_discover) {
        std::fprintf(stderr, "Error: --instrument is required "
                             "(or use --auto-discover).\n");
        return false;
    }
    if (cfg.exchange != "cme" && cfg.exchange != "ice") {
        std::fprintf(stderr, "Error: --exchange must be 'cme' or 'ice'.\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Display state: 5-level book, last 10 trades, OHLCV, message counters.
// ---------------------------------------------------------------------------

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
    uint8_t  aggressor_side{0}; // 1=Buy, 2=Sell
    uint64_t timestamp_ns{0};
};

struct DisplayState {
    BookLevel bids[BOOK_DEPTH]{};
    BookLevel asks[BOOK_DEPTH]{};
    int       bid_levels{0};
    int       ask_levels{0};

    TradeEntry trades[TRADE_DEPTH]{};
    int        trade_count{0};
    int        trade_write_idx{0}; // circular buffer index

    // OHLCV
    int64_t  open_price{0};
    int64_t  high_price{0};
    int64_t  low_price{0};
    int64_t  close_price{0};
    int64_t  volume{0};

    // Counters
    uint64_t total_messages{0};
    uint64_t messages_this_second{0};
    uint64_t msgs_per_sec{0};      // last completed second
    uint64_t decode_errors{0};
    uint64_t total_trades{0};

    // Sequence tracking
    uint32_t last_seq{0};
    uint64_t seq_gaps{0};
};

// Add a trade to the circular buffer and update OHLCV.
static void record_trade(DisplayState& ds, int64_t price, int32_t qty,
                          uint8_t aggressor, uint64_t ts) {
    auto& t = ds.trades[ds.trade_write_idx];
    t.price = price;
    t.qty = qty;
    t.aggressor_side = aggressor;
    t.timestamp_ns = ts;
    ds.trade_write_idx = (ds.trade_write_idx + 1) % TRADE_DEPTH;
    if (ds.trade_count < TRADE_DEPTH) ++ds.trade_count;
    ++ds.total_trades;

    // OHLCV
    if (ds.open_price == 0) ds.open_price = price;
    if (price > ds.high_price || ds.high_price == 0) ds.high_price = price;
    if (price < ds.low_price || ds.low_price == 0) ds.low_price = price;
    ds.close_price = price;
    ds.volume += qty;
}

// Update a book side from a single entry. Simple model: treat each level as
// a price-level slot. For New/Change, insert or update; for Delete, remove.
// This is a simplified book that maintains up to BOOK_DEPTH sorted levels.
static void update_book_side(BookLevel* levels, int& count,
                              int64_t price, int32_t qty, int32_t orders,
                              bool is_delete, bool is_bid) {
    if (is_delete) {
        // Remove the level matching this price.
        for (int i = 0; i < count; ++i) {
            if (levels[i].price == price) {
                for (int j = i; j < count - 1; ++j)
                    levels[j] = levels[j + 1];
                --count;
                levels[count] = BookLevel{};
                return;
            }
        }
        return;
    }

    // Check if the price already exists -- update in place.
    for (int i = 0; i < count; ++i) {
        if (levels[i].price == price) {
            levels[i].qty = qty;
            levels[i].order_count = orders;
            return;
        }
    }

    // Insert new level, maintaining sort order.
    // Bids: descending price. Asks: ascending price.
    BookLevel entry{price, qty, orders};
    if (count < BOOK_DEPTH) {
        levels[count] = entry;
        ++count;
    } else {
        // Replace worst level if this price is better.
        int worst = count - 1;
        bool better = is_bid ? (price > levels[worst].price)
                             : (price < levels[worst].price);
        if (!better) return;
        levels[worst] = entry;
    }

    // Sort: bids descending, asks ascending.
    if (is_bid) {
        std::sort(levels, levels + count,
                  [](const BookLevel& a, const BookLevel& b) {
                      return a.price > b.price;
                  });
    } else {
        std::sort(levels, levels + count,
                  [](const BookLevel& a, const BookLevel& b) {
                      return a.price < b.price;
                  });
    }
}

// ---------------------------------------------------------------------------
// Price formatting helpers
// ---------------------------------------------------------------------------

// Format a PRICE9 mantissa (exponent=-9) as a decimal string.
static void format_price9(char* buf, size_t len, int64_t mantissa) {
    double val = static_cast<double>(mantissa) / 1e9;
    std::snprintf(buf, len, "%.2f", val);
}

// Format a fixed-point price (PRICE_SCALE=10000, exponent=-4).
static void format_price4(char* buf, size_t len, int64_t fp) {
    double val = static_cast<double>(fp) / 10000.0;
    std::snprintf(buf, len, "%.2f", val);
}

// ---------------------------------------------------------------------------
// Transition logger: real-time book change output
// ---------------------------------------------------------------------------

class TransitionLogger {
    FILE* out_{nullptr};
    bool  color_{false};

    // ANSI color codes
    static constexpr const char* kGreen  = "\033[32m";
    static constexpr const char* kRed    = "\033[31m";
    static constexpr const char* kYellow = "\033[33m";
    static constexpr const char* kCyan   = "\033[36m";
    static constexpr const char* kReset  = "\033[0m";

    void write_timestamp() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_buf;
        localtime_r(&ts.tv_sec, &tm_buf);
        std::fprintf(out_, "[%02d:%02d:%02d.%09ld] ",
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                     ts.tv_nsec);
    }

    const char* bid_color() const { return color_ ? kGreen : ""; }
    const char* ask_color() const { return color_ ? kRed : ""; }
    const char* trade_color() const { return color_ ? kYellow : ""; }
    const char* status_color() const { return color_ ? kCyan : ""; }
    const char* reset() const { return color_ ? kReset : ""; }

public:
    // out: output stream. color: emit ANSI color codes.
    TransitionLogger(FILE* out, bool color)
        : out_(out), color_(color) {}

    bool enabled() const { return out_ != nullptr; }

    void log_book_add(bool is_bid, double price, int32_t qty,
                      int bid_levels, int ask_levels) {
        if (!out_) return;
        const char* side = is_bid ? "BID" : "ASK";
        int levels = is_bid ? bid_levels : ask_levels;
        const char* c = is_bid ? bid_color() : ask_color();
        write_timestamp();
        std::fprintf(out_, "%s%-3s ADD  %10.4f x %-5d (levels: %d)%s\n",
                     c, side, price, qty, levels, reset());
    }

    void log_book_update(bool is_bid, double price, int32_t qty,
                         int bid_levels, int ask_levels) {
        if (!out_) return;
        const char* side = is_bid ? "BID" : "ASK";
        int levels = is_bid ? bid_levels : ask_levels;
        const char* c = is_bid ? bid_color() : ask_color();
        write_timestamp();
        std::fprintf(out_, "%s%-3s UPD  %10.4f x %-5d (levels: %d)%s\n",
                     c, side, price, qty, levels, reset());
    }

    void log_book_delete(bool is_bid, double price,
                         int bid_levels, int ask_levels) {
        if (!out_) return;
        const char* side = is_bid ? "BID" : "ASK";
        int levels = is_bid ? bid_levels : ask_levels;
        const char* c = is_bid ? bid_color() : ask_color();
        write_timestamp();
        std::fprintf(out_, "%s%-3s DEL  %10.4f        (levels: %d)%s\n",
                     c, side, price, levels, reset());
    }

    void log_trade(double price, int32_t qty, uint8_t aggressor_side) {
        if (!out_) return;
        const char* agg = (aggressor_side == 1) ? "BUY" :
                          (aggressor_side == 2) ? "SELL" : "???";
        write_timestamp();
        std::fprintf(out_, "%sTRADE    %10.4f x %-5d aggressor=%s%s\n",
                     trade_color(), price, qty, agg, reset());
    }

    void log_status(const char* state) {
        if (!out_) return;
        write_timestamp();
        std::fprintf(out_, "%sSTATUS   %s%s\n",
                     status_color(), state, reset());
    }
};

// ---------------------------------------------------------------------------
// ANSI terminal display rendering
// ---------------------------------------------------------------------------

static void render_display(const DisplayState& ds, const std::string& symbol,
                           const std::string& exchange,
                           bool is_price9) {
    // Move cursor to home position and clear screen.
    std::fprintf(stderr, "\033[H\033[2J");

    std::fprintf(stderr,
        "%s  Live Market Data (%s)            msgs/sec: %lu\n",
        symbol.c_str(), exchange.c_str(),
        static_cast<unsigned long>(ds.msgs_per_sec));
    std::fprintf(stderr,
        "-----------------------------------------------------------\n");

    // Book header
    std::fprintf(stderr,
        "        BID                    ASK\n"
        "  Qty    Price      |    Price     Qty\n");

    auto fmt = [&](int64_t p, char* b, size_t l) {
        if (is_price9) format_price9(b, l, p);
        else           format_price4(b, l, p);
    };

    int depth = std::max(ds.bid_levels, ds.ask_levels);
    if (depth > BOOK_DEPTH) depth = BOOK_DEPTH;
    for (int i = 0; i < depth; ++i) {
        char bid_price[32] = "", ask_price[32] = "";
        if (i < ds.bid_levels) fmt(ds.bids[i].price, bid_price, sizeof(bid_price));
        if (i < ds.ask_levels) fmt(ds.asks[i].price, ask_price, sizeof(ask_price));

        std::fprintf(stderr, "  %4d  %10s   |  %10s  %4d\n",
            i < ds.bid_levels ? ds.bids[i].qty : 0,
            bid_price,
            ask_price,
            i < ds.ask_levels ? ds.asks[i].qty : 0);
    }

    // Trades
    std::fprintf(stderr, "\nLast %d Trades:\n", ds.trade_count);
    for (int i = 0; i < ds.trade_count; ++i) {
        // Read from newest to oldest.
        int idx = (ds.trade_write_idx - 1 - i + TRADE_DEPTH) % TRADE_DEPTH;
        const auto& t = ds.trades[idx];
        char price_buf[32];
        fmt(t.price, price_buf, sizeof(price_buf));

        // Format timestamp as HH:MM:SS.mmm
        uint64_t ns = t.timestamp_ns;
        uint64_t secs = ns / 1000000000ULL;
        uint32_t ms = static_cast<uint32_t>((ns % 1000000000ULL) / 1000000ULL);
        uint32_t h = static_cast<uint32_t>((secs / 3600) % 24);
        uint32_t m = static_cast<uint32_t>((secs / 60) % 60);
        uint32_t s = static_cast<uint32_t>(secs % 60);

        const char* side_str = (t.aggressor_side == 1) ? "BUY " :
                               (t.aggressor_side == 2) ? "SELL" : "----";
        std::fprintf(stderr, "  %10s  x%-4d  %s  %02u:%02u:%02u.%03u\n",
                     price_buf, t.qty, side_str, h, m, s, ms);
    }

    // OHLCV
    char o[32], hi[32], lo[32], c[32];
    fmt(ds.open_price, o, sizeof(o));
    fmt(ds.high_price, hi, sizeof(hi));
    fmt(ds.low_price, lo, sizeof(lo));
    fmt(ds.close_price, c, sizeof(c));
    std::fprintf(stderr, "\nOHLCV: O=%s H=%s L=%s C=%s V=%ld\n",
                 o, hi, lo, c, static_cast<long>(ds.volume));

    // Stats
    std::fprintf(stderr,
        "Total msgs: %lu  Trades: %lu  Errors: %lu  Seq gaps: %lu\n",
        static_cast<unsigned long>(ds.total_messages),
        static_cast<unsigned long>(ds.total_trades),
        static_cast<unsigned long>(ds.decode_errors),
        static_cast<unsigned long>(ds.seq_gaps));
}

// ---------------------------------------------------------------------------
// Journal writer
// ---------------------------------------------------------------------------

class JournalWriter {
    FILE* fd_{nullptr};

public:
    explicit JournalWriter(const std::string& path) {
        if (!path.empty()) {
            fd_ = std::fopen(path.c_str(), "w");
            if (!fd_) {
                std::fprintf(stderr, "Error: cannot open journal file: %s\n",
                             path.c_str());
            }
        }
    }

    ~JournalWriter() { close(); }

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    bool is_open() const { return fd_ != nullptr; }

    void write_trade(uint64_t ts, const std::string& instrument,
                     int64_t price, int32_t qty, uint8_t aggressor_side) {
        if (!fd_) return;
        const char* side = (aggressor_side == 1) ? "BUY" :
                           (aggressor_side == 2) ? "SELL" : "UNKNOWN";
        std::fprintf(fd_,
            "EXPECT MD_TRADE ts=%lu instrument=%s price=%ld qty=%d "
            "aggressor_side=%s\n",
            static_cast<unsigned long>(ts), instrument.c_str(),
            static_cast<long>(price), qty, side);
    }

    void write_book_add(uint64_t ts, const std::string& instrument,
                        const char* side, int64_t price, int32_t qty,
                        int32_t order_count) {
        if (!fd_) return;
        std::fprintf(fd_,
            "EXPECT MD_BOOK_ADD ts=%lu instrument=%s side=%s price=%ld "
            "qty=%d order_count=%d\n",
            static_cast<unsigned long>(ts), instrument.c_str(),
            side, static_cast<long>(price), qty, order_count);
    }

    void write_book_update(uint64_t ts, const std::string& instrument,
                           const char* side, int64_t price, int32_t qty,
                           int32_t order_count) {
        if (!fd_) return;
        std::fprintf(fd_,
            "EXPECT MD_BOOK_UPDATE ts=%lu instrument=%s side=%s price=%ld "
            "qty=%d order_count=%d\n",
            static_cast<unsigned long>(ts), instrument.c_str(),
            side, static_cast<long>(price), qty, order_count);
    }

    void write_book_delete(uint64_t ts, const std::string& instrument,
                           const char* side, int64_t price) {
        if (!fd_) return;
        std::fprintf(fd_,
            "EXPECT MD_BOOK_DELETE ts=%lu instrument=%s side=%s price=%ld\n",
            static_cast<unsigned long>(ts), instrument.c_str(),
            side, static_cast<long>(price));
    }

    void write_status(uint64_t ts, const std::string& instrument,
                      const char* state) {
        if (!fd_) return;
        std::fprintf(fd_,
            "EXPECT MD_STATUS ts=%lu instrument=%s state=%s\n",
            static_cast<unsigned long>(ts), instrument.c_str(), state);
    }

    void flush() {
        if (fd_) std::fflush(fd_);
    }

    void close() {
        if (fd_) { std::fflush(fd_); std::fclose(fd_); fd_ = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// CME MDP3 visitor
// ---------------------------------------------------------------------------

namespace cme_ns = exchange::cme::sbe::mdp3;

struct CmeVisitor {
    DisplayState&      ds;
    JournalWriter&     journal;
    TransitionLogger&  transitions;
    int32_t            filter_security_id;
    std::string        instrument;

    void operator()(const cme_ns::DecodedRefreshBook46& msg) {
        uint64_t ts = msg.root.transact_time;
        for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
            const auto& e = msg.md_entries[i];
            if (e.security_id != filter_security_id) continue;

            auto action = static_cast<cme_ns::MDUpdateAction>(e.md_update_action);
            auto entry_type = static_cast<cme_ns::MDEntryTypeBook>(e.md_entry_type);

            bool is_bid = (entry_type == cme_ns::MDEntryTypeBook::Bid ||
                           entry_type == cme_ns::MDEntryTypeBook::ImpliedBid);
            bool is_delete = (action == cme_ns::MDUpdateAction::Delete ||
                              action == cme_ns::MDUpdateAction::DeleteThru ||
                              action == cme_ns::MDUpdateAction::DeleteFrom);

            const char* side_str = is_bid ? "BUY" : "SELL";
            int64_t price = e.md_entry_px.mantissa;
            int32_t qty = e.md_entry_size;
            int32_t orders = e.number_of_orders;

            BookLevel* levels = is_bid ? ds.bids : ds.asks;
            int& count = is_bid ? ds.bid_levels : ds.ask_levels;
            update_book_side(levels, count, price, qty, orders,
                             is_delete, is_bid);

            // CME PRICE9: mantissa with exponent=-9.
            double display_price = static_cast<double>(price) / 1e9;

            // Journal
            if (is_delete) {
                journal.write_book_delete(ts, instrument, side_str, price);
                transitions.log_book_delete(is_bid, display_price,
                                            ds.bid_levels, ds.ask_levels);
            } else if (action == cme_ns::MDUpdateAction::New) {
                journal.write_book_add(ts, instrument, side_str, price,
                                       qty, orders);
                transitions.log_book_add(is_bid, display_price, qty,
                                         ds.bid_levels, ds.ask_levels);
            } else {
                journal.write_book_update(ts, instrument, side_str, price,
                                          qty, orders);
                transitions.log_book_update(is_bid, display_price, qty,
                                            ds.bid_levels, ds.ask_levels);
            }
        }
    }

    void operator()(const cme_ns::DecodedTradeSummary48& msg) {
        uint64_t ts = msg.root.transact_time;
        for (uint8_t i = 0; i < msg.num_md_entries; ++i) {
            const auto& e = msg.md_entries[i];
            if (e.security_id != filter_security_id) continue;

            record_trade(ds, e.md_entry_px.mantissa, e.md_entry_size,
                         e.aggressor_side, ts);
            journal.write_trade(ts, instrument, e.md_entry_px.mantissa,
                                e.md_entry_size, e.aggressor_side);
            double display_price = static_cast<double>(e.md_entry_px.mantissa) / 1e9;
            transitions.log_trade(display_price, e.md_entry_size,
                                  e.aggressor_side);
        }
    }

    void operator()(const cme_ns::DecodedSecurityStatus30& msg) {
        if (msg.root.security_id != filter_security_id) return;

        auto status = static_cast<cme_ns::SecurityTradingStatus>(
            msg.root.security_trading_status);
        const char* state = "UNKNOWN";
        switch (status) {
            case cme_ns::SecurityTradingStatus::ReadyToTrade: state = "OPEN"; break;
            case cme_ns::SecurityTradingStatus::TradingHalt:  state = "HALT"; break;
            case cme_ns::SecurityTradingStatus::Close:        state = "CLOSED"; break;
            case cme_ns::SecurityTradingStatus::PreOpen:      state = "PRE_OPEN"; break;
            case cme_ns::SecurityTradingStatus::PostClose:    state = "POST_CLOSE"; break;
            default: break;
        }
        journal.write_status(msg.root.transact_time, instrument, state);
        transitions.log_status(state);
    }

    // Snapshot and instrument def -- update book from snapshot, log but no journal.
    void operator()(const cme_ns::DecodedSnapshot53& /*msg*/) {}
    void operator()(const cme_ns::DecodedInstrumentDef54& /*msg*/) {}
};

// ---------------------------------------------------------------------------
// ICE iMpact visitor
// ---------------------------------------------------------------------------

namespace ice_ns = exchange::ice::impact;

struct IceVisitor {
    DisplayState&      ds;
    JournalWriter&     journal;
    TransitionLogger&  transitions;
    int32_t            filter_instrument_id;
    std::string        instrument;
    uint64_t           bundle_ts{0}; // timestamp from most recent BundleStart

    void on_bundle_start(const ice_ns::BundleStart& msg) {
        bundle_ts = static_cast<uint64_t>(msg.timestamp);
    }
    void on_bundle_end(const ice_ns::BundleEnd& /*msg*/) {}

    void on_add_modify_order(const ice_ns::AddModifyOrder& msg) {
        if (msg.instrument_id != filter_instrument_id) return;

        bool is_bid = (static_cast<ice_ns::Side>(msg.side) == ice_ns::Side::Buy);
        const char* side_str = is_bid ? "BUY" : "SELL";
        int64_t price = msg.price;
        auto qty = static_cast<int32_t>(msg.quantity);

        BookLevel* levels = is_bid ? ds.bids : ds.asks;
        int& count = is_bid ? ds.bid_levels : ds.ask_levels;
        update_book_side(levels, count, price, qty, 0, false, is_bid);

        uint64_t ts = (bundle_ts != 0)
            ? bundle_ts
            : static_cast<uint64_t>(msg.order_entry_date_time);
        journal.write_book_add(ts, instrument, side_str, price, qty, 0);
        // ICE uses fixed-point with PRICE_SCALE=10000.
        double display_price = static_cast<double>(price) / 10000.0;
        transitions.log_book_add(is_bid, display_price, qty,
                                 ds.bid_levels, ds.ask_levels);
    }

    void on_order_withdrawal(const ice_ns::OrderWithdrawal& msg) {
        if (msg.instrument_id != filter_instrument_id) return;

        bool is_bid = (static_cast<ice_ns::Side>(msg.side) == ice_ns::Side::Buy);
        const char* side_str = is_bid ? "BUY" : "SELL";

        BookLevel* levels = is_bid ? ds.bids : ds.asks;
        int& count = is_bid ? ds.bid_levels : ds.ask_levels;
        update_book_side(levels, count, msg.price, 0, 0, true, is_bid);

        journal.write_book_delete(bundle_ts, instrument, side_str, msg.price);
        double display_price = static_cast<double>(msg.price) / 10000.0;
        transitions.log_book_delete(is_bid, display_price,
                                    ds.bid_levels, ds.ask_levels);
    }

    void on_deal_trade(const ice_ns::DealTrade& msg) {
        if (msg.instrument_id != filter_instrument_id) return;

        auto qty = static_cast<int32_t>(msg.quantity);
        // Map ICE Side enum to aggressor encoding (1=Buy, 2=Sell).
        uint8_t agg = (static_cast<ice_ns::Side>(msg.aggressor_side) ==
                        ice_ns::Side::Buy) ? 1 : 2;
        uint64_t ts = static_cast<uint64_t>(msg.timestamp);
        record_trade(ds, msg.price, qty, agg, ts);
        journal.write_trade(ts, instrument, msg.price, qty, agg);
        double display_price = static_cast<double>(msg.price) / 10000.0;
        transitions.log_trade(display_price, qty, agg);
    }

    void on_market_status(const ice_ns::MarketStatus& msg) {
        if (msg.instrument_id != filter_instrument_id) return;

        auto status = static_cast<ice_ns::TradingStatus>(msg.trading_status);
        const char* state = "UNKNOWN";
        switch (status) {
            case ice_ns::TradingStatus::Continuous:  state = "OPEN"; break;
            case ice_ns::TradingStatus::Halt:        state = "HALT"; break;
            case ice_ns::TradingStatus::Closed:      state = "CLOSED"; break;
            case ice_ns::TradingStatus::PreOpen:     state = "PRE_OPEN"; break;
            case ice_ns::TradingStatus::Settlement:  state = "SETTLEMENT"; break;
        }
        journal.write_status(bundle_ts, instrument, state);
        transitions.log_status(state);
    }

    void on_snapshot_order(const ice_ns::SnapshotOrder& /*msg*/) {}
    void on_price_level(const ice_ns::PriceLevel& /*msg*/) {}
    void on_instrument_def(const ice_ns::InstrumentDefinition& /*msg*/) {}
};

// ---------------------------------------------------------------------------
// Instrument resolution
// ---------------------------------------------------------------------------

static int32_t resolve_cme_security_id(const std::string& symbol) {
    for (const auto& p : exchange::cme::get_cme_products()) {
        if (p.symbol == symbol) return static_cast<int32_t>(p.instrument_id);
    }
    return -1;
}

static int32_t resolve_ice_instrument_id(const std::string& symbol) {
    for (const auto& p : exchange::ice::get_ice_products()) {
        if (p.symbol == symbol) return static_cast<int32_t>(p.instrument_id);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    ObserverConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    // Resolve instrument ID -- either via secdef discovery or hardcoded lookup.
    int32_t instrument_id = -1;
    bool is_price9 = (cfg.exchange == "cme");

    if (cfg.auto_discover) {
        // Create exchange-appropriate secdef consumer.
        std::unique_ptr<exchange::SecdefConsumer> consumer;
        if (cfg.exchange == "cme") {
            consumer = std::make_unique<exchange::CmeSecdefConsumer>(
                cfg.secdef_group, cfg.secdef_port);
        } else {
            // ICE secdef uses the same multicast channel as market data.
            consumer = std::make_unique<exchange::IceSecdefConsumer>(
                cfg.group, cfg.port);
        }

        std::fprintf(stderr, "Discovering instruments via secdef...\n");
        auto instruments = consumer->discover();

        if (instruments.empty()) {
            std::fprintf(stderr,
                "Error: secdef discovery found no instruments.\n");
            return 1;
        }

        // Log discovered instruments.
        std::fprintf(stderr, "Discovered %zu instrument(s):\n",
                     instruments.size());
        for (const auto& [sym, info] : instruments) {
            std::fprintf(stderr, "  %-8s id=%-6u tick=%-6ld lot=%-6ld %s\n",
                         sym.c_str(), info.security_id,
                         static_cast<long>(info.tick_size),
                         static_cast<long>(info.lot_size),
                         info.currency.c_str());
        }

        if (!cfg.instrument.empty()) {
            // Filter to the requested symbol.
            auto it = instruments.find(cfg.instrument);
            if (it == instruments.end()) {
                std::fprintf(stderr,
                    "Error: instrument '%s' not found in secdef.\n",
                    cfg.instrument.c_str());
                return 1;
            }
            instrument_id = static_cast<int32_t>(it->second.security_id);
        } else if (instruments.size() == 1) {
            // Exactly one instrument -- use it.
            const auto& [sym, info] = *instruments.begin();
            cfg.instrument = sym;
            instrument_id = static_cast<int32_t>(info.security_id);
        } else {
            std::fprintf(stderr,
                "Error: multiple instruments discovered. "
                "Use --instrument to select one.\n");
            return 1;
        }
    } else {
        // Legacy hardcoded lookup.
        if (cfg.exchange == "cme") {
            instrument_id = resolve_cme_security_id(cfg.instrument);
        } else {
            instrument_id = resolve_ice_instrument_id(cfg.instrument);
        }

        if (instrument_id < 0) {
            std::fprintf(stderr,
                "Error: unknown instrument '%s' for exchange '%s'\n",
                cfg.instrument.c_str(), cfg.exchange.c_str());
            return 1;
        }
    }

    // Open journal.
    JournalWriter journal(cfg.journal_path);

    // Set up transition logger.
    // When --transitions is set AND --journal is not set (no ANSI display),
    // print to stdout; otherwise print to stderr so it doesn't interfere
    // with the ANSI terminal display.
    FILE* trans_out = nullptr;
    if (cfg.transitions) {
        trans_out = cfg.journal_path.empty() ? stdout : stderr;
    }
    bool trans_color = (trans_out != nullptr) && isatty(fileno(trans_out));
    TransitionLogger transitions(trans_out, trans_color);

    // Join multicast group.
    exchange::UdpMulticastReceiver receiver;
    receiver.join_group(cfg.group.c_str(), cfg.port);

    // Set receive timeout to 100ms for periodic display refresh.
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    if (setsockopt(receiver.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        std::fprintf(stderr, "Warning: cannot set receive timeout\n");
    }

    // Install signal handler.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    DisplayState ds{};

    // Prepare visitors.
    CmeVisitor cme_visitor{ds, journal, transitions, instrument_id, cfg.instrument};
    IceVisitor ice_visitor{ds, journal, transitions, instrument_id, cfg.instrument, 0};

    char buf[65536]; // max UDP datagram

    auto last_display = std::chrono::steady_clock::now();
    auto last_second  = std::chrono::steady_clock::now();

    std::fprintf(stderr, "Listening on %s:%u for %s %s ...\n",
                 cfg.group.c_str(), cfg.port, cfg.exchange.c_str(),
                 cfg.instrument.c_str());

    while (g_running) {
        ssize_t n = receiver.receive(buf, sizeof(buf));

        if (n > 0) {
            ++ds.total_messages;
            ++ds.messages_this_second;

            // Strip 4-byte McastSeqHeader.
            if (static_cast<size_t>(n) < sizeof(exchange::McastSeqHeader)) {
                ++ds.decode_errors;
                continue;
            }

            exchange::McastSeqHeader seq_hdr;
            std::memcpy(&seq_hdr, buf, sizeof(seq_hdr));

            // Gap detection.
            if (ds.last_seq != 0 && seq_hdr.seq_num != ds.last_seq + 1) {
                ++ds.seq_gaps;
            }
            ds.last_seq = seq_hdr.seq_num;

            const char* payload = buf + sizeof(exchange::McastSeqHeader);
            size_t payload_len = static_cast<size_t>(n) - sizeof(exchange::McastSeqHeader);

            if (cfg.exchange == "cme") {
                auto rc = cme_ns::decode_mdp3_message(payload, payload_len,
                                                       cme_visitor);
                if (rc != cme_ns::DecodeResult::kOk &&
                    rc != cme_ns::DecodeResult::kUnknownTemplateId) {
                    ++ds.decode_errors;
                }
            } else {
                size_t consumed = ice_ns::decode_messages(payload, payload_len,
                                                          ice_visitor);
                if (consumed == 0 && payload_len > 0) {
                    ++ds.decode_errors;
                }
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (errno == EINTR) continue;
            ++ds.decode_errors;
        }

        // Periodic display refresh (~100ms).
        auto now = std::chrono::steady_clock::now();

        // Update msgs/sec counter every second.
        auto sec_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_second);
        if (sec_elapsed.count() >= 1) {
            ds.msgs_per_sec = ds.messages_this_second;
            ds.messages_this_second = 0;
            last_second = now;
        }

        auto display_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_display);
        if (display_elapsed.count() >= 100) {
            render_display(ds, cfg.instrument, cfg.exchange, is_price9);
            last_display = now;
        }

        journal.flush();
    }

    // Print summary on exit.
    journal.close();
    std::fprintf(stderr,
        "\n--- Observer Summary ---\n"
        "Instrument:    %s (%s)\n"
        "Total msgs:    %lu\n"
        "Total trades:  %lu\n"
        "Decode errors: %lu\n"
        "Seq gaps:      %lu\n"
        "Final OHLCV:   O=%ld H=%ld L=%ld C=%ld V=%ld\n",
        cfg.instrument.c_str(), cfg.exchange.c_str(),
        static_cast<unsigned long>(ds.total_messages),
        static_cast<unsigned long>(ds.total_trades),
        static_cast<unsigned long>(ds.decode_errors),
        static_cast<unsigned long>(ds.seq_gaps),
        static_cast<long>(ds.open_price), static_cast<long>(ds.high_price),
        static_cast<long>(ds.low_price), static_cast<long>(ds.close_price),
        static_cast<long>(ds.volume));

    return 0;
}
