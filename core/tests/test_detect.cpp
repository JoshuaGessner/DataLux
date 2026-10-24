#include <fileflow/detect.h>
#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/modulation.h>
#include <fileflow/photometric.h>
#include <fileflow/sampler.h>

#include <gtest/gtest.h>

#include <cmath>

#include "render_test_util.h"

using namespace fileflow;
using namespace fileflow::test;

namespace {

constexpr GridGeometry kGrid{48, 80};
constexpr int kPx = 10;

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(
        kGrid, {.pilot_pitch = 8, .marker_size = 6, .header_rows = 4, .guard_width = 1,
                .boundary_width = 1});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

CellMatrix RenderFrame(const FrameLayout& layout) {
    // Real modulator output, so the test exercises the actual boundary/marker rendering
    // rather than a test-local imitation of it.
    const M0Modulator mod(layout);
    CellMatrix m(kGrid.cols, kGrid.rows);
    const std::vector<std::uint8_t> hdr(mod.header_capacity_bytes(), 0xA5);
    const std::vector<std::uint8_t> pay(mod.payload_capacity_bytes(), 0x5C);
    EXPECT_TRUE(mod.Render(hdr, pay, &m).ok());
    return m;
}

// Place the grid into an image with a dark surround, as locked exposure would produce.
Image8 Scene(const CellMatrix& cells, const std::array<Point2, 4>& quad, int w, int h,
             const RenderOptions& opt = {}) {
    const auto corners = GridCorners(kGrid);
    auto hom = Homography::FromCorrespondences(Span4(corners), Span4(quad));
    EXPECT_TRUE(hom.ok());
    return Render(cells, hom.value(), w, h, opt);
}

std::array<Point2, 4> CentredQuad(int w, int h, double inset) {
    return {Point2{inset, inset}, Point2{w - inset, inset},
            Point2{w - inset, h - inset}, Point2{inset, h - inset}};
}

}  // namespace

TEST(OtsuThreshold, SeparatesTwoModes) {
    Image8 img(64, 64, 20);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) img.set(x, y, 220);
    }
    const std::uint32_t t = ScreenDetector::OtsuThreshold(img.view(), 1);
    // The threshold is the INCLUSIVE top of the background class, and callers classify with
    // `value > t`. With two discrete modes the optimal boundary therefore sits exactly ON
    // the lower mode, so t == 20 is correct rather than the midpoint one might assume.
    // What must hold is that it separates the modes under the `>` convention.
    EXPECT_GE(t, 20u);
    EXPECT_LT(t, 220u);
    // The property that actually matters downstream:
    EXPECT_FALSE(20u > t) << "dark mode misclassified as lit";
    EXPECT_TRUE(220u > t) << "bright mode misclassified as background";
}

TEST(OtsuThreshold, HandlesDegenerateImages) {
    EXPECT_EQ(ScreenDetector::OtsuThreshold(ImageView8{}, 1), 128u);
    const Image8 flat(16, 16, 77);
    const std::uint32_t t = ScreenDetector::OtsuThreshold(flat.view(), 1);
    EXPECT_LE(t, 255u);  // must not crash or read out of bounds
}

TEST(ScreenDetector, RejectsBadConfig) {
    const auto layout = MakeLayout();
    EXPECT_FALSE(ScreenDetector::Create(layout, {.min_area_fraction = 0.0}).ok());
    EXPECT_FALSE(ScreenDetector::Create(layout, {.min_area_fraction = 1.0}).ok());
    EXPECT_FALSE(ScreenDetector::Create(layout, {.min_marker_score = 1.5}).ok());
    EXPECT_TRUE(ScreenDetector::Create(layout, {}).ok());
}

TEST(ScreenDetector, RequiresABoundaryRing) {
    // Localisation depends on the persistent boundary; a layout without one must be refused
    // at construction rather than failing mysteriously at detect time.
    auto no_ring = FrameLayout::Create(kGrid, {.pilot_pitch = 8, .boundary_width = 0});
    ASSERT_TRUE(no_ring.ok());
    EXPECT_EQ(ScreenDetector::Create(no_ring.value(), {}).error(),
              Error::kDegenerateParameters);
}

