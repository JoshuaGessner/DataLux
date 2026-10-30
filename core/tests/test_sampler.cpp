#include <fileflow/photometric.h>
#include <fileflow/sampler.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_util.h"

using namespace fileflow;
using namespace fileflow::test;

namespace {

// A checkerboard is the worst case for crosstalk: every cell's neighbours are its opposite,
// so any sampling error shows up immediately as a mid-grey reading.
CellMatrix Checkerboard(const GridGeometry& g) {
    CellMatrix m(g.cols, g.rows);
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            m.set(c, r, ((c + r) % 2 == 0) ? kLevelBright : kLevelDark);
        }
    }
    return m;
}

Homography FitGrid(const GridGeometry& g, const std::array<Point2, 4>& quad) {
    const auto corners = GridCorners(g);
    auto h = Homography::FromCorrespondences(Span4(corners), Span4(quad));
    EXPECT_TRUE(h.ok());
    return h.value();
}

}  // namespace

TEST(CellSampler, RejectsInvalidConfig) {
    const GridGeometry g{24, 40};
    EXPECT_FALSE(CellSampler::Create(g, {.interior_margin = 0.5}).ok());
    EXPECT_FALSE(CellSampler::Create(g, {.interior_margin = -0.1}).ok());
    EXPECT_FALSE(CellSampler::Create(g, {.samples_per_axis = 0}).ok());
    EXPECT_FALSE(CellSampler::Create(g, {.samples_per_axis = 99}).ok());
    EXPECT_TRUE(CellSampler::Create(g, {}).ok());
}

TEST(CellSampler, RecoversCheckerboardUnderAxisAlignedMapping) {
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    // 8 px per cell, no perspective.
    const std::array<Point2, 4> quad{Point2{0, 0}, Point2{192, 0},
                                     Point2{192, 320}, Point2{0, 320}};
    const Homography h = FitGrid(g, quad);
    const Image8 img = Render(cells, h, 192, 320);

    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(img.view(), h);
    ASSERT_EQ(samples.size(), g.cells());

    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            const double v = samples[static_cast<std::size_t>(r) * g.cols + c];
            ASSERT_FALSE(std::isnan(v)) << "cell " << c << "," << r;
            const double expected = ((c + r) % 2 == 0) ? 255.0 : 0.0;
            EXPECT_NEAR(v, expected, 1.0) << "cell " << c << "," << r;
        }
    }
}

TEST(CellSampler, RecoversCheckerboardUnderPerspective) {
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    // A genuinely projective quad: the grid is viewed off-axis.
    const std::array<Point2, 4> quad{Point2{30, 20}, Point2{300, 55},
                                     Point2{280, 380}, Point2{55, 350}};
    const Homography h = FitGrid(g, quad);
    const Image8 img = Render(cells, h, 340, 420);

    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(img.view(), h);

    int wrong = 0;
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            const double v = samples[static_cast<std::size_t>(r) * g.cols + c];
            if (std::isnan(v)) {
                ++wrong;
                continue;
            }
            const bool bright_expected = ((c + r) % 2 == 0);
            if ((v > 127.5) != bright_expected) ++wrong;
        }
    }
    // Under perspective the far edge is compressed to ~4 px/cell; a handful of boundary
    // cells may be marginal, but the bulk must decode.
    EXPECT_LT(wrong, static_cast<int>(g.cells()) / 100) << wrong << " of " << g.cells();
}

TEST(CellSampler, InteriorMarginRejectsNeighbourCrosstalk) {
    // THE point of interior-margin sampling. Under blur, a full-cell sample averages in the
    // neighbours; an interior sample does not. On a checkerboard that difference is the
    // difference between decoding and reading uniform grey.
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    const std::array<Point2, 4> quad{Point2{0, 0}, Point2{192, 0},
                                     Point2{192, 320}, Point2{0, 320}};
    const Homography h = FitGrid(g, quad);
    const Image8 img = Render(cells, h, 192, 320, {.blur_radius = 2});

    const auto contrast = [&](double margin) {
        auto s = CellSampler::Create(g, {.interior_margin = margin, .samples_per_axis = 3});
        EXPECT_TRUE(s.ok());
        const auto v = s.value().Sample(img.view(), h);
        double bright = 0;
        double dark = 0;
        int nb = 0;
        int nd = 0;
        // Interior cells only, so frame-edge effects do not confound the comparison.
        for (std::uint32_t r = 2; r < g.rows - 2; ++r) {
            for (std::uint32_t c = 2; c < g.cols - 2; ++c) {
                const double x = v[static_cast<std::size_t>(r) * g.cols + c];
                if (std::isnan(x)) continue;
                if ((c + r) % 2 == 0) { bright += x; ++nb; } else { dark += x; ++nd; }
            }
        }
        EXPECT_GT(nb, 0);
        EXPECT_GT(nd, 0);
        return bright / nb - dark / nd;
    };

    const double tight = contrast(0.35);
    const double loose = contrast(0.02);
    EXPECT_GT(tight, loose) << "interior margin did not improve contrast: tight=" << tight
                            << " loose=" << loose;
}

