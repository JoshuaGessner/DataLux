// Frame header codec — parses attacker-controlled bytes.
// Malformed-input cases here double as the seed corpus for fuzz/fuzz_frame_header.cpp.
#include <fileflow/frame.h>

#include <fileflow/hash.h>
#include <gtest/gtest.h>

#include <vector>

using namespace fileflow;

namespace {
FrameHeader Sample() {
    FrameHeader h;
    h.session_id = 0xDEADBEEF;
    h.sequence = 12345;
    h.profile = ModulationProfile::kM0BinaryLuminance;
    h.phase = 7;
    h.block_id = 3;
    h.esi = 999;
    h.payload_bytes = 256;
    h.flags = FrameHeader::kFlagLastBlock;
    h.fec_nsym = 32;
    return h;
}
}  // namespace

TEST(FrameHeader, PlainRoundTrip) {
    const auto h = Sample();
    const auto bytes = h.EncodePlain();
    ASSERT_EQ(bytes.size(), FrameHeader::kPlainSize);

    auto d = FrameHeader::DecodePlain(bytes);
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().session_id, h.session_id);
    EXPECT_EQ(d.value().sequence, h.sequence);
    EXPECT_EQ(d.value().profile, h.profile);
    EXPECT_EQ(d.value().phase, h.phase);
    EXPECT_EQ(d.value().block_id, h.block_id);
    EXPECT_EQ(d.value().esi, h.esi);
    EXPECT_EQ(d.value().payload_bytes, h.payload_bytes);
    EXPECT_EQ(d.value().flags, h.flags);
    EXPECT_EQ(d.value().fec_nsym, h.fec_nsym);
}

TEST(FrameHeader, PreviousVersionIsRefusedNotMisparsed) {
    // v1 used LT repair rows; decoding its symbols against v2's dense rows would fail only at the
    // final hash, after the whole transfer had been spent (ADR-0015).
    auto h = Sample();
    h.version = kProtocolVersion - 1;
    EXPECT_EQ(FrameHeader::DecodePlain(h.EncodePlain()).error(), Error::kUnsupportedVersion);
}

TEST(FrameHeader, ImpossibleCodeRateIsRejected) {
    auto h = Sample();
    h.fec_nsym = 255;  // no message bytes left in a 255-byte codeword
    EXPECT_EQ(FrameHeader::DecodePlain(h.EncodePlain()).error(), Error::kValueOutOfRange);
}

TEST(FrameHeader, EveryTruncationIsRejected) {
    const auto bytes = Sample().EncodePlain();
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        auto d = FrameHeader::DecodePlain(std::span(bytes.data(), n));
        EXPECT_FALSE(d.ok()) << "accepted a " << n << "-byte header";
    }
}

TEST(FrameHeader, AnySingleBitFlipIsCaught) {
    // The CRC sits above the FEC layer precisely to catch miscorrection (THREAT-MODEL T5).
    const auto bytes = Sample().EncodePlain();
    for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            auto corrupt = bytes;
            corrupt[byte] ^= static_cast<std::uint8_t>(1u << bit);
            auto d = FrameHeader::DecodePlain(corrupt);
            EXPECT_FALSE(d.ok()) << "byte " << byte << " bit " << bit << " slipped through";
        }
    }
}

TEST(FrameHeader, BadMagicIsRejected) {
    auto bytes = Sample().EncodePlain();
    bytes[0] = 0x00;
    // Fix the CRC so we are testing the magic check, not the CRC check.
    const std::uint32_t crc = Crc32(std::span(bytes.data(), FrameHeader::kPlainSize - 4));
    for (int i = 0; i < 4; ++i) {
        bytes[FrameHeader::kPlainSize - 4 + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((crc >> ((3 - i) * 8)) & 0xFF);
    }
    EXPECT_EQ(FrameHeader::DecodePlain(bytes).error(), Error::kBadMagic);
}

TEST(FrameHeader, FutureVersionIsRefusedNotMisparsed) {
    auto h = Sample();
    h.version = kProtocolVersion + 1;
    EXPECT_EQ(FrameHeader::DecodePlain(h.EncodePlain()).error(), Error::kUnsupportedVersion);
}

TEST(FrameHeader, UnknownProfileIsRejected) {
    auto h = Sample();
    h.profile = static_cast<ModulationProfile>(200);
    EXPECT_EQ(FrameHeader::DecodePlain(h.EncodePlain()).error(), Error::kUnknownProfile);
}

TEST(FrameHeader, OversizedPayloadLengthIsRejectedBeforeUse) {
    // A hostile length field must be bounded BEFORE it sizes anything (INPUT-VALIDATION SR-1).
    auto h = Sample();
    h.payload_bytes = FrameHeader::kMaxPayloadBytes + 1;
    EXPECT_EQ(FrameHeader::DecodePlain(h.EncodePlain()).error(), Error::kValueOutOfRange);
}

TEST(FrameHeader, ArbitraryGarbageNeverCrashes) {
    // Cheap stand-in for fuzzing until the fuzz targets run in CI.
    std::uint64_t state = 1;
    auto next = [&state]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint8_t>(state >> 33);
    };
    for (int trial = 0; trial < 20000; ++trial) {
        std::vector<std::uint8_t> buf(FrameHeader::kPlainSize);
        for (auto& b : buf) b = next();
        auto d = FrameHeader::DecodePlain(buf);
        (void)d;  // must not crash, hang, or read out of bounds
    }
}

