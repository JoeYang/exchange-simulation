#include "krx/fast/fast_types.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace exchange::krx::fast {
namespace {

// ---------------------------------------------------------------------------
// Stop-bit unsigned integer encoding/decoding
// ---------------------------------------------------------------------------

TEST(FastStopBitUnsigned, ZeroEncodes) {
    uint8_t buf[16]{};
    auto* end = encode_u64(buf, sizeof(buf), 0);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], 0x80);  // stop bit set, value 0
}

TEST(FastStopBitUnsigned, SmallValueOneByte) {
    uint8_t buf[16]{};
    // 127 = 0x7F, fits in one byte -> 0xFF (0x7F | 0x80)
    auto* end = encode_u64(buf, sizeof(buf), 127);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], 0xFF);
}

TEST(FastStopBitUnsigned, ValueNeedsTwoBytes) {
    uint8_t buf[16]{};
    // 128 = 0b10000000 -> two 7-bit groups: [0x01, 0x00]
    // Wire: 0x01, 0x80
    auto* end = encode_u64(buf, sizeof(buf), 128);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 2);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x80);
}

TEST(FastStopBitUnsigned, Value942) {
    // 942 = 0b1110101110 -> 7-bit groups: [0x07, 0x2E]
    // Wire: 0x07, 0xAE (0x2E | 0x80)
    uint8_t buf[16]{};
    auto* end = encode_u64(buf, sizeof(buf), 942);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 2);
    EXPECT_EQ(buf[0], 0x07);
    EXPECT_EQ(buf[1], 0xAE);
}

TEST(FastStopBitUnsigned, LargeValue) {
    uint8_t buf[16]{};
    uint64_t val = 1000000;
    auto* end = encode_u64(buf, sizeof(buf), val);
    ASSERT_NE(end, nullptr);
    // Decode back
    uint64_t decoded = 0;
    auto* dend = decode_u64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    EXPECT_EQ(decoded, val);
}

TEST(FastStopBitUnsigned, MaxU64Roundtrip) {
    uint8_t buf[16]{};
    uint64_t val = std::numeric_limits<uint64_t>::max();
    auto* end = encode_u64(buf, sizeof(buf), val);
    ASSERT_NE(end, nullptr);

    uint64_t decoded = 0;
    auto* dend = decode_u64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    EXPECT_EQ(decoded, val);
}

TEST(FastStopBitUnsigned, RoundtripVariousValues) {
    const uint64_t values[] = {0, 1, 63, 64, 127, 128, 255, 256,
                                16383, 16384, 1000000, 0xFFFFFFFF,
                                0xFFFFFFFFFFFFFFFF};
    for (uint64_t val : values) {
        uint8_t buf[16]{};
        auto* end = encode_u64(buf, sizeof(buf), val);
        ASSERT_NE(end, nullptr) << "encode failed for " << val;

        uint64_t decoded = 0;
        auto* dend = decode_u64(buf, end - buf, decoded);
        ASSERT_NE(dend, nullptr) << "decode failed for " << val;
        EXPECT_EQ(decoded, val) << "roundtrip mismatch for " << val;
    }
}

TEST(FastStopBitUnsigned, BufferTooSmall) {
    uint8_t buf[1]{};
    // 128 needs 2 bytes
    auto* end = encode_u64(buf, 1, 128);
    EXPECT_EQ(end, nullptr);
}

TEST(FastStopBitUnsigned, DecodeTruncated) {
    // A byte without stop bit and then EOF
    uint8_t buf[] = {0x01};
    uint64_t val = 0;
    auto* end = decode_u64(buf, 1, val);
    EXPECT_EQ(end, nullptr);
}

// ---------------------------------------------------------------------------
// Stop-bit signed integer encoding/decoding
// ---------------------------------------------------------------------------

TEST(FastStopBitSigned, PositiveZero) {
    uint8_t buf[16]{};
    auto* end = encode_i64(buf, sizeof(buf), 0);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], 0x80);  // stop bit, value 0

    int64_t decoded = -1;
    decode_i64(buf, end - buf, decoded);
    EXPECT_EQ(decoded, 0);
}

TEST(FastStopBitSigned, PositiveSmall) {
    uint8_t buf[16]{};
    // 63 = 0x3F -> fits in one byte without sign confusion (bit 6 = 0)
    auto* end = encode_i64(buf, sizeof(buf), 63);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(buf[0], 0xBF);  // 0x3F | 0x80

    int64_t decoded = 0;
    decode_i64(buf, end - buf, decoded);
    EXPECT_EQ(decoded, 63);
}

