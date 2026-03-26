#include "ice/fix/fix_parser.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace ice::fix {

// --- FixMessage accessors ---

std::string FixMessage::get_string(int tag) const {
    auto it = fields.find(tag);
    return it != fields.end() ? it->second : std::string{};
}

int64_t FixMessage::get_int(int tag) const {
    auto it = fields.find(tag);
    if (it == fields.end()) return 0;
    int64_t val = 0;
    std::from_chars(it->second.data(),
                    it->second.data() + it->second.size(), val);
    return val;
}

double FixMessage::get_double(int tag) const {
    auto it = fields.find(tag);
    if (it == fields.end()) return 0.0;
    // strtod is locale-dependent (decimal separator). FIX 4.2 mandates '.' as
    // decimal point which matches the "C" locale. If the process locale changes,
    // callers must ensure LC_NUMERIC remains "C" on the FIX parsing thread.
    return std::strtod(it->second.c_str(), nullptr);
}

char FixMessage::get_char(int tag) const {
    auto it = fields.find(tag);
    if (it == fields.end() || it->second.empty()) return '\0';
    return it->second[0];
}

// --- ParseResult ---

ParseResult ParseResult::ok(FixMessage msg) {
    ParseResult r;
    r.ok_ = true;
    r.msg_ = std::move(msg);
    return r;
}

ParseResult ParseResult::err(std::string error) {
    ParseResult r;
    r.error_ = std::move(error);
    return r;
}

// --- Parser implementation ---

namespace {

constexpr char SOH = '\x01';

// Find next SOH starting at pos, return index or npos.
size_t find_soh(const char* data, size_t len, size_t pos) {
    auto* p = static_cast<const char*>(
        std::memchr(data + pos, SOH, len - pos));
    return p ? static_cast<size_t>(p - data) : std::string_view::npos;
}

// Parse one tag=value field at `pos`. Advances pos past the trailing SOH.
// Returns false if malformed.
bool parse_field(const char* data, size_t len, size_t& pos,
                 int& out_tag, std::string_view& out_value) {
    if (pos >= len) return false;

    // Find '='
    size_t eq = pos;
    while (eq < len && data[eq] != '=') ++eq;
    if (eq >= len) return false;

    // Parse tag number
    int tag_num = 0;
    auto [ptr, ec] =
        std::from_chars(data + pos, data + eq, tag_num);
    if (ec != std::errc{} || ptr != data + eq) return false;

    // Find SOH
    size_t soh = find_soh(data, len, eq + 1);
    if (soh == std::string_view::npos) return false;

    out_tag = tag_num;
    out_value = std::string_view(data + eq + 1, soh - eq - 1);
    pos = soh + 1;
    return true;
}

// Compute FIX checksum: sum of all bytes mod 256.
uint32_t compute_checksum(const char* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += static_cast<uint8_t>(data[i]);
    }
    return sum % 256;
}

}  // namespace

ParseResult parse_fix_message(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return ParseResult::err("empty message");
    }

    size_t pos = 0;
    int tag_num = 0;
    std::string_view value;

    // --- Tag 8: BeginString ---
    if (!parse_field(data, len, pos, tag_num, value) || tag_num != 8) {
        return ParseResult::err("missing BeginString (tag 8)");
    }
    if (value != "FIX.4.2") {
        return ParseResult::err(
            "invalid BeginString: expected FIX.4.2");
    }

    // --- Tag 9: BodyLength ---
    if (!parse_field(data, len, pos, tag_num, value) || tag_num != 9) {
        return ParseResult::err("missing BodyLength (tag 9)");
    }
    int64_t body_length = 0;
    {
        auto [p, ec] = std::from_chars(value.data(),
                                        value.data() + value.size(),
                                        body_length);
        if (ec != std::errc{}) {
            return ParseResult::err("invalid BodyLength value");
        }
    }
    if (body_length < 0) {
        return ParseResult::err("negative BodyLength");
    }

    // pos now points to start of body; body ends at 10= tag
    size_t body_start = pos;
    size_t body_end = body_start + static_cast<size_t>(body_length);

    if (body_end > len) {
        return ParseResult::err(
            "BodyLength exceeds message size");
    }

    // --- Parse body fields (between BodyLength and CheckSum) ---
    FixMessage msg;
    size_t body_pos = body_start;
    while (body_pos < body_end) {
        int t = 0;
        std::string_view v;
        if (!parse_field(data, body_end, body_pos, t, v)) {
            return ParseResult::err("malformed field in body");
        }
        if (t == 35) {
            msg.msg_type = std::string(v);
        }
        msg.fields.emplace(t, std::string(v));
    }

    // Verify body_pos landed exactly at body_end
    if (body_pos != body_end) {
        return ParseResult::err(
            "BodyLength mismatch: fields don't align");
    }

    // --- Tag 10: CheckSum ---
    pos = body_end;
    if (!parse_field(data, len, pos, tag_num, value) || tag_num != 10) {
        return ParseResult::err("missing CheckSum (tag 10)");
    }

    // Validate checksum: sum of all bytes before "10=" tag
    uint32_t expected = compute_checksum(data, body_end);
    char expected_str[4];
    std::snprintf(expected_str, sizeof(expected_str), "%03u", expected);

    if (value != std::string_view(expected_str, 3)) {
        return ParseResult::err(
            "checksum mismatch: expected " +
            std::string(expected_str, 3) + ", got " +
            std::string(value));
    }

    // --- Validate MsgType present ---
    if (msg.msg_type.empty()) {
        return ParseResult::err("missing MsgType (tag 35)");
    }

    return ParseResult::ok(std::move(msg));
}

}  // namespace ice::fix
