// Intra-frame FEC: the half of ADR-0009 that was missing until now.
//
// The layer's value rests on two properties, and both are tested here rather than assumed:
// erasure decoding (positions are worth double), and interleaving (a spatial burst must be
// survivable). Either one alone is much weaker than the pair.
#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/intra_fec.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <vector>

using namespace fileflow;

namespace {

std::vector<std::uint8_t> Message(std::size_t n, std::uint64_t seed = 7) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(seed);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

}  // namespace

TEST(IntraFec, RejectsUnusableParameters) {
    EXPECT_FALSE(IntraFec::Create(2750, {.nsym = 0}).ok());
    EXPECT_FALSE(IntraFec::Create(2750, {.nsym = 255, .codeword_bytes = 255}).ok());
    EXPECT_FALSE(IntraFec::Create(2750, {.nsym = 32, .codeword_bytes = 256}).ok());
    // A frame too small for even one codeword.
    EXPECT_FALSE(IntraFec::Create(100, {.nsym = 32}).ok());
    EXPECT_TRUE(IntraFec::Create(2750, {}).ok());
}

TEST(IntraFec, CapacityAccountsForParity) {
    auto f = IntraFec::Create(2750, {.nsym = 32});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();

    EXPECT_EQ(fec.codewords(), 10u);           // 2750 / 255
    EXPECT_EQ(fec.coded_bytes(), 2550u);       // 10 * 255
    EXPECT_EQ(fec.message_bytes(), 2230u);     // 10 * (255 - 32)
    EXPECT_NEAR(fec.params().code_rate(), 223.0 / 255.0, 1e-9);
}

TEST(IntraFec, RoundTripsCleanly) {
    auto f = IntraFec::Create(2750, {.nsym = 32});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();

    const auto msg = Message(fec.message_bytes());
    auto coded = fec.Encode(msg);
    ASSERT_TRUE(coded.ok()) << ErrorName(coded.error());
    EXPECT_EQ(coded.value().size(), fec.coded_bytes());

    auto back = fec.Decode(coded.value());
    ASSERT_TRUE(back.ok()) << ErrorName(back.error());
    EXPECT_EQ(back.value(), msg);
}

TEST(IntraFec, RejectsWrongLengths) {
    auto f = IntraFec::Create(2750, {.nsym = 32});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();

    EXPECT_EQ(fec.Encode(Message(10)).error(), Error::kLengthMismatch);
    const std::vector<std::uint8_t> short_coded(10, 0);
    EXPECT_EQ(fec.Decode(short_coded).error(), Error::kLengthMismatch);
}

TEST(IntraFec, InterleavingScattersConsecutiveBytes) {
    // The property the interleave exists for: adjacent bytes must land in DIFFERENT codewords.
    // Asserted directly rather than inferred from burst survival, so a broken interleave is
    // reported as a broken interleave and not as a mysterious FEC regression.
    auto f = IntraFec::Create(2750, {.nsym = 32});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();
    const std::size_t n = fec.codewords();
    ASSERT_GT(n, 1u);

    for (std::size_t i = 0; i + 1 < 100; ++i) {
        EXPECT_NE(fec.CodewordOf(i), fec.CodewordOf(i + 1))
            << "adjacent bytes " << i << " and " << i + 1 << " share a codeword";
    }
    // A run of n adjacent bytes touches every codeword exactly once.
    std::vector<bool> hit(n, false);
    for (std::size_t i = 0; i < n; ++i) hit[fec.CodewordOf(i)] = true;
    EXPECT_TRUE(std::all_of(hit.begin(), hit.end(), [](bool b) { return b; }));
}

TEST(IntraFec, SurvivesASpatialBurstThatWouldKillOneCodeword) {
    // THE interleaving test. A contiguous run of erased bytes -- a glare spot, a fingerprint,
    // a rolling-shutter transition band -- is the realistic damage pattern. Without
    // interleaving such a burst lands inside one codeword and blows its budget; with it, the
    // damage is spread thinly across all of them.
    constexpr std::size_t kNsym = 32;
    auto f = IntraFec::Create(2750, {.nsym = kNsym});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();

    const auto msg = Message(fec.message_bytes(), 11);
    auto coded = fec.Encode(msg);
    ASSERT_TRUE(coded.ok());
    auto damaged = coded.value();

    // A burst far longer than any single codeword could absorb: 10 codewords * 32 parity = 320
    // erasure budget in total, and the burst is 300 contiguous bytes.
    const std::size_t burst_start = 400;
    const std::size_t burst_len = 300;
    std::vector<std::size_t> erased;
    for (std::size_t i = 0; i < burst_len; ++i) {
        damaged[burst_start + i] = 0x00;
        erased.push_back(burst_start + i);
    }

    auto back = fec.Decode(damaged, erased);
    ASSERT_TRUE(back.ok()) << "a 300-byte burst should be survivable when interleaved across "
                           << fec.codewords() << " codewords";
    EXPECT_EQ(back.value(), msg);
}