TEST(FastStopBitSigned, PositiveNeedsExtraByte) {
    uint8_t buf[16]{};
    // 64 = 0x40 -> bit 6 is set, looks negative. Need extra zero byte.
    auto* end = encode_i64(buf, sizeof(buf), 64);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 2);
    EXPECT_EQ(buf[0], 0x00);  // leading zero, sign bit clear
    EXPECT_EQ(buf[1], 0xC0);  // 0x40 | 0x80

    int64_t decoded = 0;
    decode_i64(buf, end - buf, decoded);
    EXPECT_EQ(decoded, 64);
}

TEST(FastStopBitSigned, NegativeOne) {
    uint8_t buf[16]{};
    auto* end = encode_i64(buf, sizeof(buf), -1);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], 0xFF);  // 0x7F | 0x80 = all 1s

    int64_t decoded = 0;
    decode_i64(buf, end - buf, decoded);
    EXPECT_EQ(decoded, -1);
}

TEST(FastStopBitSigned, NegativeSmall) {
    uint8_t buf[16]{};
    // -64 = 0x40 in sign-magnitude, fits in one 7-bit group with sign
    auto* end = encode_i64(buf, sizeof(buf), -64);
    ASSERT_NE(end, nullptr);

    int64_t decoded = 0;
    decode_i64(buf, end - buf, decoded);
    EXPECT_EQ(decoded, -64);
}

TEST(FastStopBitSigned, RoundtripVariousValues) {
    const int64_t values[] = {0, 1, -1, 63, 64, -63, -64, -65,
                               127, 128, -127, -128,
                               1000000, -1000000,
                               std::numeric_limits<int64_t>::max(),
                               std::numeric_limits<int64_t>::min()};
    for (int64_t val : values) {
        uint8_t buf[16]{};
        auto* end = encode_i64(buf, sizeof(buf), val);
        ASSERT_NE(end, nullptr) << "encode failed for " << val;

        int64_t decoded = 0;
        auto* dend = decode_i64(buf, end - buf, decoded);
        ASSERT_NE(dend, nullptr) << "decode failed for " << val;
        EXPECT_EQ(decoded, val) << "roundtrip mismatch for " << val;
    }
}

// ---------------------------------------------------------------------------
// Nullable encoding/decoding
// ---------------------------------------------------------------------------

TEST(FastNullable, UnsignedNull) {
    uint8_t buf[16]{};
    auto* end = encode_nullable_u64(buf, sizeof(buf), std::nullopt);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], kNullByte);

    std::optional<uint64_t> decoded = 42;
    auto* dend = decode_nullable_u64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    EXPECT_FALSE(decoded.has_value());
}

TEST(FastNullable, UnsignedZeroValue) {
    uint8_t buf[16]{};
    // Value 0 encodes as wire value 1
    auto* end = encode_nullable_u64(buf, sizeof(buf), uint64_t{0});
    ASSERT_NE(end, nullptr);

    std::optional<uint64_t> decoded;
    auto* dend = decode_nullable_u64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), 0u);
}

TEST(FastNullable, UnsignedNonZero) {
    uint8_t buf[16]{};
    auto* end = encode_nullable_u64(buf, sizeof(buf), uint64_t{42});
    ASSERT_NE(end, nullptr);

    std::optional<uint64_t> decoded;
    auto* dend = decode_nullable_u64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), 42u);
}

TEST(FastNullable, SignedNull) {
    uint8_t buf[16]{};
    auto* end = encode_nullable_i64(buf, sizeof(buf), std::nullopt);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], kNullByte);

    std::optional<int64_t> decoded = 42;
    auto* dend = decode_nullable_i64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    EXPECT_FALSE(decoded.has_value());
}

TEST(FastNullable, SignedZero) {
    uint8_t buf[16]{};
    auto* end = encode_nullable_i64(buf, sizeof(buf), int64_t{0});
    ASSERT_NE(end, nullptr);

    std::optional<int64_t> decoded;
    auto* dend = decode_nullable_i64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), 0);
}

