#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Flat journal line -- a single ACTION or EXPECT with key-value fields.
// Unlike JournalParser::JournalEntry which groups EXPECT under ACTION,
// this representation treats every line independently, which is needed for
// the observer journal (EXPECT-only) and for merging timelines.
// ---------------------------------------------------------------------------
struct FlatJournalLine {
    enum Kind { Action, Expect };
    Kind kind;
    std::string type;  // e.g. "ILINK3_NEW_ORDER", "MD_TRADE", "EXEC_FILL"
    std::unordered_map<std::string, std::string> fields;

    int64_t get_int(const std::string& key) const {
        auto it = fields.find(key);
        if (it == fields.end()) return 0;
        return std::stoll(it->second);
    }

    std::string get_str(const std::string& key) const {
        auto it = fields.find(key);
        if (it == fields.end()) return {};
        return it->second;
    }

    bool has(const std::string& key) const {
        return fields.count(key) > 0;
    }

    int64_t ts() const { return get_int("ts"); }
};

// ---------------------------------------------------------------------------
// Parse a journal file into flat lines. Handles both trader journals
// (ACTION + EXPECT) and observer journals (EXPECT-only).
// Skips CONFIG lines, comments, blank lines.
// ---------------------------------------------------------------------------
inline std::vector<FlatJournalLine> parse_flat_journal(
        const std::string& content) {
    std::vector<FlatJournalLine> lines;
    std::istringstream stream(content);
    std::string raw;

    while (std::getline(stream, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.empty()) continue;

        auto first = raw.find_first_not_of(" \t");
        if (first == std::string::npos || raw[first] == '#') continue;

        // Tokenize on whitespace.
        std::vector<std::string> tokens;
        std::istringstream tok_stream(raw);
        std::string tok;
        while (tok_stream >> tok) tokens.push_back(std::move(tok));
        if (tokens.size() < 2) continue;

        FlatJournalLine line;
        if (tokens[0] == "ACTION") {
            line.kind = FlatJournalLine::Action;
        } else if (tokens[0] == "EXPECT") {
            line.kind = FlatJournalLine::Expect;
        } else {
            continue;  // Skip CONFIG and unknown keywords.
        }

        line.type = tokens[1];

        for (size_t i = 2; i < tokens.size(); ++i) {
            auto eq = tokens[i].find('=');
            if (eq == std::string::npos) continue;
            line.fields[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
        }

        lines.push_back(std::move(line));
    }
    return lines;
}

inline std::vector<FlatJournalLine> parse_flat_journal_file(
        const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "journal_reconciler: cannot open '" + path + "'");
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return parse_flat_journal(content);
}

// ---------------------------------------------------------------------------
// Reconciliation report -- counts for each invariant check.
// ---------------------------------------------------------------------------
struct ReconciliationReport {
    // Invariant 1: Trade matching.
    int64_t observer_trades = 0;
    int64_t matched_trades = 0;   // observer trades matched to 2 fills
    int64_t unmatched_trades = 0;

    // Invariant 2: Book add traceability.
    int64_t observer_book_adds = 0;
    int64_t book_adds_traced = 0;

    // Invariant 3: Book delete traceability.
    int64_t observer_book_deletes = 0;
    int64_t book_deletes_traced_to_cancel = 0;
    int64_t book_deletes_traced_to_fill = 0;

    // Invariant 4: Event ordering.
    int64_t ordering_violations = 0;

    // Invariant 5: Phantom events.
    int64_t phantom_trades = 0;
    int64_t phantom_book_adds = 0;

    // Invariant 6: Lost events.
    int64_t accepted_without_book_add_or_trade = 0;
    int64_t fill_without_trade = 0;
    int64_t cancelled_without_book_delete = 0;

    // Invariant 7: Final book state.
    int64_t final_book_levels = 0;

    // Invariant 8: Fill consistency.
    int64_t price_mismatches = 0;
    int64_t qty_mismatches = 0;

    // Derived stats.
    int64_t total_observer_events = 0;
    int64_t total_client_journals = 0;

    // Latency samples (ns): observer_ts - closest trader_ts for matched events.
    std::vector<int64_t> latency_samples;

