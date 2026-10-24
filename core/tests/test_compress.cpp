#include <fileflow/compress.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fileflow {
namespace {

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

std::vector<std::uint8_t> Noise(std::size_t n) {
    std::vector<std::uint8_t> out(n);
    std::uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (auto& b : out) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        b = static_cast<std::uint8_t>(x);
    }
    return out;
}

}  // namespace

TEST(Compress, TextShrinksAndComesBackExactly) {
    const auto data = Text(500'000);
    const auto packed = Compress(data, {.level = 19});
    ASSERT_TRUE(packed.ok());
    EXPECT_LT(packed.value().size(), data.size() / 4) << "text should shrink several times";
    const auto back = Decompress(packed.value(), data.size());
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), data);
}

TEST(Compress, HigherLevelsAreNoWorse) {
    const auto data = Text(300'000);
    const auto fast = Compress(data, {.level = 1});
    const auto best = Compress(data, {.level = MaxCompressionLevel(), .long_window = true});
    ASSERT_TRUE(fast.ok() && best.ok());
    EXPECT_LE(best.value().size(), fast.value().size());
    EXPECT_EQ(Decompress(best.value(), data.size()).value(), data);
}

TEST(Compress, NoiseRoundTripsAtAboutItsOwnSize) {
    const auto data = Noise(200'000);
    const auto packed = Compress(data, {.level = 3});
    ASSERT_TRUE(packed.ok());
    EXPECT_LT(packed.value().size(), data.size() + 1024);
    EXPECT_EQ(Decompress(packed.value(), data.size()).value(), data);
}

TEST(Compress, EmptyRoundTrips) {
    const std::vector<std::uint8_t> empty;
    const auto packed = Compress(empty);
    ASSERT_TRUE(packed.ok());
    const auto back = Decompress(packed.value(), 0);
    ASSERT_TRUE(back.ok());
    EXPECT_TRUE(back.value().empty());
}

TEST(Compress, ASizeLieIsRefusedNotObeyed) {
    // A sender's declared size is a bound. A stream claiming or producing anything else -- the
    // cheapest form of decompression bomb is "this tiny stream is 4 GB" -- fails.
    const auto data = std::vector<std::uint8_t>(1'000'000, 0);
    const auto packed = Compress(data);
    ASSERT_TRUE(packed.ok());
    EXPECT_FALSE(Decompress(packed.value(), data.size() - 1).ok());
    EXPECT_FALSE(Decompress(packed.value(), data.size() + 1).ok());
}

TEST(Compress, GarbageAndTruncationAreRefused) {
    const auto data = Text(50'000);
    auto packed = Compress(data).value();
    EXPECT_FALSE(Decompress(std::vector<std::uint8_t>{1, 2, 3, 4, 5}, 10).ok());
    const std::vector<std::uint8_t> cut(packed.begin(), packed.begin() + static_cast<long>(packed.size() / 2));
    EXPECT_FALSE(Decompress(cut, data.size()).ok());
    packed[packed.size() / 2] ^= 0x5A;
    const auto flipped = Decompress(packed, data.size());
    // A flipped byte may still decode -- zstd has no checksum here; the transfer's SHA-256 is the
    // integrity check -- but it must never produce the wrong SIZE.
    if (flipped.ok()) {
        EXPECT_EQ(flipped.value().size(), data.size());
    }
}

}  // namespace fileflow