TEST(CellSampler, OutOfFrameCellsBecomeErasuresNotGuesses) {
    const GridGeometry g{24, 40};
    const auto cells = Checkerboard(g);
    const std::array<Point2, 4> quad{Point2{0, 0}, Point2{192, 0},
                                     Point2{192, 320}, Point2{0, 320}};
    const Homography h = FitGrid(g, quad);
    // Image covers only the top-left quadrant of the grid.
    const Image8 img = Render(cells, h, 96, 160);

    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(img.view(), h);

    std::size_t nan_count = 0;
    for (const double v : samples) {
        if (std::isnan(v)) ++nan_count;
    }
    EXPECT_GT(nan_count, 0u) << "cells outside the image must be erasures";
    EXPECT_LT(nan_count, samples.size()) << "the visible quadrant must still decode";

    // Specifically: a cell well outside the image must be NaN, never a fabricated value.
    EXPECT_TRUE(std::isnan(samples[static_cast<std::size_t>(g.rows - 1) * g.cols + g.cols - 1]));
}

TEST(CellSampler, EmptyImageYieldsAllErasures) {
    const GridGeometry g{8, 8};
    auto s = CellSampler::Create(g, {});
    ASSERT_TRUE(s.ok());
    const auto samples = s.value().Sample(ImageView8{}, Homography::Identity());
    ASSERT_EQ(samples.size(), g.cells());
    for (const double v : samples) EXPECT_TRUE(std::isnan(v));
}

namespace {

struct TexturedPlane {
    int width;
    int height;
    int stride;
    std::vector<std::uint8_t> bytes;

    TexturedPlane(int w, int h, int padding, std::uint32_t seed)
        : width(w), height(h), stride(w + padding),
          bytes(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0xCD) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto xx = static_cast<std::uint32_t>(x);
                const auto yy = static_cast<std::uint32_t>(y);
                const auto value = seed + xx * 17U + yy * 83U + xx * yy * 13U + (xx ^ (yy * 7U));
                bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
                      static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(value & 255U);
            }
        }
    }

    [[nodiscard]] ImageView8 view() const { return {bytes.data(), width, height, stride}; }
};

void ExpectPairMatches(const CellSampler& sampler, const ImageView8& a, const ImageView8& b,
                       const Homography& h, const RadialDistortion& lens) {
    const std::array expected{sampler.Sample(a, h, lens), sampler.Sample(b, h, lens)};
    const auto actual = sampler.SamplePair(a, b, h, lens);
    for (std::size_t plane = 0; plane < actual.size(); ++plane) {
        ASSERT_EQ(actual[plane].size(), expected[plane].size());
        for (std::size_t cell = 0; cell < actual[plane].size(); ++cell) {
            SCOPED_TRACE(::testing::Message() << "plane " << plane << ", cell " << cell);
            if (std::isnan(expected[plane][cell])) EXPECT_TRUE(std::isnan(actual[plane][cell]));
            else EXPECT_EQ(actual[plane][cell], expected[plane][cell]);  // exact, not a tolerance
        }
    }
}

}  // namespace