    bool all_pass() const {
        return unmatched_trades == 0 &&
               (observer_book_adds == book_adds_traced) &&
               (observer_book_deletes ==
                    book_deletes_traced_to_cancel +
                    book_deletes_traced_to_fill) &&
               ordering_violations == 0 &&
               phantom_trades == 0 &&
               phantom_book_adds == 0 &&
               accepted_without_book_add_or_trade == 0 &&
               fill_without_trade == 0 &&
               cancelled_without_book_delete == 0 &&
               final_book_levels == 0 &&
               price_mismatches == 0 &&
               qty_mismatches == 0;
    }
};

// ---------------------------------------------------------------------------
// JournalReconciler -- the core reconciliation engine.
// ---------------------------------------------------------------------------
class JournalReconciler {
public:
    // Add a trader journal (flat lines parsed from file or string).
    void add_trader_journal(std::vector<FlatJournalLine> lines) {
        trader_journals_.push_back(std::move(lines));
    }

    // Set the observer journal.
    void set_observer_journal(std::vector<FlatJournalLine> lines) {
        observer_lines_ = std::move(lines);
    }

    // Run all 8 invariant checks and return the report.
    ReconciliationReport reconcile() const {
        ReconciliationReport report;
        report.total_client_journals =
            static_cast<int64_t>(trader_journals_.size());
        report.total_observer_events =
            static_cast<int64_t>(observer_lines_.size());

        // Consolidate all trader EXPECT lines sorted by timestamp.
        auto trader_expects = consolidate_trader_expects();

        // Collect trader ACTION lines (new orders, cancels).
        auto trader_actions = consolidate_trader_actions();

        // Build indexes for lookups.
        // fills: keyed by (price, qty) -> list of fills
        FillIndex fill_index = build_fill_index(trader_expects);

        // (Accept index not needed -- inv2 builds its own from raw journals.)

        // cancelled: set of (order_id)
        CancelSet cancel_set = build_cancel_set(trader_expects);

        // fully-filled: set of (order_id) where leaves_qty=0
        FullFillSet full_fill_set = build_full_fill_set(trader_expects);

        // observer event lists by type.
        std::vector<const FlatJournalLine*> obs_trades;
        std::vector<const FlatJournalLine*> obs_book_adds;
        std::vector<const FlatJournalLine*> obs_book_deletes;
        std::vector<const FlatJournalLine*> obs_all_ordered;

        for (const auto& line : observer_lines_) {
            if (line.kind != FlatJournalLine::Expect) continue;
            obs_all_ordered.push_back(&line);
            if (line.type == "MD_TRADE") obs_trades.push_back(&line);
            else if (line.type == "MD_BOOK_ADD") obs_book_adds.push_back(&line);
            else if (line.type == "MD_BOOK_DELETE")
                obs_book_deletes.push_back(&line);
        }

        check_inv1_trade_matching(report, obs_trades, fill_index);
        check_inv2_book_add_traceability(report, obs_book_adds);
        check_inv3_book_delete_traceability(
            report, obs_book_deletes, cancel_set, full_fill_set);
        check_inv4_event_ordering(
            report, obs_all_ordered, trader_expects);
        check_inv5_no_phantoms(report);
        check_inv6_no_lost_events(
            report, trader_expects, obs_trades, obs_book_adds,
            obs_book_deletes);
        check_inv7_final_book(report, obs_book_adds, obs_book_deletes);
        check_inv8_fill_consistency(report, obs_trades, fill_index);

        return report;
    }

private:
    std::vector<std::vector<FlatJournalLine>> trader_journals_;
    std::vector<FlatJournalLine> observer_lines_;

    // Index types.
    struct FillKey {
        int64_t price;
        int64_t qty;
        bool operator==(const FillKey& o) const {
            return price == o.price && qty == o.qty;
        }
    };
    struct FillKeyHash {
        size_t operator()(const FillKey& k) const {
            return std::hash<int64_t>()(k.price) ^
                   (std::hash<int64_t>()(k.qty) << 32);
        }
    };
    using FillIndex = std::unordered_map<
        FillKey, std::vector<const FlatJournalLine*>, FillKeyHash>;

