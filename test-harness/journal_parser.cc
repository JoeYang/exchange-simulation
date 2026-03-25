#include "test-harness/journal_parser.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// ParsedAction helpers
// ---------------------------------------------------------------------------

int64_t ParsedAction::get_int(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) {
        throw std::out_of_range("ParsedAction: missing field '" + key + "'");
    }
    // std::stoll throws std::invalid_argument / std::out_of_range on bad input.
    return std::stoll(it->second);
}

std::string ParsedAction::get_str(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) {
        throw std::out_of_range("ParsedAction: missing field '" + key + "'");
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// ParsedExpectation helpers
// ---------------------------------------------------------------------------

int64_t ParsedExpectation::get_int(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) {
        throw std::out_of_range("ParsedExpectation: missing field '" + key + "'");
    }
    return std::stoll(it->second);
}

std::string ParsedExpectation::get_str(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) {
        throw std::out_of_range("ParsedExpectation: missing field '" + key + "'");
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// JournalParser -- public entry points
// ---------------------------------------------------------------------------

Journal JournalParser::parse(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("JournalParser: cannot open file '" + file_path + "'");
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(std::move(line));
    }

    return parse_lines(lines, file_path);
}

Journal JournalParser::parse_string(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return parse_lines(lines, "<string>");
}

// ---------------------------------------------------------------------------
// JournalParser -- private implementation
// ---------------------------------------------------------------------------

Journal JournalParser::parse_lines(const std::vector<std::string>& lines,
                                   const std::string& source_name) {
    Journal journal;
    JournalEntry* current_entry = nullptr;

    int line_number = 0;
    for (const std::string& raw : lines) {
        ++line_number;

        // Strip trailing carriage return (Windows line endings).
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Skip blank lines.
        bool all_space = true;
        for (char c : line) {
            if (c != ' ' && c != '\t') { all_space = false; break; }
        }
        if (line.empty() || all_space) continue;

        // Skip comment lines.
        // Find first non-whitespace character.
        std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line[first] == '#') continue;

        std::vector<std::string> tokens = split(line);
        if (tokens.empty()) continue;

        const std::string& keyword = tokens[0];

        if (keyword == "CONFIG") {
            std::unordered_map<std::string, std::string> kv;
            parse_kv_tokens(tokens, 1, kv, source_name, line_number);
            apply_config(kv, journal.config);

        } else if (keyword == "ACTION") {
            if (tokens.size() < 2) {
                throw std::runtime_error(
                    source_name + ":" + std::to_string(line_number) +
                    ": ACTION line missing type token");
            }

            const std::string& type_str = tokens[1];
            ParsedAction::Type atype;
            if (type_str == "NEW_ORDER") {
                atype = ParsedAction::NewOrder;
            } else if (type_str == "CANCEL") {
                atype = ParsedAction::Cancel;
            } else if (type_str == "MODIFY") {
                atype = ParsedAction::Modify;
            } else if (type_str == "TRIGGER_EXPIRY") {
                atype = ParsedAction::TriggerExpiry;
            } else if (type_str == "SET_SESSION_STATE") {
                atype = ParsedAction::SetSessionState;
            } else if (type_str == "EXECUTE_AUCTION") {
                atype = ParsedAction::ExecuteAuction;
            } else if (type_str == "PUBLISH_INDICATIVE") {
                atype = ParsedAction::PublishIndicative;
            } else if (type_str == "MASS_CANCEL") {
                atype = ParsedAction::MassCancel;
            } else if (type_str == "MASS_CANCEL_ALL") {
                atype = ParsedAction::MassCancelAll;
            } else if (type_str == "ILINK3_NEW_ORDER") {
                atype = ParsedAction::ILink3NewOrder;
            } else if (type_str == "ILINK3_CANCEL") {
                atype = ParsedAction::ILink3Cancel;
            } else if (type_str == "ILINK3_REPLACE") {
                atype = ParsedAction::ILink3Replace;
            } else if (type_str == "ILINK3_MASS_CANCEL") {
                atype = ParsedAction::ILink3MassCancel;
            } else if (type_str == "SESSION_START") {
                atype = ParsedAction::SessionStart;
            } else if (type_str == "SESSION_OPEN") {
                atype = ParsedAction::SessionOpen;
            } else if (type_str == "SESSION_CLOSE") {
                atype = ParsedAction::SessionClose;
            } else {
                throw std::runtime_error(
                    source_name + ":" + std::to_string(line_number) +
                    ": unknown ACTION type '" + type_str + "'");
            }

            ParsedAction action;
            action.type = atype;
            parse_kv_tokens(tokens, 2, action.fields, source_name, line_number);

            journal.entries.push_back(JournalEntry{std::move(action), {}});
            current_entry = &journal.entries.back();

        } else if (keyword == "EXPECT") {
            if (current_entry == nullptr) {
                throw std::runtime_error(
                    source_name + ":" + std::to_string(line_number) +
                    ": EXPECT line before any ACTION");
            }
            if (tokens.size() < 2) {
                throw std::runtime_error(
                    source_name + ":" + std::to_string(line_number) +
                    ": EXPECT line missing event type token");
            }

            ParsedExpectation expectation;
            expectation.event_type = tokens[1];
            parse_kv_tokens(tokens, 2, expectation.fields, source_name, line_number);
            current_entry->expectations.push_back(std::move(expectation));

        } else {
            throw std::runtime_error(
                source_name + ":" + std::to_string(line_number) +
                ": unknown keyword '" + keyword + "'");
        }
    }

    return journal;
}

void JournalParser::parse_kv_tokens(
        const std::vector<std::string>& tokens,
        std::size_t offset,
        std::unordered_map<std::string, std::string>& out,
        const std::string& source_name,
        int line_number) {
    for (std::size_t i = offset; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];
        std::size_t eq = tok.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error(
                source_name + ":" + std::to_string(line_number) +
                ": malformed key=value token '" + tok + "'");
        }
        std::string key   = tok.substr(0, eq);
        std::string value = tok.substr(eq + 1);
        out[std::move(key)] = std::move(value);
    }
}

void JournalParser::apply_config(
        const std::unordered_map<std::string, std::string>& kv,
        ParsedConfig& cfg) {
    auto it = kv.find("match_algo");
    if (it != kv.end()) cfg.match_algo = it->second;

    auto apply_int = [&](const std::string& key, int64_t& field) {
        auto jt = kv.find(key);
        if (jt != kv.end()) field = std::stoll(jt->second);
    };

    apply_int("tick_size",       cfg.tick_size);
    apply_int("lot_size",        cfg.lot_size);
    apply_int("max_orders",      cfg.max_orders);
    apply_int("max_levels",      cfg.max_levels);
    apply_int("max_order_ids",   cfg.max_order_ids);
    apply_int("price_band_low",  cfg.price_band_low);
    apply_int("price_band_high", cfg.price_band_high);
}

std::vector<std::string> JournalParser::split(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

}  // namespace exchange