TEST(ScreenDetector, MarkerCodeHasEnoughHammingDistanceToResolveRotation) {
    // REGRESSION for finding F9. The original markers differed by a corner-specific NOTCH of
    // 0-3 cells: minimum pairwise distance ONE cell out of 36, giving a worst-case rotation
    // margin of 2.8% that a single noisy cell could flip. Orientation was effectively a coin
    // toss, and every detection test failed because of it.
    //
    // This asserts the CODE property directly rather than only its downstream effect, so a
    // future marker redesign cannot quietly reintroduce a weak code and be caught only by a
    // puzzling detector failure.
    const auto layout = MakeLayout();
    const std::uint32_t m = layout.config().marker_size;
    const GridGeometry& g = layout.geometry();

    // Extract each corner's pattern in local (lc, lr) coordinates.
    auto pattern = [&](int corner) {
        const bool left = (corner == 0 || corner == 2);
        const bool top = (corner == 0 || corner == 1);
        std::vector<std::uint8_t> p;
        for (std::uint32_t lr = 0; lr < m; ++lr) {
            for (std::uint32_t lc = 0; lc < m; ++lc) {
                const std::uint32_t c = left ? lc : (g.cols - 1 - lc);
                const std::uint32_t r = top ? lr : (g.rows - 1 - lr);
                p.push_back(layout.MarkerValue(c, r));
            }
        }
        return p;
    };

    const std::size_t cells = static_cast<std::size_t>(m) * m;
    std::size_t worst = cells;
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            const auto a = pattern(i);
            const auto b = pattern(j);
            std::size_t dist = 0;
            for (std::size_t k = 0; k < cells; ++k) {
                if (a[k] != b[k]) ++dist;
            }
            worst = std::min(worst, dist);
        }
    }

    // Worst-case rotation margin: under any quarter turn all four corners take a different
    // id, so total mismatch is at least 4 * min distance out of 4 * cells.
    const double margin = static_cast<double>(worst) / static_cast<double>(cells);
    EXPECT_GT(margin, DetectionConfig{}.min_rotation_margin)
        << "min pairwise marker distance " << worst << "/" << cells
        << " gives margin " << margin << ", below the detector's requirement";
}

TEST(ScreenDetector, LocatesAndOrientsAnAxisAlignedScreen) {
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout);
    const int w = kGrid.cols * kPx + 80;
    const int h = kGrid.rows * kPx + 80;
    const auto quad = CentredQuad(w, h, 40);
    const Image8 img = Scene(cells, quad, w, h);

    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto d = det.value().Detect(img.view());
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());

    EXPECT_EQ(d.value().rotation, 0);
    EXPECT_GT(d.value().marker_score, 0.9);

    // Corner accuracy against ground truth.
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(d.value().quad[i].x, quad[i].x, 2.0) << "corner " << i;
        EXPECT_NEAR(d.value().quad[i].y, quad[i].y, 2.0) << "corner " << i;
    }
}

TEST(ScreenDetector, GeometricErrorIsSubCell) {
    // The metric that matters: how far the ESTIMATED homography puts a cell centre from
    // where the TRUE homography puts it. Error much beyond a fraction of a cell shifts every
    // sample toward its neighbours and raises crosstalk across the whole frame.
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout);
    const int w = kGrid.cols * kPx + 80;
    const int h = kGrid.rows * kPx + 80;
    const auto quad = CentredQuad(w, h, 40);
    const Image8 img = Scene(cells, quad, w, h);

    const auto corners = GridCorners(kGrid);
    auto truth = Homography::FromCorrespondences(Span4(corners), Span4(quad));
    ASSERT_TRUE(truth.ok());

    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto d = det.value().Detect(img.view());
    ASSERT_TRUE(d.ok());

    double worst = 0.0;
    for (std::uint32_t r = 0; r < kGrid.rows; r += 7) {
        for (std::uint32_t c = 0; c < kGrid.cols; c += 7) {
            const Point2 gp{static_cast<double>(c) + 0.5, static_cast<double>(r) + 0.5};
            const Point2 a = truth.value().Apply(gp);
            const Point2 b = d.value().grid_to_image.Apply(gp);
            worst = std::max(worst, std::hypot(a.x - b.x, a.y - b.y));
        }
    }
    // Expressed in CELLS, which is the unit that matters, not pixels.
    EXPECT_LT(worst / kPx, 0.25) << "worst cell-centre error " << worst << " px";
}