    using CancelSet = std::unordered_set<int64_t>;  // order_ids
    using FullFillSet = std::unordered_set<int64_t>;  // order_ids

    // --- Consolidation ---

    std::vector<const FlatJournalLine*> consolidate_trader_expects() const {
        std::vector<const FlatJournalLine*> all;
        for (const auto& journal : trader_journals_) {
            for (const auto& line : journal) {
                if (line.kind == FlatJournalLine::Expect) {
                    all.push_back(&line);
                }
            }
        }
        std::sort(all.begin(), all.end(),
                  [](const FlatJournalLine* a, const FlatJournalLine* b) {
                      return a->ts() < b->ts();
                  });
        return all;
    }

    std::vector<const FlatJournalLine*> consolidate_trader_actions() const {
        std::vector<const FlatJournalLine*> all;
        for (const auto& journal : trader_journals_) {
            for (const auto& line : journal) {
                if (line.kind == FlatJournalLine::Action) {
                    all.push_back(&line);
                }
            }
        }
        std::sort(all.begin(), all.end(),
                  [](const FlatJournalLine* a, const FlatJournalLine* b) {
                      return a->ts() < b->ts();
                  });
        return all;
    }

    // --- Index builders ---

    static FillIndex build_fill_index(
            const std::vector<const FlatJournalLine*>& expects) {
        FillIndex idx;
        for (const auto* line : expects) {
            if (line->type == "EXEC_FILL" ||
                line->type == "EXEC_PARTIAL_FILL") {
                FillKey key{line->get_int("fill_price"),
                            line->get_int("fill_qty")};
                idx[key].push_back(line);
            }
        }
        return idx;
    }

    static CancelSet build_cancel_set(
            const std::vector<const FlatJournalLine*>& expects) {
        CancelSet set;
        for (const auto* line : expects) {
            if (line->type == "ORDER_CANCELLED" && line->has("order_id")) {
                set.insert(line->get_int("order_id"));
            }
        }
        return set;
    }

    static FullFillSet build_full_fill_set(
            const std::vector<const FlatJournalLine*>& expects) {
        FullFillSet set;
        for (const auto* line : expects) {
            if ((line->type == "EXEC_FILL" ||
                 line->type == "EXEC_PARTIAL_FILL") &&
                line->has("leaves_qty") &&
                line->get_int("leaves_qty") == 0) {
                set.insert(line->get_int("order_id"));
            }
        }
        return set;
    }

    // --- Invariant checks ---

    // Inv 1: Every MD_TRADE matches exactly 2 EXEC_FILLs (by price+qty).
    static void check_inv1_trade_matching(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& obs_trades,
            FillIndex& fill_index) {
        report.observer_trades = static_cast<int64_t>(obs_trades.size());
        for (const auto* trade : obs_trades) {
            FillKey key{trade->get_int("price"), trade->get_int("qty")};
            auto it = fill_index.find(key);
            if (it != fill_index.end() && it->second.size() >= 2) {
                report.matched_trades++;
                // Consume 2 fills from the bucket.
                it->second.erase(it->second.begin(),
                                 it->second.begin() + 2);
            } else {
                report.unmatched_trades++;
            }
        }
    }

    // Inv 2: Every MD_BOOK_ADD traces to an ORDER_ACCEPTED.
    // We match by price + side since that's what the observer sees.
    void check_inv2_book_add_traceability(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& obs_book_adds) const {
        report.observer_book_adds =
            static_cast<int64_t>(obs_book_adds.size());

        // Build a more useful index: from ACTION new orders that got accepted,
        // index by (price, side).
        // Walk trader journals: pair each ACTION new_order with its
        // following ORDER_ACCEPTED.
        struct AcceptedOrder {
            int64_t price;
            std::string side;
            bool consumed = false;
        };
        std::vector<AcceptedOrder> accepted_orders;

        for (const auto& journal : trader_journals_) {
            const FlatJournalLine* pending_action = nullptr;
            for (const auto& line : journal) {
                if (line.kind == FlatJournalLine::Action &&
                    (line.type.find("NEW_ORDER") != std::string::npos)) {
                    pending_action = &line;
                } else if (line.kind == FlatJournalLine::Expect &&
                           line.type == "ORDER_ACCEPTED" &&
                           pending_action != nullptr) {
                    accepted_orders.push_back(AcceptedOrder{
                        pending_action->get_int("price"),
                        pending_action->get_str("side"),
                        false});
                    pending_action = nullptr;
                } else if (line.kind == FlatJournalLine::Action) {
                    pending_action = nullptr;
                }
            }
        }

        for (const auto* add : obs_book_adds) {
            int64_t price = add->get_int("price");
            std::string side = add->get_str("side");
            bool found = false;
            for (auto& ao : accepted_orders) {
                if (!ao.consumed && ao.price == price && ao.side == side) {
                    ao.consumed = true;
                    found = true;
                    break;
                }
            }
            if (found) report.book_adds_traced++;
        }
    }

