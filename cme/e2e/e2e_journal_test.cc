// e2e_journal_test.cc
//
// Discovers and runs all E2E journal files from test-journals/e2e/*.journal
// and test-journals/cme/*.journal through the full CME E2E pipeline.
//
// Uses GoogleTest parameterized tests: one test case per journal file.
// Reports pass/fail per expectation line with detailed diagnostics.
//
// When no journal files exist yet (before generation), the test passes
// with zero parameterized instances.

#include "cme/e2e/e2e_test_runner.h"
#include "test-harness/journal_parser.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace exchange;
using namespace exchange::cme;

namespace {

std::vector<std::string> discover_journals(const std::string& dir) {
    std::vector<std::string> paths;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return paths;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".journal") {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<std::string> all_e2e_journals() {
    std::vector<std::string> all;
    auto add_from = [&](const std::string& dir) {
        auto journals = discover_journals(dir);
        all.insert(all.end(), journals.begin(), journals.end());
    };
    add_from("test-journals/e2e");
    add_from("test-journals/cme");
    return all;
}

std::string journal_test_name(const std::string& path) {
    fs::path p(path);
    return p.stem().string();
}

class E2EJournalTest : public ::testing::TestWithParam<std::string> {};

TEST_P(E2EJournalTest, RunJournal) {
    const std::string& path = GetParam();
    SCOPED_TRACE("Journal: " + path);

    Journal journal = JournalParser::parse(path);
    E2ETestRunner runner;
    auto results = runner.run(journal);

    size_t pass_count = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].passed)
            << "Expectation " << i
            << " at action " << results[i].action_index
            << " [" << results[i].category << "]:"
            << "\n  expected: " << results[i].expected
            << "\n  actual:   " << results[i].actual;
        if (results[i].passed) ++pass_count;
    }

    if (!results.empty()) {
        EXPECT_EQ(pass_count, results.size())
            << pass_count << "/" << results.size() << " expectations passed";
    }
}

// Allow the suite to have zero instances when no journal files exist yet.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(E2EJournalTest);

// ValuesIn with an empty vector produces zero test instances — valid with
// the GTEST_ALLOW_UNINSTANTIATED macro above.
const auto& journals_for_test() {
    static auto journals = all_e2e_journals();
    return journals;
}

INSTANTIATE_TEST_SUITE_P(
    E2EJournals,
    E2EJournalTest,
    ::testing::ValuesIn(journals_for_test()),
    [](const ::testing::TestParamInfo<std::string>& info) {
        return journal_test_name(info.param);
    }
);

}  // anonymous namespace