TEST(FastNullable, SignedNegative) {
    uint8_t buf[16]{};
    auto* end = encode_nullable_i64(buf, sizeof(buf), int64_t{-100});
    ASSERT_NE(end, nullptr);

    std::optional<int64_t> decoded;
    auto* dend = decode_nullable_i64(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), -100);
}

// ---------------------------------------------------------------------------
// Presence Map
// ---------------------------------------------------------------------------

TEST(FastPmap, EmptyPmap) {
    PresenceMap pmap{};
    uint8_t buf[4]{};
    auto* end = encode_pmap(buf, sizeof(buf), pmap);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    EXPECT_EQ(buf[0], 0x80);  // empty pmap, just stop bit

    PresenceMap decoded{};
    auto* dend = decode_pmap(buf, end - buf, decoded);
    ASSERT_NE(dend, nullptr);
    EXPECT_EQ(decoded.byte_count, 1u);
}

TEST(FastPmap, SingleBitSet) {
    PresenceMap pmap{};
    pmap.set_bit(0);  // first field present

    uint8_t buf[4]{};
    auto* end = encode_pmap(buf, sizeof(buf), pmap);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);
    // Bit 0 -> byte[0] bit 6 -> 0x40, plus stop bit -> 0xC0
    EXPECT_EQ(buf[0], 0xC0);

    PresenceMap decoded{};
    decode_pmap(buf, end - buf, decoded);
    decoded.reset_read();
    EXPECT_TRUE(decoded.next_bit());   // bit 0 set
    EXPECT_FALSE(decoded.next_bit());  // bit 1 not set
}

TEST(FastPmap, MultipleBitsSet) {
    PresenceMap pmap{};
    pmap.set_bit(0);
    pmap.set_bit(2);
    pmap.set_bit(4);

    uint8_t buf[4]{};
    auto* end = encode_pmap(buf, sizeof(buf), pmap);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 1);

    PresenceMap decoded{};
    decode_pmap(buf, end - buf, decoded);
    decoded.reset_read();
    EXPECT_TRUE(decoded.next_bit());   // 0
    EXPECT_FALSE(decoded.next_bit());  // 1
    EXPECT_TRUE(decoded.next_bit());   // 2
    EXPECT_FALSE(decoded.next_bit());  // 3
    EXPECT_TRUE(decoded.next_bit());   // 4
    EXPECT_FALSE(decoded.next_bit());  // 5
}

TEST(FastPmap, TwoBytesPmap) {
    PresenceMap pmap{};
    pmap.set_bit(0);  // byte 0, bit 6
    pmap.set_bit(7);  // byte 1, bit 6

    uint8_t buf[4]{};
    auto* end = encode_pmap(buf, sizeof(buf), pmap);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end - buf, 2);
    // byte 0: bit 6 set, no stop bit -> 0x40
    EXPECT_EQ(buf[0], 0x40);
    // byte 1: bit 6 set, stop bit -> 0xC0
    EXPECT_EQ(buf[1], 0xC0);

    PresenceMap decoded{};
    decode_pmap(buf, end - buf, decoded);
    EXPECT_EQ(decoded.byte_count, 2u);
    decoded.reset_read();
    EXPECT_TRUE(decoded.next_bit());   // 0
    for (int i = 1; i < 7; ++i)
        EXPECT_FALSE(decoded.next_bit());  // 1-6
    EXPECT_TRUE(decoded.next_bit());   // 7
}

// ---------------------------------------------------------------------------
// Message struct constants
// ---------------------------------------------------------------------------

TEST(FastMessageTypes, TemplateIds) {
    EXPECT_EQ(static_cast<uint32_t>(FastQuote::TEMPLATE_ID), 1u);
    EXPECT_EQ(static_cast<uint32_t>(FastTrade::TEMPLATE_ID), 2u);
    EXPECT_EQ(static_cast<uint32_t>(FastStatus::TEMPLATE_ID), 3u);
    EXPECT_EQ(static_cast<uint32_t>(FastSnapshot::TEMPLATE_ID), 4u);
}

TEST(FastMessageTypes, StructSizes) {
    // Verify structs are reasonably sized (not bloated)
    EXPECT_LE(sizeof(FastQuote), 48u);
    EXPECT_LE(sizeof(FastTrade), 32u);
    EXPECT_LE(sizeof(FastStatus), 16u);
    EXPECT_LE(sizeof(FastSnapshot), 64u);
}

}  // namespace
}  // namespace exchange::krx::fast