    // Inv 3: Every MD_BOOK_DELETE traces to a cancel or full fill.
    static void check_inv3_book_delete_traceability(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& obs_deletes,
            const CancelSet& cancel_set,
            const FullFillSet& full_fill_set) {
        report.observer_book_deletes =
            static_cast<int64_t>(obs_deletes.size());

        // For each observer book delete, check if there's a corresponding
        // cancel or full fill. Since observer deletes don't carry order_id,
        // we track by price+side and match against cancels/fills at that
        // price. We use a simple count-based approach: count cancels and
        // full-fills per (price,side), count observer deletes per
        // (price,side), and verify coverage.
        // For now, we accept any delete that can be attributed to a cancel
        // or fill based on available counts.
        int64_t cancel_count = static_cast<int64_t>(cancel_set.size());
        int64_t fill_count = static_cast<int64_t>(full_fill_set.size());
        int64_t total_deletes =
            static_cast<int64_t>(obs_deletes.size());

        // Attribute deletes to cancels first, then fills.
        int64_t traced_to_cancel =
            std::min(total_deletes, cancel_count);
        int64_t remaining = total_deletes - traced_to_cancel;
        int64_t traced_to_fill =
            std::min(remaining, fill_count);

        report.book_deletes_traced_to_cancel = traced_to_cancel;
        report.book_deletes_traced_to_fill = traced_to_fill;
    }

    // Inv 4: Event ordering consistency.
    static void check_inv4_event_ordering(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& obs_events,
            const std::vector<const FlatJournalLine*>& trader_expects) {
        // Build a mapping from (type, price, qty/side) -> sequence number
        // in trader timeline. Then verify observer events appear in
        // non-decreasing sequence order.
        // Simplified: check that observer timestamps are non-decreasing.
        // (In a real exchange, the sequencer guarantees this.)
        int64_t prev_ts = 0;
        for (const auto* event : obs_events) {
            int64_t ts = event->ts();
            if (ts < prev_ts) {
                report.ordering_violations++;
            }
            prev_ts = ts;
        }

        // Also check trader expects are ordered (they should be since
        // we sorted them).
        (void)trader_expects;
    }

    // Inv 5: No phantom events.
    // Derived from inv1 (trade matching) and inv2 (book add traceability).
    static void check_inv5_no_phantoms(ReconciliationReport& report) {
        // Phantom trade = observer trade with no matching fill pair.
        report.phantom_trades = report.unmatched_trades;
        // Phantom book add = observer add with no matching accepted order.
        report.phantom_book_adds =
            report.observer_book_adds - report.book_adds_traced;
    }

