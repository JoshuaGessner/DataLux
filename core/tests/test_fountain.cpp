#include <fileflow/fountain.h>

#include <gtest/gtest.h>

#include <bit>
#include <set>
#include <vector>

using namespace fileflow;

namespace {

std::vector<std::uint8_t> Payload(std::size_t n, std::uint64_t seed = 42) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(seed);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

FountainParams Params(std::uint32_t k, std::uint32_t sym) {
    FountainParams p;
    p.source_symbols = k;
    p.symbol_size = sym;
    p.session_seed = 0xC0FFEE;
    return p;
}

}  // namespace

TEST(SplitMix64, IsDeterministicAcrossInstances) {
    // The decoder regenerates each symbol's neighbour set from its ESI alone, so this MUST
    // be reproducible or nothing decodes.
    SplitMix64 a(12345), b(12345);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(a.Next(), b.Next());
}

TEST(SplitMix64, BelowStaysInRange) {
    SplitMix64 rng(7);
    for (int i = 0; i < 10000; ++i) {
        const std::uint32_t v = rng.Below(37);
        EXPECT_LT(v, 37u);
    }
    EXPECT_EQ(rng.Below(1), 0u);
    EXPECT_EQ(rng.Below(0), 0u);
}

TEST(Fountain, ParamsRejectDegenerateValues) {
    EXPECT_EQ(Params(0, 256).Validate().error(), Error::kValueOutOfRange);
    EXPECT_EQ(Params(64, 0).Validate().error(), Error::kValueOutOfRange);

    FountainParams huge = Params(64, 256);
    huge.source_symbols = FountainParams::kMaxSourceSymbols + 1;
    EXPECT_EQ(huge.Validate().error(), Error::kValueOutOfRange);
}

TEST(Fountain, RowsAreDeterministicDenseAndNeverEmpty) {
    // Encoder and decoder regenerate rows independently; any disagreement and nothing decodes.
    for (const std::uint32_t k : {1u, 2u, 3u, 63u, 64u, 65u, 200u}) {
        const auto p = Params(k, 8);
        const FountainRows a(p), b(p);
        std::vector<std::uint64_t> ra(a.words()), rb(b.words());
        std::uint64_t ones = 0;
        for (std::uint32_t esi = k; esi < k + 200; ++esi) {
            a.Row(esi, ra.data());
            b.Row(esi, rb.data());
            ASSERT_EQ(ra, rb) << "k=" << k << " esi=" << esi;
            std::uint64_t n = 0;
            for (std::size_t w = 0; w < ra.size(); ++w) {
                n += static_cast<std::uint64_t>(std::popcount(ra[w]));
                if (w + 1 == ra.size() && k % 64 != 0) {
                    ASSERT_EQ(ra[w] >> (k % 64), 0u) << "bit set past k";
                }
            }
            ASSERT_GT(n, 0u) << "an empty row can never help";
            ones += n;
        }
        // Dense means ~k/2 per row. Low density is exactly what made v1's repair symbols useless.
        if (k >= 63) {
            EXPECT_NEAR(static_cast<double>(ones) / 200.0, k / 2.0, k * 0.1) << "k=" << k;
        }
    }
}

TEST(Fountain, IsSystematic) {
    // Source symbols first: a clean channel then decodes with almost no peeling work.
    const auto p = Params(32, 64);
    const auto data = Payload(32 * 64);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());

    for (std::uint32_t i = 0; i < p.source_symbols; ++i) {
        const auto sym = enc.value().Symbol(i);
        ASSERT_EQ(sym.size(), p.symbol_size);
        for (std::uint32_t b = 0; b < p.symbol_size; ++b) {
            EXPECT_EQ(sym[b], data[i * p.symbol_size + b]) << "esi=" << i << " b=" << b;
        }
    }
}

TEST(Fountain, DecodesFromSystematicPrefixAlone) {
    const auto p = Params(64, 128);
    const auto data = Payload(64 * 128);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());

    for (std::uint32_t i = 0; i < p.source_symbols; ++i) {
        dec.value().Ingest(i, enc.value().Symbol(i));
    }
    ASSERT_TRUE(dec.value().complete());
    EXPECT_DOUBLE_EQ(dec.value().overhead(), 0.0);
    EXPECT_EQ(dec.value().Take().value(), data);
}

TEST(Fountain, RecoversFromRepairSymbolsAfterLoss) {
    const auto p = Params(64, 128);
    const auto data = Payload(64 * 128);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec_r = FountainDecoder::Create(p);
    ASSERT_TRUE(dec_r.ok());
    auto& dec = dec_r.value();

    // Drop every 3rd systematic symbol, then feed repair symbols until complete.
    for (std::uint32_t i = 0; i < p.source_symbols; ++i) {
        if (i % 3 == 0) continue;
        dec.Ingest(i, enc.value().Symbol(i));
    }
    ASSERT_FALSE(dec.complete());

    std::uint32_t esi = p.source_symbols;
    while (!dec.complete() && esi < p.source_symbols * 20) {
        dec.Ingest(esi, enc.value().Symbol(esi));
        ++esi;
    }

    ASSERT_TRUE(dec.complete()) << "failed to decode after " << esi << " symbols";
    EXPECT_EQ(dec.Take().value(), data);
}

