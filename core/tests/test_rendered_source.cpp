// End-to-end through the IMAGE path: render -> detect -> rectify -> sample -> normalise.
//
// The distinction from test_end_to_end.cpp matters. That suite feeds cell samples straight to
// the demodulator, which isolates coding and modulation. This one makes the decoder earn its
// homography from pixels, so geometric error, detection failures and photometric estimation
// error are all in the loop. Those are real terms in the budget and are invisible to the
// cell-sample path.
#include <fileflow/modulation.h>
#include <fileflow/sim/rendered_source.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace fileflow;
using namespace fileflow::sim;

namespace {

constexpr GridGeometry kGrid{48, 80};

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(
        kGrid, {.pilot_pitch = 8, .marker_size = 6, .header_rows = 4, .guard_width = 1,
                .boundary_width = 1});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

// A short sequence of distinct display states, rendered by the real modulator.
std::vector<CellMatrix> MakeFrames(const FrameLayout& layout, int n) {
    const M0Modulator mod(layout);
    std::vector<CellMatrix> frames;
    SplitMix64 rng(4242);
    for (int i = 0; i < n; ++i) {
        std::vector<std::uint8_t> hdr(mod.header_capacity_bytes());
        std::vector<std::uint8_t> pay(mod.payload_capacity_bytes());
        for (auto& b : hdr) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        for (auto& b : pay) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        CellMatrix m(kGrid.cols, kGrid.rows);
        EXPECT_TRUE(mod.Render(hdr, pay, &m).ok());
        frames.push_back(std::move(m));
    }
    return frames;
}

RenderedSourceConfig BaseConfig() {
    RenderedSourceConfig cfg;
    cfg.render.view.image_width = 640;
    cfg.render.view.image_height = 900;
    cfg.render.view.distance = 1.0;
    cfg.sampler = {.interior_margin = 0.3, .samples_per_axis = 3};
    return cfg;
}

// Fraction of cells recovered correctly against the frame's own ground truth.
double CellAccuracy(const CapturedFrame& f, const GridGeometry& g) {
    if (!f.ground_truth.has_value()) return 0.0;
    std::size_t ok = 0;
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < g.cells(); ++i) {
        const double v = f.cell_samples[i];
        if (std::isnan(v)) continue;
        ++total;
        const bool bright = v > 127.5;
        if (bright == (f.ground_truth->flat(i) == kLevelBright)) ++ok;
    }
    return total == 0 ? 0.0 : static_cast<double>(ok) / static_cast<double>(total);
}

}  // namespace

TEST(RenderedSource, RejectsALayoutWithoutABoundaryRing) {
    auto no_ring = FrameLayout::Create(kGrid, {.pilot_pitch = 8, .boundary_width = 0});
    ASSERT_TRUE(no_ring.ok());
    auto src = RenderedSource::Create(no_ring.value(), {}, BaseConfig());
    EXPECT_FALSE(src.ok());
}

TEST(RenderedSource, HeadOnViewDecodesNearlyEveryCell) {
    const auto layout = MakeLayout();
    auto src = RenderedSource::Create(layout, MakeFrames(layout, 4), BaseConfig());
    ASSERT_TRUE(src.ok()) << ErrorName(src.error());

    int frames = 0;
    while (auto f = src.value().Next()) {
        ++frames;
        EXPECT_GT(CellAccuracy(*f, kGrid), 0.99) << "frame " << f->index;
    }
    EXPECT_EQ(frames, 4);
    EXPECT_EQ(src.value().detection_failures(), 0u);
    // Geometric error is the metric that gates everything downstream.
    EXPECT_LT(src.value().worst_geometric_error_cells(), 0.25)
        << src.value().worst_geometric_error_cells() << " cells";
}

