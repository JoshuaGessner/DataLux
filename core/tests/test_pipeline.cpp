// Sub-cell alignment (C08a): the global shift (F45) and corner refinement (F51).
#include <fileflow/pipeline.h>
#include <fileflow/session.h>
#include <fileflow/sim/render.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

using namespace fileflow;

namespace {

constexpr GridGeometry kGrid{120, 200};

struct Scene {
    FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    Image8 img;
    Homography truth;

    explicit Scene(std::uint64_t seed = 77, double yaw = 8.0, double pitch = -5.0, int blur = 1,
                   int frame_index = 1) {
        std::vector<std::uint8_t> file(30000);
        SplitMix64 rng(seed);
        for (auto& b : file) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        auto tx = TransmitSession::Create(layout, file, "align.bin", 0xA11);
        CellMatrix frame(kGrid.cols, kGrid.rows);
        // Frame 0 is a manifest frame; later ones carry fountain symbols. Both are realistic.
        for (int i = 0; i <= frame_index; ++i) EXPECT_TRUE(tx.ok() && tx.value().Next(&frame).ok());
        sim::OpticalRenderConfig rc;
        rc.view.image_width = 1000;
        rc.view.image_height = 1600;
        rc.view.yaw_deg = yaw;
        rc.view.pitch_deg = pitch;
        rc.blur_radius = blur;
        rc.seed = seed;
        img = sim::RenderView(frame, kGrid, rc, &truth);
    }
};

// Worst sampling-point error over the grid, in cells of the true geometry.
double WorstErrorCells(const Homography& est, const Homography& truth) {
    double worst = 0.0;
    for (std::uint32_t r = 0; r < kGrid.rows; r += 4) {
        for (std::uint32_t c = 0; c < kGrid.cols; c += 4) {
            const Point2 gp{c + 0.5, r + 0.5};
            const Point2 a = truth.Apply(gp);
            const Point2 b = est.Apply(gp);
            const Point2 a1 = truth.Apply({gp.x + 1.0, gp.y});
            worst = std::max(worst, std::hypot(a.x - b.x, a.y - b.y) /
                                        std::hypot(a1.x - a.x, a1.y - a.y));
        }
    }
    return worst;
}

// The truth with each grid corner moved independently, in cells: the shape of F34's asymmetric
// rounded-corner bias, which no single translation can undo.
Homography PerturbCorners(const Homography& truth, const std::array<Point2, 4>& off) {
    const double c = kGrid.cols;
    const double r = kGrid.rows;
    const std::array<Point2, 4> grid{Point2{0, 0}, Point2{c, 0}, Point2{c, r}, Point2{0, r}};
    std::array<Point2, 4> img{};
    for (std::size_t i = 0; i < 4; ++i) {
        img[i] = truth.Apply({grid[i].x + off[i].x, grid[i].y + off[i].y});
    }
    return Homography::FromCorrespondences(grid, img).value();
}

}  // namespace

// Not an assertion: the trade-off table behind SubcellConfig::align_margin, printed so the choice
// can be re-made from evidence when the layout changes.
TEST(Alignment, AlignWindowSweep) {
    const std::vector<Scene> scenes = [] {
        std::vector<Scene> v;
        v.emplace_back(77, 8.0, -5.0, 1, 1);
        v.emplace_back(5, -12.0, 6.0, 2, 3);
        v.emplace_back(901, 3.0, 10.0, 1, 0);
        v.emplace_back(42, 15.0, 0.0, 0, 2);
        return v;
    }();
    const std::array<std::array<Point2, 4>, 3> perturbations{{
        {Point2{0.4, 0.0}, {0.0, 0.0}, {-0.35, 0.35}, {0.0, -0.4}},
        {Point2{-0.3, 0.3}, {0.4, 0.1}, {0.0, 0.0}, {0.2, -0.3}},
        {Point2{0.0, 0.45}, {-0.45, 0.0}, {0.0, -0.45}, {0.45, 0.0}},
    }};
    struct Arm { double margin; int n; std::size_t anchors; };
    for (const Arm arm : {Arm{0.15, 4, 512}, Arm{0.15, 4, 256}, Arm{0.30, 3, 256}}) {
        const double m = arm.margin;
        const int n = arm.n;
        {
            double worst_fixed = 0.0;
            double worst_drift = 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            for (const Scene& s : scenes) {
                PipelineConfig cfg;
                cfg.subcell.align_margin = m;
                cfg.subcell.align_samples = n;
                cfg.subcell.max_anchors = arm.anchors;
                auto p = FramePipeline::Create(s.layout, cfg);
                ASSERT_TRUE(p.ok());
                for (const auto& off : perturbations) {
                    const Homography h = PerturbCorners(s.truth, off);
                    worst_fixed = std::max(
                        worst_fixed, WorstErrorCells(p.value().Align(s.img.view(), h), s.truth));
                }
                worst_drift = std::max(
                    worst_drift, WorstErrorCells(p.value().Align(s.img.view(), s.truth), s.truth));
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count() /
                              (static_cast<double>(scenes.size()) * 4.0);
            std::printf("   margin %.2f n=%d anchors %3zu: worst perturbed -> %.3f cells, worst "
                        "drift %.3f, %.1f ms/align\n",
                        m, n, arm.anchors, worst_fixed, worst_drift, ms);
        }
    }
}

TEST(Alignment, GlobalShiftRemovesAUniformOffset) {
    const Scene s;
    const Homography off =
        PerturbCorners(s.truth, {Point2{0.3, -0.25}, {0.3, -0.25}, {0.3, -0.25}, {0.3, -0.25}});
    ASSERT_GT(WorstErrorCells(off, s.truth), 0.35);

    PipelineConfig cfg;
    cfg.subcell.refine_corners = false;
    auto p = FramePipeline::Create(s.layout, cfg);
    ASSERT_TRUE(p.ok());
    EXPECT_LT(WorstErrorCells(p.value().Align(s.img.view(), off), s.truth), 0.08);
}

TEST(Alignment, CornerRefinementFixesWhatAShiftCannot) {
    // F51. Two corners right, two wrong in different directions -- the global search can only
    // trade one edge's error for another's.
    const Scene s;
    const Homography off =
        PerturbCorners(s.truth, {Point2{0.4, 0.0}, {0.0, 0.0}, {-0.35, 0.35}, {0.0, -0.4}});
    const double before = WorstErrorCells(off, s.truth);
    ASSERT_GT(before, 0.35);

    PipelineConfig shift_only;
    shift_only.subcell.refine_corners = false;
    auto a = FramePipeline::Create(s.layout, shift_only);
    ASSERT_TRUE(a.ok());
    const double shifted = WorstErrorCells(a.value().Align(s.img.view(), off), s.truth);

    auto b = FramePipeline::Create(s.layout);
    ASSERT_TRUE(b.ok());
    const double refined = WorstErrorCells(b.value().Align(s.img.view(), off), s.truth);

    EXPECT_GT(shifted, 0.25) << "a translation should NOT be able to fix this";
    EXPECT_LT(refined, 0.12) << "corner refinement left the grid off the cells";
    EXPECT_GT(b.value().diagnostics().mean_corner_gain(), 1.0);
}

TEST(Alignment, AnAlreadyCorrectGridIsLeftAlone) {
    // The search must be able to decline: a refinement that drifts a correct grid is worse than
    // none, because it costs exactly the frames that were working.
    const Scene s;
    auto p = FramePipeline::Create(s.layout);
    ASSERT_TRUE(p.ok());
    EXPECT_LT(WorstErrorCells(p.value().Align(s.img.view(), s.truth), s.truth), 0.06);
}
