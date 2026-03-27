#include "test-harness/test_runner.h"
#include "test-harness/journal_parser.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

namespace exchange {
namespace {

class JournalTest : public ::testing::TestWithParam<std::string> {};

TEST_P(JournalTest, RunJournal) {
    auto journal = JournalParser::parse(GetParam());
    JournalTestRunner runner;
    TestResult result;
    if (journal.config.match_algo == "PRO_RATA") {
        result = runner.run_pro_rata(journal);
    } else if (journal.config.match_algo == "FIFO_SMP") {
        result = runner.run_smp_fifo(journal);
    } else if (journal.config.rate_limit > 0) {
        result = runner.run_rate_throttled(journal);
    } else {
        result = runner.run_fifo(journal);
    }
    EXPECT_TRUE(result.passed) << "Action " << result.action_index
        << ", Event " << result.event_index
        << "\nExpected: " << result.expected
        << "\nActual:   " << result.actual
        << "\nDiff:     " << result.diff;
}

// Discover all .journal files from the Bazel runfiles path.
std::vector<std::string> discover_journals() {
    std::vector<std::string> files;
    std::filesystem::path dir("test-journals");
    if (!std::filesystem::exists(dir)) {
        return files;
    }
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".journal") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

INSTANTIATE_TEST_SUITE_P(AllJournals, JournalTest,
    ::testing::ValuesIn(discover_journals()),
    [](const auto& info) {
        auto path = std::filesystem::path(info.param);
        return path.stem().string();
    });

}  // namespace
}  // namespace exchange
