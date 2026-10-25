#include <fileflow/compress.h>
#include <fileflow/envelope.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fileflow {
namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::string Hex(const std::vector<std::uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (auto v : b) {
        out += d[v >> 4];
        out += d[v & 15];
    }
    return out;
}

std::vector<std::uint8_t> Text(std::size_t n) {
    const std::string words[] = {"light ", "camera ", "screen ", "fountain ", "symbol ", "frame "};
    std::vector<std::uint8_t> out;
    std::uint32_t x = 12345;
    while (out.size() < n) {
        x = x * 1103515245U + 12345U;
        const std::string& w = words[(x >> 16) % 6];
        out.insert(out.end(), w.begin(), w.end());
    }
    out.resize(n);
    return out;
}

}  // namespace

// The same bytes are opened by the app's Envelope.kt in EnvelopeTest.desktopEnvelopesOpen: the two
// sides of the format are checked against one shared vector, not each against itself.
TEST(Envelope, OneFileIsTheAppsFormat) {
    auto p = PackEnvelope(Bytes("hello"), "x.txt", CompressionMode::kOff);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(Hex(p.value().bytes),
              "4646453200" "0005" "782e747874" "0000000000000005" "00" "00000000" "68656c6c6f");
    EXPECT_EQ(p.value().codec, 0);
    EXPECT_EQ(p.value().original_size, 5U);
}

TEST(Envelope, ABatchIsTheAppsFormat) {
    auto p = PackBatchEnvelope({{"a.txt", Bytes("hi")}, {"b", Bytes("!")}}, "2 files",
                               CompressionMode::kOff);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(Hex(p.value().bytes),
              "4646453204" "0007" "322066696c6573" "0000000000000023" "00" "00000000"
              "46464231" "0002" "0005" "612e747874" "0000000000000002" "6869"
              "0001" "62" "0000000000000001" "21");
}

TEST(Envelope, TextIsCompressedAndComesBack) {
    const auto data = Text(200'000);
    for (auto mode : {CompressionMode::kAuto, CompressionMode::kMaximum}) {
        auto p = PackEnvelope(data, "notes.txt", mode);
        ASSERT_TRUE(p.ok());
        EXPECT_EQ(p.value().codec, 2);
        const auto& b = p.value().bytes;
        const std::size_t header = 5 + 2 + 9 + 8 + 1 + 4;
        ASSERT_LT(b.size(), data.size() / 4);
        EXPECT_EQ(b[header - 5], 2);  // codec
        auto back = Decompress({b.data() + header, b.size() - header}, data.size());
        ASSERT_TRUE(back.ok());
        EXPECT_EQ(back.value(), data);
    }
}

TEST(Envelope, WhatDoesNotCompressIsSentAsIs) {
    std::vector<std::uint8_t> noise(100'000);
    std::uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (auto& v : noise) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        v = static_cast<std::uint8_t>(x);
    }
    auto p = PackEnvelope(noise, "photo.jpg", CompressionMode::kMaximum);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().codec, 0);
    EXPECT_EQ(p.value().bytes.size(), noise.size() + 5 + 2 + 9 + 8 + 1 + 4);
}

TEST(Envelope, LongNamesAreCutAndBatchesAreBounded) {
    auto p = PackEnvelope(Bytes("x"), std::string(400, 'n'), CompressionMode::kOff);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().bytes[5], 0);
    EXPECT_EQ(p.value().bytes[6], 255);
    EXPECT_EQ(PackBatchEnvelope({}, "none", CompressionMode::kOff).error(), Error::kValueOutOfRange);
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> many(kEnvelopeMaxBatchFiles + 1,
                                                                        {"f", Bytes("1")});
    EXPECT_EQ(PackBatchEnvelope(many, "many", CompressionMode::kOff).error(), Error::kValueOutOfRange);
}

}  // namespace fileflow
