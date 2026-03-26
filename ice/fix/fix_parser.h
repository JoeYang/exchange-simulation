#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ice::fix {

// Parsed FIX message: msg_type + tag-value map with typed accessors.
struct FixMessage {
    std::string msg_type;
    std::unordered_map<int, std::string> fields;

    // Typed accessors — return zero/empty if tag is absent.
    [[nodiscard]] std::string get_string(int tag) const;
    [[nodiscard]] int64_t get_int(int tag) const;
    [[nodiscard]] double get_double(int tag) const;
    [[nodiscard]] char get_char(int tag) const;
};

// Lightweight result type: holds either a FixMessage or an error string.
// Avoids pulling in <expected> (C++23) for broader compiler compat.
class ParseResult {
   public:
    static ParseResult ok(FixMessage msg);
    static ParseResult err(std::string error);

    [[nodiscard]] bool has_value() const { return ok_; }
    [[nodiscard]] const FixMessage& value() const { return msg_; }
    [[nodiscard]] const std::string& error() const { return error_; }

   private:
    bool ok_{false};
    FixMessage msg_;
    std::string error_;
};

// Parse a raw FIX 4.2 message (SOH-delimited tag=value pairs).
// Validates BeginString, BodyLength, and CheckSum.
ParseResult parse_fix_message(const char* data, size_t len);

}  // namespace ice::fix
