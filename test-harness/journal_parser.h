#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Parsed configuration from CONFIG lines in a .journal file.
// All fields have defaults matching the spec (Section 8.3).
// ---------------------------------------------------------------------------
struct ParsedConfig {
    std::string match_algo   = "FIFO";
    int64_t     tick_size        = 100;
    int64_t     lot_size         = 10000;
    int64_t     max_orders       = 1000;
    int64_t     max_levels       = 100;
    int64_t     max_order_ids    = 10000;
    int64_t     price_band_low   = 0;
    int64_t     price_band_high  = 0;
};

// ---------------------------------------------------------------------------
// Parsed ACTION line.  All fields are stored as strings; typed accessors
// perform on-demand conversion so errors surface at assertion time.
// ---------------------------------------------------------------------------
struct ParsedAction {
    enum Type { NewOrder, Cancel, Modify, TriggerExpiry,
                SetSessionState, ExecuteAuction, PublishIndicative,
                MassCancel, MassCancelAll,
                // iLink3 E2E action types (client-side order entry)
                ILink3NewOrder, ILink3Cancel, ILink3Replace, ILink3MassCancel,
                // E2E session lifecycle actions
                SessionStart, SessionOpen, SessionClose };

    Type type;
    std::unordered_map<std::string, std::string> fields;

    // Returns the field value converted to int64_t.
    // Throws std::out_of_range  if key is missing.
    // Throws std::invalid_argument if value is not a valid integer.
    int64_t     get_int(const std::string& key) const;

    // Returns the raw string value for key.
    // Throws std::out_of_range if key is missing.
    std::string get_str(const std::string& key) const;
};

// ---------------------------------------------------------------------------
// Parsed EXPECT line.
// ---------------------------------------------------------------------------
struct ParsedExpectation {
    std::string event_type;  // e.g. "ORDER_ACCEPTED", "TRADE", "DEPTH_UPDATE"
    std::unordered_map<std::string, std::string> fields;

    // Same semantics as ParsedAction helpers.
    int64_t     get_int(const std::string& key) const;
    std::string get_str(const std::string& key) const;
};

// ---------------------------------------------------------------------------
// One ACTION together with all immediately-following EXPECT lines.
// ---------------------------------------------------------------------------
struct JournalEntry {
    ParsedAction                  action;
    std::vector<ParsedExpectation> expectations;
};

// ---------------------------------------------------------------------------
// Top-level parsed journal.
// ---------------------------------------------------------------------------
struct Journal {
    ParsedConfig              config;
    std::vector<JournalEntry> entries;
};

// ---------------------------------------------------------------------------
// Parser.
// ---------------------------------------------------------------------------
class JournalParser {
public:
    // Parse a .journal file from disk.
    // Throws std::runtime_error on I/O errors or malformed content.
    static Journal parse(const std::string& file_path);

    // Parse journal content from an in-memory string (primarily for testing).
    // Throws std::runtime_error on malformed content.
    static Journal parse_string(const std::string& content);

private:
    // Shared implementation used by both public entry points.
    static Journal parse_lines(const std::vector<std::string>& lines,
                               const std::string& source_name);

    // Parse "key=value key=value ..." tokens starting at offset into fields.
    // Throws std::runtime_error if any token lacks a '=' separator.
    static void parse_kv_tokens(const std::vector<std::string>& tokens,
                                std::size_t offset,
                                std::unordered_map<std::string, std::string>& out,
                                const std::string& source_name,
                                int line_number);

    // Apply a parsed CONFIG token map to the config struct.
    static void apply_config(const std::unordered_map<std::string, std::string>& kv,
                             ParsedConfig& cfg);

    // Tokenise a single line on whitespace.
    static std::vector<std::string> split(const std::string& line);
};

}  // namespace exchange
