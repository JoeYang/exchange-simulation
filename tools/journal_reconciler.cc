// journal-reconciler: Post-trade reconciliation of trader journals vs
// observer journal. Runs 8 invariant checks and prints a formatted report.
//
// Usage:
//   journal-reconciler
//       --observer-journal /tmp/observer.journal
//       --client-journals /tmp/c1.journal,/tmp/c2.journal,...

#include "tools/journal_reconciler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --observer-journal PATH "
        "--client-journals PATH[,PATH...]\n", prog);
}

// Split a comma-separated string into a vector of strings.
std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> result;
    std::string::size_type start = 0;
    while (start < s.size()) {
        auto pos = s.find(',', start);
        if (pos == std::string::npos) {
            result.push_back(s.substr(start));
            break;
        }
        result.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return result;
}

// Format a percentage string. Returns "N/A" if denominator is zero.
std::string pct(int64_t num, int64_t denom) {
    if (denom == 0) return "N/A";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%",
                  100.0 * static_cast<double>(num) /
                  static_cast<double>(denom));
    return buf;
}

void print_report(const exchange::ReconciliationReport& r) {
    std::printf("Reconciliation Report\n");
    std::printf("=====================\n");
    std::printf("Clients: %ld\n", static_cast<long>(r.total_client_journals));
    std::printf("Observer events: %ld\n",
                static_cast<long>(r.total_observer_events));
    std::printf("\n");

    // Invariant 1: Trade Matching.
    std::printf("Trade Matching (Invariant 1):\n");
    std::printf("  Observer trades:   %6ld\n",
                static_cast<long>(r.observer_trades));
    std::printf("  Matched (2 fills): %6ld  (%s)\n",
                static_cast<long>(r.matched_trades),
                pct(r.matched_trades, r.observer_trades).c_str());
    std::printf("  Unmatched:         %6ld  (%s)\n",
                static_cast<long>(r.unmatched_trades),
                pct(r.unmatched_trades, r.observer_trades).c_str());
    std::printf("\n");

    // Invariants 2-3: Book Traceability.
    std::printf("Book Traceability (Invariants 2-3):\n");
    std::printf("  MD_BOOK_ADD:         %6ld\n",
                static_cast<long>(r.observer_book_adds));
    std::printf("  Traced to NEW_ORDER: %6ld  (%s)\n",
                static_cast<long>(r.book_adds_traced),
                pct(r.book_adds_traced, r.observer_book_adds).c_str());
    std::printf("  MD_BOOK_DELETE:      %6ld\n",
                static_cast<long>(r.observer_book_deletes));
    std::printf("  Traced to CANCEL:    %6ld  (%s)\n",
                static_cast<long>(r.book_deletes_traced_to_cancel),
                pct(r.book_deletes_traced_to_cancel,
                    r.observer_book_deletes).c_str());
    std::printf("  Traced to FILL:      %6ld  (%s)\n",
                static_cast<long>(r.book_deletes_traced_to_fill),
                pct(r.book_deletes_traced_to_fill,
                    r.observer_book_deletes).c_str());
    std::printf("\n");

    // Invariant 4: Event Ordering.
    std::printf("Event Ordering (Invariant 4):\n");
    std::printf("  Order violations:    %6ld\n",
                static_cast<long>(r.ordering_violations));
    std::printf("\n");

    // Invariant 5: Phantom Events.
    std::printf("Phantom Events (Invariant 5):\n");
    std::printf("  Phantom trades:      %6ld\n",
                static_cast<long>(r.phantom_trades));
    std::printf("  Phantom book adds:   %6ld\n",
                static_cast<long>(r.phantom_book_adds));
    std::printf("\n");

    // Invariant 6: Lost Events.
    std::printf("Lost Events (Invariant 6):\n");
    std::printf("  ORDER_ACCEPTED w/o MD_BOOK_ADD or MD_TRADE: %ld\n",
                static_cast<long>(r.accepted_without_book_add_or_trade));
    std::printf("  EXEC_FILL w/o MD_TRADE:                     %ld\n",
                static_cast<long>(r.fill_without_trade));
    std::printf("  ORDER_CANCELLED w/o MD_BOOK_DELETE:          %ld\n",
                static_cast<long>(r.cancelled_without_book_delete));
    std::printf("\n");

    // Invariant 7: Final Book State.
    std::printf("Final Book State (Invariant 7):\n");
    std::printf("  Observer book levels remaining: %ld",
                static_cast<long>(r.final_book_levels));
    if (r.final_book_levels == 0) {
        std::printf("  (clean shutdown)");
    }
    std::printf("\n\n");

    // Invariant 8: Fill Consistency.
    std::printf("Fill Consistency (Invariant 8):\n");
    std::printf("  Price mismatches:  %6ld\n",
                static_cast<long>(r.price_mismatches));
    std::printf("  Qty mismatches:    %6ld\n",
                static_cast<long>(r.qty_mismatches));
    std::printf("\n");

    // Latency Distribution.
    if (!r.latency_samples.empty()) {
        auto sorted = r.latency_samples;
        std::sort(sorted.begin(), sorted.end());
        auto percentile = [&](double p) -> int64_t {
            size_t idx = static_cast<size_t>(
                p * static_cast<double>(sorted.size() - 1));
            return sorted[idx];
        };
        std::printf(
            "Latency Distribution (client action -> observer sees effect):\n");
        std::printf("  p50:  %6ld us\n", percentile(0.50) / 1000);
        std::printf("  p95:  %6ld us\n", percentile(0.95) / 1000);
        std::printf("  p99:  %6ld us\n", percentile(0.99) / 1000);
        std::printf("  max:  %6ld us\n",
                    static_cast<long>(sorted.back()) / 1000);
        std::printf("\n");
    }

    // Summary.
    if (r.all_pass()) {
        std::printf("RESULT: ALL INVARIANTS PASS\n");
    } else {
        std::printf("RESULT: INVARIANT VIOLATIONS DETECTED\n");
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string observer_path;
    std::string client_paths_csv;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--observer-journal" && i + 1 < argc) {
            observer_path = argv[++i];
        } else if (arg == "--client-journals" && i + 1 < argc) {
            client_paths_csv = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (observer_path.empty() || client_paths_csv.empty()) {
        usage(argv[0]);
        return 2;
    }

    auto client_paths = split_csv(client_paths_csv);
    if (client_paths.empty()) {
        std::fprintf(stderr, "Error: no client journals specified\n");
        return 2;
    }

    exchange::JournalReconciler reconciler;

    // Parse observer journal.
    try {
        reconciler.set_observer_journal(
            exchange::parse_flat_journal_file(observer_path));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error reading observer journal: %s\n",
                     e.what());
        return 1;
    }

    // Parse each client journal.
    for (const auto& path : client_paths) {
        try {
            reconciler.add_trader_journal(
                exchange::parse_flat_journal_file(path));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error reading client journal '%s': %s\n",
                         path.c_str(), e.what());
            return 1;
        }
    }

    // Run reconciliation and print report.
    auto report = reconciler.reconcile();
    print_report(report);

    return report.all_pass() ? 0 : 1;
}
