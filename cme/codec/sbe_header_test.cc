#include "cme/codec/sbe_header.h"

#include <cstring>
#include <gtest/gtest.h>

namespace exchange::cme::sbe {
namespace {

// ---------------------------------------------------------------------------
// MessageHeader encode/decode round-trip
// ---------------------------------------------------------------------------

TEST(MessageHeaderTest, RoundTrip) {
    MessageHeader original{};
    original.block_length = 200;
    original.template_id  = 514;
    original.schema_id    = ILINK3_SCHEMA_ID;
    original.version      = ILINK3_VERSION;

    char buf[sizeof(MessageHeader)];
    original.encode_to(buf);

    MessageHeader decoded{};
    MessageHeader::decode_from(buf, decoded);

    EXPECT_EQ(decoded.block_length, 200);
    EXPECT_EQ(decoded.template_id, 514);
    EXPECT_EQ(decoded.schema_id, ILINK3_SCHEMA_ID);
    EXPECT_EQ(decoded.version, ILINK3_VERSION);
}

TEST(MessageHeaderTest, Size) {
    EXPECT_EQ(sizeof(MessageHeader), 8u);
}

TEST(MessageHeaderTest, InPlaceEncodeDecode) {
    MessageHeader hdr{};
    hdr.block_length = 1234;
    hdr.template_id  = 5678;
    hdr.schema_id    = MDP3_SCHEMA_ID;
    hdr.version      = MDP3_VERSION;

    hdr.encode();
    hdr.decode();

    EXPECT_EQ(hdr.block_length, 1234);
    EXPECT_EQ(hdr.template_id, 5678);
    EXPECT_EQ(hdr.schema_id, MDP3_SCHEMA_ID);
    EXPECT_EQ(hdr.version, MDP3_VERSION);
}

// ---------------------------------------------------------------------------
// GroupHeader encode/decode round-trip
// ---------------------------------------------------------------------------

TEST(GroupHeaderTest, RoundTrip) {
    GroupHeader original{};
    original.block_length = 32;
    original.num_in_group = 7;

    char buf[sizeof(GroupHeader)];
    original.encode_to(buf);

    GroupHeader decoded{};
    GroupHeader::decode_from(buf, decoded);

    EXPECT_EQ(decoded.block_length, 32);
    EXPECT_EQ(decoded.num_in_group, 7);
}

TEST(GroupHeaderTest, Size) {
    EXPECT_EQ(sizeof(GroupHeader), 3u);
}

TEST(GroupHeader16Test, Size) {
    EXPECT_EQ(sizeof(GroupHeader16), 4u);
}

TEST(GroupHeader16Test, RoundTrip) {
    GroupHeader16 hdr{};
    hdr.block_length = 64;
    hdr.num_in_group = 300;

    hdr.encode();
    hdr.decode();

    EXPECT_EQ(hdr.block_length, 64);
    EXPECT_EQ(hdr.num_in_group, 300);
}

// ---------------------------------------------------------------------------
// Endian helpers
// ---------------------------------------------------------------------------

TEST(EndianTest, RoundTrip16) {
    uint16_t val = 0xABCD;
    EXPECT_EQ(decode_le16(encode_le16(val)), val);
}

TEST(EndianTest, RoundTrip32) {
    uint32_t val = 0xDEADBEEF;
    EXPECT_EQ(decode_le32(encode_le32(val)), val);
}

TEST(EndianTest, RoundTrip64) {
    uint64_t val = 0x0123456789ABCDEFull;
    EXPECT_EQ(decode_le64(encode_le64(val)), val);
}

TEST(EndianTest, SignedRoundTrip64) {
    int64_t val = -123456789012345ll;
    EXPECT_EQ(decode_le64_signed(encode_le64_signed(val)), val);
}

TEST(EndianTest, SignedZero) {
    EXPECT_EQ(decode_le64_signed(encode_le64_signed(0)), 0);
}

TEST(EndianTest, WireFormatLittleEndian) {
    // Verify that encoding 0x0102 produces bytes [0x02, 0x01] (little-endian)
    uint16_t val = 0x0102;
    uint16_t encoded = encode_le16(val);
    uint8_t bytes[2];
    std::memcpy(bytes, &encoded, 2);
    EXPECT_EQ(bytes[0], 0x02);
    EXPECT_EQ(bytes[1], 0x01);
}

// ---------------------------------------------------------------------------
// Null sentinel values
// ---------------------------------------------------------------------------

TEST(NullSentinelTest, UInt8) {
    EXPECT_TRUE(is_null(UINT8_NULL));
    EXPECT_FALSE(is_null(static_cast<uint8_t>(0)));
    EXPECT_FALSE(is_null(static_cast<uint8_t>(254)));
}

TEST(NullSentinelTest, UInt16) {
    EXPECT_TRUE(is_null(UINT16_NULL));
    EXPECT_FALSE(is_null(static_cast<uint16_t>(0)));
    EXPECT_FALSE(is_null(static_cast<uint16_t>(65534)));
}

TEST(NullSentinelTest, UInt32) {
    EXPECT_TRUE(is_null(UINT32_NULL));
    EXPECT_FALSE(is_null(static_cast<uint32_t>(0)));
}

TEST(NullSentinelTest, UInt64) {
    EXPECT_TRUE(is_null(UINT64_NULL));
    EXPECT_FALSE(is_null(static_cast<uint64_t>(0)));
}

TEST(NullSentinelTest, Int32) {
    EXPECT_TRUE(is_null_i32(INT32_NULL));
    EXPECT_FALSE(is_null_i32(0));
    EXPECT_FALSE(is_null_i32(-1));
}

TEST(NullSentinelTest, Int64) {
    EXPECT_TRUE(is_null_i64(INT64_NULL));
    EXPECT_FALSE(is_null_i64(0));
    EXPECT_FALSE(is_null_i64(-1));
}

TEST(NullSentinelTest, CorrectValues) {
    EXPECT_EQ(UINT8_NULL, 255);
    EXPECT_EQ(UINT16_NULL, 65535);
    EXPECT_EQ(UINT32_NULL, 4294967295u);
    EXPECT_EQ(UINT64_NULL, 18446744073709551615ull);
    EXPECT_EQ(INT32_NULL, 2147483647);
    EXPECT_EQ(INT64_NULL, 9223372036854775807ll);
    EXPECT_EQ(INT8_NULL, 127);
    EXPECT_EQ(ENUM_NULL, 255);
}

// ---------------------------------------------------------------------------
// PRICE9 conversion
// ---------------------------------------------------------------------------

TEST(Price9Test, FromDouble) {
    PRICE9 p = PRICE9::from_double(123.456789012);
    // 123.456789012 * 1e9 = 123456789012 (with rounding)
    EXPECT_EQ(p.mantissa, 123456789012ll);
}

TEST(Price9Test, ToDouble) {
    PRICE9 p{123456789012ll};
    double result = p.to_double();
    EXPECT_NEAR(result, 123.456789012, 1e-6);
}

TEST(Price9Test, RoundTrip) {
    double original = 99999.123456789;
    PRICE9 p = PRICE9::from_double(original);
    double result = p.to_double();
    EXPECT_NEAR(result, original, 1e-6);
}

TEST(Price9Test, Zero) {
    PRICE9 p = PRICE9::from_double(0.0);
    EXPECT_EQ(p.mantissa, 0);
    EXPECT_DOUBLE_EQ(p.to_double(), 0.0);
}

TEST(Price9Test, Negative) {
    PRICE9 p = PRICE9::from_double(-50.5);
    EXPECT_LT(p.mantissa, 0);
    EXPECT_NEAR(p.to_double(), -50.5, 1e-6);
}

TEST(Price9Test, Null) {
    PRICE9 p{INT64_NULL};
    EXPECT_TRUE(p.is_null());
}

TEST(Price9Test, NotNull) {
    PRICE9 p{12345ll};
    EXPECT_FALSE(p.is_null());
}

TEST(Price9Test, EncodeDecode) {
    PRICE9 p = PRICE9::from_double(42.123456789);
    int64_t original_mantissa = p.mantissa;
    p.encode();
    p.decode();
    EXPECT_EQ(p.mantissa, original_mantissa);
}

// ---------------------------------------------------------------------------
// Decimal9 alias
// ---------------------------------------------------------------------------

TEST(Decimal9Test, IsAlias) {
    // Decimal9 is a type alias for PRICE9
    Decimal9 d{500000000ll};  // 0.5
    EXPECT_NEAR(d.to_double(), 0.5, 1e-9);
}

// ---------------------------------------------------------------------------
// MaturityMonthYear
// ---------------------------------------------------------------------------

TEST(MaturityMonthYearTest, Size) {
    EXPECT_EQ(sizeof(MaturityMonthYear), 5u);
}

TEST(MaturityMonthYearTest, Values) {
    MaturityMonthYear m{};
    m.year  = 2024;
    m.month = 12;
    m.day   = 15;
    m.week  = 3;

    m.encode();
    m.decode();

    EXPECT_EQ(m.year, 2024);
    EXPECT_EQ(m.month, 12);
    EXPECT_EQ(m.day, 15);
    EXPECT_EQ(m.week, 3);
}

TEST(MaturityMonthYearTest, Null) {
    MaturityMonthYear m{};
    m.year  = UINT16_NULL;
    m.month = UINT8_NULL;
    m.day   = UINT8_NULL;
    m.week  = UINT8_NULL;
    EXPECT_TRUE(m.is_null());
}

TEST(MaturityMonthYearTest, NotNull) {
    MaturityMonthYear m{};
    m.year  = 2025;
    m.month = 3;
    m.day   = UINT8_NULL;
    m.week  = UINT8_NULL;
    EXPECT_FALSE(m.is_null());
}

// ---------------------------------------------------------------------------
// Schema constants
// ---------------------------------------------------------------------------

TEST(SchemaConstantsTest, ILink3) {
    EXPECT_EQ(ILINK3_SCHEMA_ID, 8);
    EXPECT_EQ(ILINK3_VERSION, 5);
}

TEST(SchemaConstantsTest, MDP3) {
    EXPECT_EQ(MDP3_SCHEMA_ID, 1);
    EXPECT_EQ(MDP3_VERSION, 13);
}

}  // namespace
}  // namespace exchange::cme::sbe