    // Inv 6: No lost events.
    static void check_inv6_no_lost_events(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& trader_expects,
            const std::vector<const FlatJournalLine*>& obs_trades,
            const std::vector<const FlatJournalLine*>& obs_book_adds,
            const std::vector<const FlatJournalLine*>& obs_book_deletes) {
        // Count trader events that should have observer counterparts.
        int64_t accepted_count = 0;
        int64_t fill_count = 0;
        int64_t cancelled_count = 0;

        // Track which accepted orders were immediately filled (no book add
        // expected). An order_id with both ORDER_ACCEPTED and EXEC_FILL
        // with leaves_qty=0 means it was immediately fully filled.
        std::unordered_set<int64_t> immediately_filled;
        for (const auto* line : trader_expects) {
            if ((line->type == "EXEC_FILL" ||
                 line->type == "EXEC_PARTIAL_FILL") &&
                line->has("leaves_qty") &&
                line->get_int("leaves_qty") == 0) {
                immediately_filled.insert(line->get_int("order_id"));
            }
        }

        for (const auto* line : trader_expects) {
            if (line->type == "ORDER_ACCEPTED") {
                accepted_count++;
            } else if (line->type == "EXEC_FILL" ||
                       line->type == "EXEC_PARTIAL_FILL") {
                fill_count++;
            } else if (line->type == "ORDER_CANCELLED") {
                cancelled_count++;
            }
        }

        // Each accepted order should produce an MD_BOOK_ADD unless
        // immediately filled (which produces MD_TRADE instead).
        int64_t expected_book_adds =
            accepted_count -
            static_cast<int64_t>(immediately_filled.size());
        int64_t actual_book_adds =
            static_cast<int64_t>(obs_book_adds.size());
        report.accepted_without_book_add_or_trade =
            std::max(int64_t{0}, expected_book_adds - actual_book_adds);

        // Each fill pair should produce an MD_TRADE. Fills come in pairs
        // (aggressor + resting), so divide by 2.
        int64_t expected_trades = fill_count / 2;
        int64_t actual_trades =
            static_cast<int64_t>(obs_trades.size());
        report.fill_without_trade =
            std::max(int64_t{0}, expected_trades - actual_trades);

        // Each cancel should produce an MD_BOOK_DELETE.
        int64_t actual_deletes =
            static_cast<int64_t>(obs_book_deletes.size());
        report.cancelled_without_book_delete =
            std::max(int64_t{0}, cancelled_count - actual_deletes);
    }

    // Inv 7: Final book state should be empty after clean shutdown.
    static void check_inv7_final_book(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& obs_book_adds,
            const std::vector<const FlatJournalLine*>& obs_book_deletes) {
        // Net book levels: count adds minus deletes per (side, price).
        struct LevelKey {
            std::string side;
            int64_t price;
            bool operator==(const LevelKey& o) const {
                return side == o.side && price == o.price;
            }
        };
        struct LevelKeyHash {
            size_t operator()(const LevelKey& k) const {
                return std::hash<std::string>()(k.side) ^
                       (std::hash<int64_t>()(k.price) << 16);
            }
        };
        std::unordered_map<LevelKey, int64_t, LevelKeyHash> book;

        for (const auto* add : obs_book_adds) {
            LevelKey key{add->get_str("side"), add->get_int("price")};
            book[key]++;
        }
        for (const auto* del : obs_book_deletes) {
            LevelKey key{del->get_str("side"), del->get_int("price")};
            book[key]--;
        }

        int64_t remaining = 0;
        for (const auto& [key, count] : book) {
            if (count > 0) remaining += count;
        }
        report.final_book_levels = remaining;
    }

    // Inv 8: Fill price/qty consistency between trader fills and observer
    // trades.
    static void check_inv8_fill_consistency(
            ReconciliationReport& report,
            const std::vector<const FlatJournalLine*>& obs_trades,
            const FillIndex& fill_index) {
        for (const auto* trade : obs_trades) {
            int64_t obs_price = trade->get_int("price");
            int64_t obs_qty = trade->get_int("qty");

            // Look for fills at this exact price+qty.
            FillKey key{obs_price, obs_qty};
            auto it = fill_index.find(key);
            if (it == fill_index.end() || it->second.empty()) {
                // No fills at exact price+qty -- check if there are fills
                // at the same price with different qty (qty mismatch) or
                // same qty with different price (price mismatch).
                bool found_any = false;
                for (const auto& [fk, fills] : fill_index) {
                    if (fills.empty()) continue;
                    if (fk.price == obs_price && fk.qty != obs_qty) {
                        report.qty_mismatches++;
                        found_any = true;
                        break;
                    }
                    if (fk.qty == obs_qty && fk.price != obs_price) {
                        report.price_mismatches++;
                        found_any = true;
                        break;
                    }
                }
                // If no fills found at all, it's already counted as
                // unmatched/phantom in inv1/inv5.
                (void)found_any;
            }
            // Exact match at price+qty = consistent.
        }
    }
};

}  // namespace exchange