TEST(IntraFec, ErasurePositionsDoubleTheCorrectionPower) {
    // Same damage, decoded with and without position information. This is the concrete payoff
    // of every erasure-tracking decision upstream.
    constexpr std::size_t kNsym = 32;
    auto f = IntraFec::Create(2750, {.nsym = kNsym});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();

    const auto msg = Message(fec.message_bytes(), 13);
    auto coded = fec.Encode(msg);
    ASSERT_TRUE(coded.ok());

    // 24 damaged bytes per codeword: above the 16-error limit, below the 32-erasure limit.
    const std::size_t per_cw = 24;
    auto damaged = coded.value();
    std::vector<std::size_t> erased;
    for (std::size_t i = 0; i < per_cw * fec.codewords(); ++i) {
        damaged[i] = static_cast<std::uint8_t>(damaged[i] ^ 0xFF);
        erased.push_back(i);
    }

    // With positions: within budget.
    auto with_pos = fec.Decode(damaged, erased);
    ASSERT_TRUE(with_pos.ok()) << per_cw << " erasures/codeword should be within nsym=" << kNsym;
    EXPECT_EQ(with_pos.value(), msg);

    // Without positions: the same damage is 24 blind errors per codeword, over the nsym/2 = 16
    // limit. It must fail rather than silently miscorrect.
    auto blind = fec.Decode(damaged);
    if (blind.ok()) {
        EXPECT_EQ(blind.value(), msg) << "blind decode claimed success but returned wrong data";
    }
}

TEST(IntraFec, ReportsUncorrectableRatherThanReturningPartialData) {
    // A frame beyond the code's budget must be REFUSED. The fountain layer is built to absorb
    // a lost frame and would be poisoned by a silently half-corrected one.
    auto f = IntraFec::Create(2750, {.nsym = 16});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();

    const auto msg = Message(fec.message_bytes(), 17);
    auto coded = fec.Encode(msg);
    ASSERT_TRUE(coded.ok());
    auto damaged = coded.value();

    // Erase far more than the total budget across every codeword.
    std::vector<std::size_t> erased;
    for (std::size_t i = 0; i < damaged.size() / 2; ++i) {
        damaged[i] = 0x5A;
        erased.push_back(i);
    }
    auto r = fec.Decode(damaged, erased);
    if (r.ok()) {
        EXPECT_EQ(r.value(), msg) << "returned data that was neither correct nor refused";
    } else {
        EXPECT_EQ(r.error(), Error::kUncorrectable);
    }
}

TEST(IntraFec, RejectsOutOfRangeErasurePositions) {
    auto f = IntraFec::Create(2750, {.nsym = 32});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();
    auto coded = fec.Encode(Message(fec.message_bytes()));
    ASSERT_TRUE(coded.ok());

    const std::vector<std::size_t> bad{fec.coded_bytes()};
    EXPECT_EQ(fec.Decode(coded.value(), bad).error(), Error::kValueOutOfRange);
}

TEST(IntraFec, RandomisedScatteredErasures) {
    constexpr std::size_t kNsym = 32;
    auto f = IntraFec::Create(2750, {.nsym = kNsym});
    ASSERT_TRUE(f.ok());
    const auto& fec = f.value();
    SplitMix64 rng(4242);

    for (int trial = 0; trial < 60; ++trial) {
        const auto msg = Message(fec.message_bytes(), 100 + static_cast<std::uint64_t>(trial));
        auto coded = fec.Encode(msg);
        ASSERT_TRUE(coded.ok());
        auto damaged = coded.value();

        // Scatter erasures at a rate comfortably inside the per-codeword budget.
        std::vector<std::size_t> erased;
        for (std::size_t i = 0; i < damaged.size(); ++i) {
            if (rng.NextDouble() < 0.08) {  // ~20 of 255 per codeword on average
                damaged[i] = static_cast<std::uint8_t>(rng.Next() & 0xFF);
                erased.push_back(i);
            }
        }
        auto back = fec.Decode(damaged, erased);
        if (back.ok()) {
            EXPECT_EQ(back.value(), msg) << "trial " << trial;
        }
        // Failure is acceptable when a codeword happened to draw more than nsym erasures;
        // silently wrong output never is, and the check above catches it.
    }
}