// ---------------------------------------------------------------- codec

TEST(HeaderCodec, RoundTripsCleanly) {
    const HeaderCodec codec(32);
    const auto h = Sample();
    auto coded = codec.Encode(h);
    ASSERT_TRUE(coded.ok());
    EXPECT_EQ(coded.value().size(), codec.coded_size());

    auto buf = coded.value();
    auto d = codec.Decode(buf);
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().sequence, h.sequence);
}

TEST(HeaderCodec, CorrectsSymbolErrorsWithinItsBudget) {
    const HeaderCodec codec(32);  // corrects 16 symbols
    const auto h = Sample();
    auto coded = codec.Encode(h);
    ASSERT_TRUE(coded.ok());

    auto buf = coded.value();
    for (std::size_t i = 0; i < 16; ++i) buf[i * 2] ^= 0xFF;

    auto d = codec.Decode(buf);
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().sequence, h.sequence);
    EXPECT_EQ(d.value().session_id, h.session_id);
}

TEST(HeaderCodec, BeyondBudgetFailsRatherThanLying) {
    const HeaderCodec codec(32);
    auto coded = codec.Encode(Sample());
    ASSERT_TRUE(coded.ok());

    auto buf = coded.value();
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] ^= static_cast<std::uint8_t>(0x5A + i);

    auto d = codec.Decode(buf);
    if (d.ok()) {
        // Miscorrection that survives RS must still be caught by the CRC.
        EXPECT_EQ(d.value().sequence, Sample().sequence);
    } else {
        SUCCEED();
    }
}

TEST(HeaderCodec, ErasuresDoubleTheCorrectableDamage) {
    // F50. 30 damaged bytes is far past the 16 errors nsym = 32 can locate blind, and well inside
    // the 32 erasures it can fill when told where they are.
    const HeaderCodec codec(32);
    const auto h = Sample();
    auto coded = codec.Encode(h);
    ASSERT_TRUE(coded.ok());

    std::vector<std::size_t> erased;
    auto buf = coded.value();
    for (std::size_t i = 0; i < 30; ++i) {
        buf[i * 2] = 0x00;  // what an all-abstaining vote used to produce
        erased.push_back(i * 2);
    }
    auto blind = buf;
    EXPECT_FALSE(codec.Decode(blind).ok()) << "30 blind errors must be past the budget";

    auto d = codec.Decode(buf, erased);
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().sequence, h.sequence);
    EXPECT_EQ(d.value().session_id, h.session_id);
}

TEST(HeaderCodec, WrongErasureFlagsNeverCostASuccess) {
    // Flags on bytes that were in fact correct spend budget for nothing. With errors elsewhere
    // the flagged attempt can fail where a blind one succeeds, so the codec must fall back.
    const HeaderCodec codec(32);
    const auto h = Sample();
    auto coded = codec.Encode(h);
    ASSERT_TRUE(coded.ok());

    auto buf = coded.value();
    std::vector<std::size_t> erased;
    for (std::size_t i = 0; i < 20; ++i) erased.push_back(i);   // all correct, all flagged
    for (std::size_t i = 0; i < 12; ++i) buf[30 + i] ^= 0x3C;   // 12 real errors elsewhere

    auto d = codec.Decode(buf, erased);
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().sequence, h.sequence);
}

TEST(HeaderCodec, OutOfRangeErasureIsRejected) {
    const HeaderCodec codec(32);
    auto coded = codec.Encode(Sample());
    ASSERT_TRUE(coded.ok());
    auto buf = coded.value();
    const std::vector<std::size_t> bad{buf.size()};
    EXPECT_EQ(codec.Decode(buf, bad).error(), Error::kValueOutOfRange);
}

TEST(HeaderCodec, WrongLengthIsRejected) {
    const HeaderCodec codec(32);
    std::vector<std::uint8_t> buf(10, 0);
    EXPECT_EQ(codec.Decode(buf).error(), Error::kLengthMismatch);
}