TEST(Fountain, ToleratesDuplicatesAndOutOfOrder) {
    const auto p = Params(32, 64);
    const auto data = Payload(32 * 64);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec_r = FountainDecoder::Create(p);
    ASSERT_TRUE(dec_r.ok());
    auto& dec = dec_r.value();

    // Reverse order, every symbol delivered three times.
    for (std::uint32_t i = p.source_symbols; i-- > 0;) {
        for (int rep = 0; rep < 3; ++rep) dec.Ingest(i, enc.value().Symbol(i));
    }
    ASSERT_TRUE(dec.complete());
    EXPECT_EQ(dec.Take().value(), data);
}

TEST(Fountain, IgnoresWrongSizedSymbols) {
    const auto p = Params(16, 64);
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());
    const std::vector<std::uint8_t> wrong(7, 0xFF);
    EXPECT_FALSE(dec.value().Ingest(0, wrong));
    EXPECT_EQ(dec.value().recovered(), 0u);
}

TEST(Fountain, IncompleteTakeIsRefused) {
    const auto p = Params(16, 64);
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().Take().error(), Error::kFountainIncomplete);
}

// Measures the Rfountain term the goodput model needs. This is the seed of EXP-012.
TEST(Fountain, RepairOnlyOverheadIsBounded) {
    const auto p = Params(128, 64);
    const auto data = Payload(128 * 64);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());

    double worst = 0.0;
    for (std::uint32_t trial = 0; trial < 8; ++trial) {
        FountainParams tp = p;
        tp.session_seed = 0xC0FFEE + trial;
        auto e = FountainEncoder::Create(tp, data);
        ASSERT_TRUE(e.ok());
        auto d = FountainDecoder::Create(tp);
        ASSERT_TRUE(d.ok());

        // Skip the systematic prefix entirely: repair symbols only, the worst case.
        std::uint32_t esi = tp.source_symbols;
        while (!d.value().complete() && esi < tp.source_symbols * 50) {
            d.value().Ingest(esi, e.value().Symbol(esi));
            ++esi;
        }
        ASSERT_TRUE(d.value().complete()) << "trial " << trial;
        EXPECT_EQ(d.value().Take().value(), data) << "trial " << trial;
        worst = std::max(worst, d.value().overhead());
    }

    // A dense GF(2) row set reaches full rank within a handful of symbols of k. The v1 LT code
    // needed up to 2.0 here (that was this test's bound).
    EXPECT_LT(worst, 0.1) << "repair-only reception overhead regressed";
}

// THE REGRESSION GUARD FOR F49. Systematic prefix, i.i.d. frame loss, symbols in ESI order --
// exactly what the transmitter does. v1 measured 0.20-0.33 overhead here; the bound below is
// several times the dense code's expectation (~0.025 at k = 64) and far below v1, so it fails
// if the repair rows ever lose their density again.
TEST(Fountain, OverheadUnderLossIsNearOptimal) {
    const auto p0 = Params(64, 16);
    const auto data = Payload(64 * 16);
    for (const double loss : {0.05, 0.10, 0.30}) {
        double sum = 0.0;
        constexpr int kTrials = 200;
        for (int t = 0; t < kTrials; ++t) {
            FountainParams p = p0;
            p.session_seed = 0x5EED0000u + static_cast<std::uint32_t>(t);
            auto enc = FountainEncoder::Create(p, data);
            ASSERT_TRUE(enc.ok());
            auto dec = FountainDecoder::Create(p);
            ASSERT_TRUE(dec.ok());
            SplitMix64 rng(static_cast<std::uint64_t>(t) * 977 + static_cast<std::uint64_t>(loss * 100));
            for (std::uint32_t esi = 0; !dec.value().complete() && esi < 64 * 20; ++esi) {
                if (rng.NextDouble() < loss) continue;
                dec.value().Ingest(esi, enc.value().Symbol(esi));
            }
            ASSERT_TRUE(dec.value().complete());
            ASSERT_EQ(dec.value().Take().value(), data);
            sum += dec.value().overhead();
        }
        EXPECT_LT(sum / kTrials, 0.06) << "loss " << loss;
    }
}

TEST(Fountain, PrimedSymbolsAreKnownWithoutBeingReceived) {
    // The last block's zero padding: the receiver knows it from the manifest, so it must never
    // have to be transmitted -- and supplying it must not count as reception.
    const auto p = Params(16, 32);
    std::vector<std::uint8_t> data = Payload(10 * 32);  // symbols 10..15 are padding
    data.resize(16 * 32, 0);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());

    const std::vector<std::uint8_t> zero(32, 0);
    for (std::uint32_t i = 10; i < 16; ++i) dec.value().Prime(i, zero);
    EXPECT_EQ(dec.value().recovered(), 6u);
    EXPECT_EQ(dec.value().ingested(), 0u);

    // Lose source symbol 3; repair symbols fill it in.
    for (std::uint32_t i = 0; i < 10; ++i) {
        if (i != 3) dec.value().Ingest(i, enc.value().Symbol(i));
    }
    for (std::uint32_t esi = 16; !dec.value().complete() && esi < 100; ++esi) {
        dec.value().Ingest(esi, enc.value().Symbol(esi));
    }
    ASSERT_TRUE(dec.value().complete());
    EXPECT_EQ(dec.value().Take().value(), data);
    EXPECT_EQ(dec.value().primed(), 6u);
    EXPECT_GE(dec.value().overhead(), 0.0);
}
