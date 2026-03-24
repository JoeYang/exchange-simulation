#pragma once

#include "test-harness/journal_parser.h"
#include "test-harness/recorded_event.h"

#include <string>
#include <vector>

namespace exchange {

// Serializes ParsedConfig, ParsedAction, and RecordedEvent back to the
// .journal text format understood by JournalParser.  The primary use case
// is "record mode": run the engine, capture output via a recording listener,
// then call write() to persist the result as a regression baseline.
class JournalWriter {
public:
    // Write a complete journal to a file.
    // Throws std::runtime_error on I/O errors.
    static void write(const std::string& path,
                      const ParsedConfig& config,
                      const std::vector<JournalEntry>& entries);

    // Serialize to string (for testing and in-memory use).
    static std::string to_string(const ParsedConfig& config,
                                 const std::vector<JournalEntry>& entries);

    // Convert a RecordedEvent to an EXPECT line (no trailing newline).
    // Format: EXPECT <EVENT_TYPE> key=value ...
    static std::string event_to_expect_line(const RecordedEvent& event);

    // Convert a ParsedAction to an ACTION line (no trailing newline).
    // Format: ACTION <TYPE> key=value ...
    static std::string action_to_action_line(const ParsedAction& action);

    // Convert a ParsedConfig to a CONFIG line (no trailing newline).
    // Format: CONFIG match_algo=... tick_size=... lot_size=... ...
    static std::string config_to_config_lines(const ParsedConfig& config);
};

}  // namespace exchange