TEST(RenderedSource, SurvivesAnObliqueView) {
    // 20 degrees of yaw plus 12 of pitch: perspective compresses the far edge, which is the
    // regime [VISUALMIMO-CISS11] identifies as the limit on multiplexing gain.
    const auto layout = MakeLayout();
    auto cfg = BaseConfig();
    cfg.render.view.yaw_deg = 20.0;
    cfg.render.view.pitch_deg = 12.0;

    auto src = RenderedSource::Create(layout, MakeFrames(layout, 3), cfg);
    ASSERT_TRUE(src.ok());

    double worst_acc = 1.0;
    while (auto f = src.value().Next()) {
        worst_acc = std::min(worst_acc, CellAccuracy(*f, kGrid));
    }
    EXPECT_EQ(src.value().detection_failures(), 0u);
    EXPECT_GT(worst_acc, 0.97) << "oblique view accuracy " << worst_acc;
}

TEST(RenderedSource, SurvivesHandheldJitter) {
    // Every frame gets a different homography. A decoder that silently cached geometry, or
    // that only worked for one alignment, fails here and not in the static case.
    const auto layout = MakeLayout();
    auto cfg = BaseConfig();
    cfg.jitter_deg = 4.0;
    cfg.jitter_distance = 0.05;

    auto src = RenderedSource::Create(layout, MakeFrames(layout, 6), cfg);
    ASSERT_TRUE(src.ok());

    int decoded = 0;
    double worst_acc = 1.0;
    while (auto f = src.value().Next()) {
        if (f->ground_truth.has_value() &&
            std::any_of(f->cell_samples.begin(), f->cell_samples.end(),
                        [](double v) { return !std::isnan(v); })) {
            ++decoded;
            worst_acc = std::min(worst_acc, CellAccuracy(*f, kGrid));
        }
    }
    EXPECT_GE(decoded, 5) << "jitter cost too many frames";
    EXPECT_GT(worst_acc, 0.95);
}

TEST(RenderedSource, SurvivesTheFalloffPlusGlareCombination) {
    // Finding F7's impairment pair, now through the full image path rather than injected
    // cell samples.
    const auto layout = MakeLayout();
    auto cfg = BaseConfig();
    cfg.render.corner_falloff = 0.45;
    cfg.render.glare_lift = 90.0;
    cfg.render.blur_radius = 1;
    cfg.render.noise_amplitude = 6.0;

    auto src = RenderedSource::Create(layout, MakeFrames(layout, 3), cfg);
    ASSERT_TRUE(src.ok());

    double worst_acc = 1.0;
    while (auto f = src.value().Next()) {
        worst_acc = std::min(worst_acc, CellAccuracy(*f, kGrid));
    }
    EXPECT_EQ(src.value().detection_failures(), 0u);
    EXPECT_GT(worst_acc, 0.95) << "falloff+glare accuracy " << worst_acc;
}