TEST(CellSamplerPair, ExactTexturedPlaneResultsAcrossMappingsLensesAndSampleCounts) {
    const GridGeometry grid{12, 16};
    const TexturedPlane a(137, 113, 11, 19), b(137, 113, 23, 211);
    const std::array mappings{
        Homography(std::array<double, 9>{8.25, 0.4, 5.0, 0.15, 5.25, 9.0, 0, 0, 1}),
        Homography(std::array<double, 9>{8.25, 0.4, 5.0, 0.15, 5.25, 9.0, 0.0005, -0.0003, 1}),
        Homography(std::array<double, 9>{9.3, 0.8, 4.0, -0.3, 7.2, 8.0, 0.025, 0.02, 1}),
        Homography(std::array<double, 9>{0, -7, 130, 8, 0, 3, 0, 0, 1}),  // quarter-turn
        Homography(std::array<double, 9>{1, 0, 0, 0, 1, 0, 0.125, 0, -0.8125}),  // horizon
        Homography(std::array<double, 9>{12, 0, -15.8, 0, 10, -20.3, 0, 0, 1}),  // partial edges
    };
    const std::array lenses{
        RadialDistortion{}, RadialDistortion{64.3, 55.1, 1.0 / (137.0 * 137.0 + 113.0 * 113.0), -0.08},
        RadialDistortion{64.3, 55.1, 1.0 / (137.0 * 137.0 + 113.0 * 113.0), 0.04},
    };
    for (int n = 1; n <= 8; ++n) {
        for (const double margin : {0.0, 0.25, 0.49}) {
            const auto sampler = CellSampler::Create(grid, {.interior_margin = margin, .samples_per_axis = n}).value();
            for (std::size_t map = 0; map < mappings.size(); ++map) {
                for (std::size_t lens = 0; lens < lenses.size(); ++lens) {
                    SCOPED_TRACE(::testing::Message() << "samples " << n << ", margin " << margin
                                                    << ", mapping " << map << ", lens " << lens);
                    ExpectPairMatches(sampler, a.view(), b.view(), mappings[map], lenses[lens]);
                }
            }
        }
    }
}

TEST(CellSamplerPair, TinyImagesAndPartialBorderCoverageKeepExactErasures) {
    const GridGeometry grid{8, 8};
    const Homography h(std::array<double, 9>{0.2, 0, -0.4, 0, 0.2, -0.4, 0, 0, 1});
    for (const auto& dimensions : {std::array{1, 1}, std::array{1, 9}, std::array{9, 1}, std::array{2, 2}}) {
        const TexturedPlane a(dimensions[0], dimensions[1], 3, 7), b(dimensions[0], dimensions[1], 9, 131);
        for (int n = 1; n <= 8; ++n) {
            SCOPED_TRACE(::testing::Message() << dimensions[0] << "x" << dimensions[1] << ", samples " << n);
            const auto sampler = CellSampler::Create(grid, {.interior_margin = 0.0, .samples_per_axis = n}).value();
            ExpectPairMatches(sampler, a.view(), b.view(), h, {});
        }
    }
    const TexturedPlane a(2, 2, 3, 7), b(2, 2, 9, 131);
    const auto sampler = CellSampler::Create(grid, {.interior_margin = 0.0, .samples_per_axis = 8}).value();
    const auto actual = sampler.SamplePair(a.view(), b.view(), h);
    std::size_t erased = 0;
    for (const double value : actual[0]) erased += static_cast<std::size_t>(std::isnan(value));
    EXPECT_GT(erased, 0U);
    EXPECT_LT(erased, actual[0].size());
}

TEST(CellSamplerPair, MissingAndUnequalPlanesRetainIndependentCoverage) {
    const GridGeometry grid{12, 16};
    const TexturedPlane a(137, 113, 11, 19), b(71, 59, 23, 211);
    const TexturedPlane shorter(137, 59, 7, 93), narrower(71, 113, 13, 171);
    const Homography h(std::array<double, 9>{8.25, 0.4, 5.0, 0.15, 5.25, 9.0, 0.001, -0.002, 1});
    const std::array pairs{
        std::array{ImageView8{}, a.view()}, std::array{a.view(), ImageView8{}},
        std::array{ImageView8{}, ImageView8{}}, std::array{a.view(), b.view()}, std::array{b.view(), a.view()},
        std::array{a.view(), shorter.view()}, std::array{narrower.view(), a.view()},
    };
    for (int n = 1; n <= 8; ++n) {
        const auto sampler = CellSampler::Create(grid, {.samples_per_axis = n}).value();
        for (std::size_t pair = 0; pair < pairs.size(); ++pair) {
            SCOPED_TRACE(::testing::Message() << "samples " << n << ", pair " << pair);
            ExpectPairMatches(sampler, pairs[pair][0], pairs[pair][1], h,
                              {64.3, 55.1, 1.0 / (137.0 * 137.0 + 113.0 * 113.0), -0.08});
        }
    }
}