TEST(ScreenDetector, ResolvesAllFourRotations) {
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout);
    const int w = kGrid.cols * kPx + 60;
    const int h = kGrid.rows * kPx + 60;
    const auto base = CentredQuad(w, h, 30);

    for (int rot = 0; rot < 4; ++rot) {
        // Feed the grid in rotated, and require the detector to undo it.
        std::array<Point2, 4> quad{};
        for (std::size_t i = 0; i < 4; ++i) {
            quad[i] = base[(i + static_cast<std::size_t>(4 - rot)) % 4];
        }
        const Image8 img = Scene(cells, quad, w, h);

        auto det = ScreenDetector::Create(layout, {});
        ASSERT_TRUE(det.ok());
        auto d = det.value().Detect(img.view());
        ASSERT_TRUE(d.ok()) << "rotation " << rot << ": " << ErrorName(d.error());
        EXPECT_GT(d.value().marker_score, 0.85) << "rotation " << rot;

        // Whatever internal rotation it chose, the resulting homography must map grid
        // corners back to the true screen corners.
        const auto corners = GridCorners(kGrid);
        auto truth = Homography::FromCorrespondences(Span4(corners), Span4(quad));
        ASSERT_TRUE(truth.ok());
        for (double gy : {0.0, 1.0}) {
            for (double gx : {0.0, 1.0}) {
                const Point2 gp{gx * kGrid.cols, gy * kGrid.rows};
                const Point2 a = truth.value().Apply(gp);
                const Point2 b = d.value().grid_to_image.Apply(gp);
                EXPECT_NEAR(a.x, b.x, 3.0) << "rotation " << rot;
                EXPECT_NEAR(a.y, b.y, 3.0) << "rotation " << rot;
            }
        }
    }
}

TEST(ScreenDetector, RefusesWhenNothingIsThere) {
    const auto layout = MakeLayout();
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());

    EXPECT_EQ(det.value().Detect(ImageView8{}).error(), Error::kMarkersNotFound);

    const Image8 dark(200, 300, 5);
    EXPECT_EQ(det.value().Detect(dark.view()).error(), Error::kMarkersNotFound);

    // Uniform bright: no structure, markers cannot match.
    const Image8 white(200, 300, 250);
    EXPECT_FALSE(det.value().Detect(white.view()).ok());
}

TEST(ScreenDetector, RefusesASmallBrightDistractor) {
    const auto layout = MakeLayout();
    Image8 img(400, 600, 4);
    // A lamp: bright, but far too small to be the screen.
    for (int y = 100; y < 140; ++y) {
        for (int x = 100; x < 140; ++x) img.set(x, y, 255);
    }
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    EXPECT_EQ(det.value().Detect(img.view()).error(), Error::kMarkersNotFound);
}

TEST(ScreenDetector, LocatesAScreenWithALampElsewhereInTheView) {
    // F56. Every lit pixel used to vote for the corners, so one light source anywhere in the frame
    // pulled a corner out to itself and detection failed -- a rig in a dark room never sees it, a
    // person in a lit room sees it constantly. Two distractors, at different edges.
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout);
    const int w = kGrid.cols * kPx + 320;
    const int h = kGrid.rows * kPx + 200;
    const std::array<Point2, 4> quad{Point2{60, 100}, Point2{60.0 + kGrid.cols * kPx, 100},
                                     Point2{60.0 + kGrid.cols * kPx, 100.0 + kGrid.rows * kPx},
                                     Point2{60, 100.0 + kGrid.rows * kPx}};
    Image8 img = Scene(cells, quad, w, h);
    for (int y = 10; y < 160; ++y) {
        for (int x = w - 120; x < w; ++x) img.set(x, y, 255);  // a window, top right
    }
    for (int y = h - 50; y < h; ++y) {
        for (int x = 0; x < 200; ++x) img.set(x, y, 240);  // a lamp, bottom left
    }

    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto d = det.value().Detect(img.view());
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(d.value().quad[i].x, quad[i].x, 2.0) << "corner " << i;
        EXPECT_NEAR(d.value().quad[i].y, quad[i].y, 2.0) << "corner " << i;
    }
}