TEST(RenderedSource, TrackingBeatsPerFrameAcquisitionEndToEnd) {
    // THE ADR-0006 A/B, run through the complete chain rather than against the tracker in
    // isolation. Same frames, same geometry, same impairments; the only difference is whether
    // the geometry stage may reuse the previous frame's lock.
    //
    // Both arms must also decode EQUALLY WELL. A cheaper geometry stage that quietly loses
    // frames or degrades accuracy is not an improvement, and reporting the pixel saving
    // without that check would be exactly the kind of partial metric this project bans.
    const auto layout = MakeLayout();
    auto cfg = BaseConfig();
    cfg.render.view.yaw_deg = 8.0;
    cfg.jitter_deg = 1.5;  // handheld: the lock must survive real frame-to-frame movement

    const auto run = [&](bool disable_tracking) {
        auto c = cfg;
        c.disable_tracking = disable_tracking;
        auto src = RenderedSource::Create(layout, MakeFrames(layout, 8), c);
        EXPECT_TRUE(src.ok());
        double worst_acc = 1.0;
        while (auto f = src.value().Next()) {
            if (f->ground_truth.has_value()) {
                worst_acc = std::min(worst_acc, CellAccuracy(*f, kGrid));
            }
        }
        struct R {
            std::uint64_t px;
            std::uint64_t emitted;
            std::uint64_t acquisitions;
            double worst_acc;
        };
        return R{src.value().total_pixels_examined(), src.value().frames_emitted(),
                 src.value().full_acquisitions(), worst_acc};
    };

    const auto baseline = run(true);   // full acquisition every frame
    const auto tracked = run(false);   // tracker allowed to hold a lock

    std::printf("[ADR-0006 e2e] per-frame acquisition: %llu px, %llu acquisitions\n"
                "               tracked:              %llu px, %llu acquisitions  (%.3f)\n",
                static_cast<unsigned long long>(baseline.px),
                static_cast<unsigned long long>(baseline.acquisitions),
                static_cast<unsigned long long>(tracked.px),
                static_cast<unsigned long long>(tracked.acquisitions),
                static_cast<double>(tracked.px) / static_cast<double>(baseline.px));

    EXPECT_EQ(baseline.acquisitions, 8u) << "baseline should reacquire every frame";
    EXPECT_EQ(tracked.acquisitions, 1u) << "tracker should acquire once and then hold";

    // The bound is derived, not a round number. The annulus ratio tracks the screen's frame
    // fraction (~0.54 in this configuration), and frame 1 is still a full acquisition, so the
    // best achievable total here is (1 + 7*0.54)/8 ~= 0.60. An earlier "< 0.5" assertion was
    // simply unreachable at this screen size and would have been a threshold chosen to look
    // good rather than one the geometry permits.
    const double ratio = static_cast<double>(tracked.px) / static_cast<double>(baseline.px);
    EXPECT_LT(ratio, 0.75) << "tracking did not meaningfully reduce work: " << ratio;

    // Steady-state is the number that matters for a long transfer, where the one-off
    // acquisition amortises to nothing.
    const double per_frame_baseline = static_cast<double>(baseline.px) / 8.0;
    const double steady = (static_cast<double>(tracked.px) - per_frame_baseline) / 7.0;
    std::printf("               steady-state per tracked frame: %.3f of an acquisition\n",
                steady / per_frame_baseline);

    // The saving must not have been bought with lost frames or worse decoding.
    EXPECT_EQ(tracked.emitted, baseline.emitted) << "tracking lost frames";
    EXPECT_GE(tracked.worst_acc, baseline.worst_acc - 0.01) << "tracking degraded accuracy";
}

TEST(RenderedSource, ReportsDetectionFailureRatherThanBadData) {
    // Screen mostly out of frame. The correct outcome is a REFUSAL counted as a drop, not a
    // frame of confidently wrong cells: a wrong homography poisons every downstream stage.
    const auto layout = MakeLayout();
    auto cfg = BaseConfig();
    cfg.render.view.offset_x = 0.85;  // push the screen off the edge

    auto src = RenderedSource::Create(layout, MakeFrames(layout, 3), cfg);
    ASSERT_TRUE(src.ok());

    int frames = 0;
    while (auto f = src.value().Next()) {
        ++frames;
        // Whatever happens, it must never claim ground truth it did not decode.
        if (src.value().detection_failures() > 0) {
            EXPECT_TRUE(std::all_of(f->cell_samples.begin(), f->cell_samples.end(),
                                    [](double v) { return std::isnan(v); }) ||
                        f->ground_truth.has_value());
        }
    }
    EXPECT_EQ(frames, 3);
    EXPECT_EQ(src.value().detection_failures() + src.value().frames_emitted(), 3u);
    // Drops must be accounted, never silent.
    EXPECT_EQ(src.value().frames_dropped(), src.value().detection_failures() +
                                                src.value().photometric_failures());
}

TEST(RenderedSource, DistanceEventuallyDefeatsDetection) {
    // Sanity on the geometry model: far enough away and the screen stops being findable.
    // This is a MODEL check, not a claim about real range -- the simulator is uncalibrated.
    const auto layout = MakeLayout();
    auto cfg = BaseConfig();
    cfg.render.view.distance = 12.0;

    auto src = RenderedSource::Create(layout, MakeFrames(layout, 2), cfg);
    ASSERT_TRUE(src.ok());
    while (auto f = src.value().Next()) {
        (void)f;
    }
    EXPECT_GT(src.value().detection_failures(), 0u)
        << "screen still detected at 12x reference distance -- check the projection model";
}