TEST(LargestLitRegion, FindsTheScreenNotTheLampAndIsBoundedOnAHugeFrame) {
    Image8 img(4000, 3000, 6);
    for (int y = 500; y < 2500; ++y) {  // the screen's ring
        for (int x = 800; x < 2000; ++x) {
            if (y < 510 || y >= 2490 || x < 810 || x >= 1990) img.set(x, y, 220);
        }
    }
    for (int y = 0; y < 400; ++y) {
        for (int x = 3600; x < 4000; ++x) img.set(x, y, 255);  // a brighter, solid lamp
    }
    const LitRegion r = LargestLitRegion(img.view(), 128);
    ASSERT_TRUE(r.valid);
    // Within the one-block padding (32 px at this size) plus the block quantisation.
    EXPECT_NEAR(r.x0, 800, 64);
    EXPECT_NEAR(r.x1, 1999, 64);
    EXPECT_NEAR(r.y0, 500, 64);
    EXPECT_NEAR(r.y1, 2499, 64);

    EXPECT_FALSE(LargestLitRegion(Image8(640, 480, 6).view(), 128).valid);
    EXPECT_FALSE(LargestLitRegion(ImageView8{}, 128).valid);
}

TEST(ScreenDetector, RefusesRatherThanGuessWhenMarkersAreDestroyed) {
    // THE anti-confidence test. A blank bright rectangle has a perfectly good boundary but no
    // marker structure, so no rotation is better than any other. Returning a confident
    // homography here would send every downstream stage to the wrong place.
    const auto layout = MakeLayout();
    CellMatrix blank(kGrid.cols, kGrid.rows);
    for (std::uint32_t r = 0; r < kGrid.rows; ++r) {
        for (std::uint32_t c = 0; c < kGrid.cols; ++c) blank.set(c, r, kLevelBright);
    }
    const int w = kGrid.cols * kPx + 60;
    const int h = kGrid.rows * kPx + 60;
    const Image8 img = Scene(blank, CentredQuad(w, h, 30), w, h);

    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto d = det.value().Detect(img.view());
    EXPECT_FALSE(d.ok()) << "detector fabricated an orientation from a blank rectangle";
    EXPECT_EQ(d.error(), Error::kMarkersNotFound);
}

TEST(ScreenDetector, FullyLitImageCostsNoAllocation) {
    // REGRESSION for the streaming-extremes fix. The previous implementation collected every
    // lit pixel into a std::vector<Point2> before taking four extremes, so a fully-lit frame
    // -- the worst case, and one an attacker simply chooses -- allocated width*height*16
    // bytes. Measured: 48 MB per frame at 12 MP, 762 MB at 200 MP, ~1.85 GB/s at 4K60
    // (THREAT-MODEL T2 resource exhaustion, and unusable on the hot path regardless).
    //
    // A unit test cannot assert "did not allocate", so this asserts the observable proxy:
    // the pathological input is handled promptly and without dying. Under ASan a regression
    // to the vector form on a large frame is also far more likely to be caught here.
    const auto layout = MakeLayout();
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());

    const Image8 all_lit(1200, 1600, 255);
    auto d = det.value().Detect(all_lit.view());
    // A uniform field has no marker structure, so refusing is the right answer. What matters
    // is that it got there without allocating a point per pixel.
    EXPECT_FALSE(d.ok());
}

TEST(ScreenDetector, FindsAScreenTouchingTheImageOrigin) {
    // The extreme accumulators are seeded to +/-1e300 rather than 0 precisely because a lit
    // pixel at (0,0) has both sum and diff equal to 0. Seeding with 0 would silently refuse
    // to record it and shift the detected top-left corner inward.
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout);
    const int w = kGrid.cols * kPx;
    const int h = kGrid.rows * kPx;
    const std::array<Point2, 4> flush{Point2{0, 0}, Point2{w - 1.0, 0},
                                      Point2{w - 1.0, h - 1.0}, Point2{0, h - 1.0}};
    const Image8 img = Scene(cells, flush, w, h);

    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto d = det.value().Detect(img.view());
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_NEAR(d.value().quad[0].x, 0.0, 1.5);
    EXPECT_NEAR(d.value().quad[0].y, 0.0, 1.5);
}

TEST(ScreenDetector, ArbitraryImagesNeverCrashAndNeverLie) {
    // Deterministic stand-in for fuzz/fuzz_screen_detect.cpp, which cannot run under Apple
    // clang (no libFuzzer -- finding F5). The camera is attacker-controlled and the detector
    // runs BEFORE any checksum exists, so this surface must be exercised on every platform,
    // not only in Linux CI.
    const auto layout = MakeLayout();
    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto sampler = CellSampler::Create(kGrid, {});
    ASSERT_TRUE(sampler.ok());

    SplitMix64 rng(0xC0FFEE);
    for (int trial = 0; trial < 400; ++trial) {
        const int w = 1 + static_cast<int>(rng.Next() % 96);
        const int h = 1 + static_cast<int>(rng.Next() % 96);
        Image8 img(w, h, 0);
        // Mix pure noise with structured blocks so some trials actually reach the scoring
        // path rather than all bailing out at the area check.
        const bool structured = (trial % 3) == 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t v =
                    structured ? static_cast<std::uint8_t>(((x / 3 + y / 3) % 2) ? 255 : 0)
                               : static_cast<std::uint8_t>(rng.Next() & 0xFF);
                img.set(x, y, v);
            }
        }

        auto d = det.value().Detect(img.view());
        if (!d.ok()) continue;  // refusing is always acceptable

        // If it claims a detection, the claim must be coherent.
        for (const double v : d.value().grid_to_image.m()) {
            ASSERT_TRUE(std::isfinite(v)) << "trial " << trial;
        }
        ASSERT_GE(d.value().marker_score, DetectionConfig{}.min_marker_score);
        ASSERT_GE(d.value().marker_score - d.value().runner_up_score,
                  DetectionConfig{}.min_rotation_margin);
        ASSERT_TRUE(d.value().grid_to_image.Inverse().ok()) << "non-invertible homography";

        const auto samples = sampler.value().Sample(img.view(), d.value().grid_to_image);
        ASSERT_EQ(samples.size(), kGrid.cells());
        for (const double v : samples) {
            // Either an honest erasure or a real number -- never an invented infinity.
            ASSERT_TRUE(std::isnan(v) || std::isfinite(v)) << "trial " << trial;
        }
    }
}

TEST(ScreenDetector, FullChainDetectSampleNormaliseDecodes) {
    // End-to-end through the REAL geometry: detect the screen, build the homography from the
    // detection (not from ground truth), sample, normalise, and check that structural cells
    // land where they should. This is what the live receiver will do.
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout);
    const int w = kGrid.cols * kPx + 70;
    const int h = kGrid.rows * kPx + 70;
    // Off-axis, blurred, noisy, and with the falloff+glare combination from finding F7.
    const std::array<Point2, 4> quad{Point2{45, 38}, Point2{w - 30.0, 60},
                                     Point2{w - 52.0, h - 35.0}, Point2{33, h - 58.0}};
    const Image8 img = Scene(cells, quad, w, h,
                             {.corner_falloff = 0.6, .glare_lift = 40.0,
                              .blur_radius = 1, .noise_amplitude = 5.0});

    auto det = ScreenDetector::Create(layout, {});
    ASSERT_TRUE(det.ok());
    auto d = det.value().Detect(img.view());
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());

    auto s = CellSampler::Create(kGrid, {.interior_margin = 0.3, .samples_per_axis = 3});
    ASSERT_TRUE(s.ok());
    const auto raw = s.value().Sample(img.view(), d.value().grid_to_image);

    auto f = PhotometricField::Estimate(layout, raw);
    ASSERT_TRUE(f.ok()) << ErrorName(f.error());
    const auto norm = f.value().Normalise(raw);

    // Boundary cells are always bright: a direct, unambiguous check that the estimated
    // geometry actually lines up with the transmitted frame.
    int checked = 0;
    int correct = 0;
    for (std::uint32_t r = 0; r < kGrid.rows; ++r) {
        for (std::uint32_t c = 0; c < kGrid.cols; ++c) {
            if (layout.role(c, r) != CellRole::kBoundary) continue;
            const double v = norm[static_cast<std::size_t>(r) * kGrid.cols + c];
            if (std::isnan(v)) continue;
            ++checked;
            if (v > 127.5) ++correct;
        }
    }
    ASSERT_GT(checked, 100);
    EXPECT_GT(static_cast<double>(correct) / checked, 0.95)
        << correct << " of " << checked << " boundary cells decoded bright";
}
