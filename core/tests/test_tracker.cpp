#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/modulation.h>
#include <fileflow/tracker.h>

#include <gtest/gtest.h>

#include <cmath>

#include "render_test_util.h"

using namespace fileflow;
using namespace fileflow::test;

namespace {

constexpr GridGeometry kGrid{48, 80};

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(
        kGrid, {.pilot_pitch = 8, .marker_size = 6, .header_rows = 4, .guard_width = 1,
                .boundary_width = 1});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

CellMatrix RenderFrame(const FrameLayout& layout, std::uint64_t seed) {
    const M0Modulator mod(layout);
    SplitMix64 rng(seed);
    std::vector<std::uint8_t> hdr(mod.header_capacity_bytes());
    std::vector<std::uint8_t> pay(mod.payload_capacity_bytes());
    for (auto& b : hdr) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    for (auto& b : pay) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    CellMatrix m(kGrid.cols, kGrid.rows);
    EXPECT_TRUE(mod.Render(hdr, pay, &m).ok());
    return m;
}

Image8 Scene(const CellMatrix& cells, const std::array<Point2, 4>& quad, int w, int h) {
    const auto corners = GridCorners(kGrid);
    auto hom = Homography::FromCorrespondences(Span4(corners), Span4(quad));
    EXPECT_TRUE(hom.ok());
    return Render(cells, hom.value(), w, h);
}

std::array<Point2, 4> QuadAt(double cx, double cy, double half_w, double half_h) {
    return {Point2{cx - half_w, cy - half_h}, Point2{cx + half_w, cy - half_h},
            Point2{cx + half_w, cy + half_h}, Point2{cx - half_w, cy + half_h}};
}

}  // namespace

TEST(ScreenTracker, RejectsBadConfig) {
    const auto layout = MakeLayout();
    EXPECT_FALSE(ScreenTracker::Create(layout, {.search_margin = 0.0}).ok());
    EXPECT_FALSE(ScreenTracker::Create(layout, {.max_corner_jump = 0.0}).ok());
    EXPECT_FALSE(ScreenTracker::Create(layout, {.max_degraded_frames = -1}).ok());
    EXPECT_TRUE(ScreenTracker::Create(layout, {}).ok());
}

TEST(ScreenTracker, AcquiresThenTracks) {
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    constexpr int w = 700;
    constexpr int h = 1000;
    const auto quad = QuadAt(350, 500, 230, 390);

    for (int i = 0; i < 5; ++i) {
        const Image8 img =
            Scene(RenderFrame(layout, static_cast<std::uint64_t>(100 + i)), quad, w, h);
        const auto r = tr.value().Track(img.view());
        ASSERT_TRUE(r.ok) << "frame " << i;
        if (i == 0) {
            EXPECT_TRUE(r.reacquired) << "first frame must be a full acquisition";
        } else {
            EXPECT_FALSE(r.reacquired) << "frame " << i << " should have tracked";
            EXPECT_EQ(r.state, TrackState::kTracking);
        }
    }
    EXPECT_EQ(tr.value().full_acquisitions(), 1u);
    EXPECT_EQ(tr.value().successful_frames(), 5u);
}

TEST(ScreenTracker, TrackingExaminesFarFewerPixelsThanAcquisition) {
    // THE ADR-0006 MEASUREMENT. The project's founding claim is that a persistent tracked
    // grid beats repeated independent detection. This is that claim reduced to a number:
    // pixels examined per frame, tracked vs reacquired.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    const int w = 900;
    const int h = 1300;
    // Screen occupies a modest part of a large frame -- the realistic case, and the one where
    // full-image search is most wasteful.
    const auto quad = QuadAt(450, 650, 180, 300);

    const Image8 first = Scene(RenderFrame(layout, 1), quad, w, h);
    ASSERT_TRUE(tr.value().Track(first.view()).ok);
    const std::uint64_t acquire_px = tr.value().last_pixels_examined();

    const Image8 second = Scene(RenderFrame(layout, 2), quad, w, h);
    const auto r = tr.value().Track(second.view());
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.reacquired);
    const std::uint64_t track_px = tr.value().last_pixels_examined();

    EXPECT_EQ(acquire_px, static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h));
    EXPECT_LT(track_px, acquire_px / 2)
        << "tracked " << track_px << " px vs acquisition " << acquire_px << " px";

    // Report the ratio: this is the evidence, not the assertion.
    const double ratio = static_cast<double>(track_px) / static_cast<double>(acquire_px);
    std::printf("[ADR-0006] tracked/acquired pixel ratio = %.3f (%llu vs %llu)\n", ratio,
                static_cast<unsigned long long>(track_px),
                static_cast<unsigned long long>(acquire_px));
}

TEST(ScreenTracker, TrackingSavingGrowsAsTheScreenShrinksInFrame) {
    // The single ratio above is GEOMETRY-SPECIFIC and would be misleading quoted alone.
    // The search window is the quad dilated by search_margin, so
    //     ratio ~= (1 + 2*margin)^2 * (quad area / image area)
    // i.e. the saving is driven by how much of the frame the screen occupies, NOT by
    // tracking being clever. When the screen fills the frame, tracking saves almost nothing.
    //
    // That matters for ADR-0006 because the interesting regime -- high-resolution sensor,
    // screen at a normal distance -- is exactly the small-quad end, and it is also the end
    // that gets MORE favourable as sensor resolution rises, since the window is sized by the
    // screen's apparent size rather than by the pixel count.
    const auto layout = MakeLayout();
    constexpr int w = 1200;
    constexpr int h = 1600;

    std::printf("[ADR-0006] quad/frame -> tracked/acquired pixel ratio\n");
    // Starts above 1.0 because a ratio of exactly 1.0 is a legitimate first value: when the
    // dilated window covers the whole image there is no saving at all. Seeding at 1.0 would
    // have made the first comparison fail on a correct result.
    double prev_ratio = 1.01;
    for (const double half_w : {440.0, 300.0, 200.0, 150.0}) {
        const double half_h = half_w * 1.6;
        auto tr = ScreenTracker::Create(layout, {});
        ASSERT_TRUE(tr.ok());
        const auto quad = QuadAt(600, 800, half_w, half_h);

        const Image8 a = Scene(RenderFrame(layout, 1), quad, w, h);
        ASSERT_TRUE(tr.value().Track(a.view()).ok) << "half_w " << half_w;
        const std::uint64_t acq = tr.value().last_pixels_examined();

        const Image8 b = Scene(RenderFrame(layout, 2), quad, w, h);
        const auto r = tr.value().Track(b.view());
        ASSERT_TRUE(r.ok);
        ASSERT_FALSE(r.reacquired) << "half_w " << half_w;
        const std::uint64_t trk = tr.value().last_pixels_examined();

        const double quad_frac = (4.0 * half_w * half_h) / (static_cast<double>(w) * h);
        const double ratio = static_cast<double>(trk) / static_cast<double>(acq);
        std::printf("           %.3f      -> %.3f\n", quad_frac, ratio);

        EXPECT_LT(ratio, prev_ratio) << "saving did not improve as the screen shrank";
        prev_ratio = ratio;
    }
}

TEST(ScreenTracker, TrackingStillSavesWhenTheScreenFillsTheFrame) {
    // REGRESSION for the boundary-annulus optimisation, and the resolution of finding F13.
    //
    // This test previously asserted the OPPOSITE -- that tracking saved nothing at all in this
    // configuration -- and it was correct at the time. The tracker dilated the quad's bounding
    // box, so a screen filling ~64% of the frame produced a window covering the whole image.
    // That mattered because filling the frame is what maximises pixels per cell: the saving
    // disappeared precisely in the configuration we expect to operate in.
    //
    // Scanning only an annulus around the boundary ring fixes it. The corner extremes lie ON
    // the boundary, so the interior scan was pure waste. Measured here: 1.000 -> ~0.55.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    constexpr int w = 1200;
    constexpr int h = 1600;
    const auto quad = QuadAt(600, 800, 440, 704);  // ~64% of the frame

    const Image8 a = Scene(RenderFrame(layout, 1), quad, w, h);
    ASSERT_TRUE(tr.value().Track(a.view()).ok);
    const std::uint64_t acq = tr.value().last_pixels_examined();

    const Image8 b = Scene(RenderFrame(layout, 2), quad, w, h);
    const auto r = tr.value().Track(b.view());
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.reacquired);
    const std::uint64_t trk = tr.value().last_pixels_examined();

    const double ratio = static_cast<double>(trk) / static_cast<double>(acq);
    std::printf("[ADR-0006] screen fills 64%% of frame -> ratio %.3f\n", ratio);
    EXPECT_LT(ratio, 0.70) << "annulus search lost its advantage in the dense-framing regime";
}

TEST(ScreenTracker, AnnulusTracksARotatedScreen) {
    // The annulus is built by scaling the quad about its CENTROID, not by insetting a bounding
    // box. A bbox-based annulus does not follow a rotated quad's boundary, so it would miss
    // the boundary ring exactly when the receiver is held at an angle -- which is the common
    // case, not the exception.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    constexpr int w = 1000;
    constexpr int h = 1300;
    // A visibly rotated quad: corners no longer axis-aligned.
    const std::array<Point2, 4> rot{Point2{360, 200}, Point2{760, 380},
                                    Point2{620, 1080}, Point2{220, 900}};

    const Image8 a = Scene(RenderFrame(layout, 21), rot, w, h);
    ASSERT_TRUE(tr.value().Track(a.view()).ok) << "acquisition failed on a rotated screen";

    const Image8 b = Scene(RenderFrame(layout, 22), rot, w, h);
    const auto r = tr.value().Track(b.view());
    ASSERT_TRUE(r.ok) << "annulus lost a rotated screen";
    EXPECT_FALSE(r.reacquired) << "rotated screen forced a full reacquisition";
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(r.quad[i].x, rot[i].x, 4.0) << "corner " << i;
        EXPECT_NEAR(r.quad[i].y, rot[i].y, 4.0) << "corner " << i;
    }
}

TEST(ScreenTracker, DetectionHasAMinimumApparentScreenSize) {
    // Sweeps apparent screen size and reports where detection stops.
    //
    // ⚠ READ THE RESULT CAREFULLY. This sweep once appeared to show an optical resolution
    // floor near 6 px/cell (finding F14) and that conclusion was RETRACTED twice over: the
    // original number was an artifact of the F15 payload-dependence bug, and the corrected
    // boundary turns out to sit exactly on `min_area_fraction` -- a config threshold about
    // what counts as a plausible screen, not a limit of the optics.
    //
    // So what this measures is the CONFIGURED minimum screen size, and it will move if
    // min_area_fraction moves. A genuine resolution limit cannot be found with the current
    // renderer at all, because it models neither display subpixel structure nor sensor MTF
    // nor moire -- the mechanisms that actually produce a density cliff.
    const auto layout = MakeLayout();
    constexpr int w = 1200;
    constexpr int h = 1600;

    double smallest_ok_px_per_cell = 0.0;
    std::printf("[envelope] px/cell -> detection\n");
    for (const double half_w : {440.0, 300.0, 200.0, 150.0, 130.0, 110.0, 90.0}) {
        const double half_h = half_w * 1.6;
        auto tr = ScreenTracker::Create(layout, {});
        ASSERT_TRUE(tr.ok());
        const Image8 img =
            Scene(RenderFrame(layout, 5), QuadAt(600, 800, half_w, half_h), w, h);
        const bool ok = tr.value().Track(img.view()).ok;
        const double px_per_cell = (2.0 * half_w) / static_cast<double>(kGrid.cols);
        std::printf("           %5.2f    -> %s\n", px_per_cell, ok ? "detected" : "refused");
        if (ok) smallest_ok_px_per_cell = px_per_cell;
    }

    ASSERT_GT(smallest_ok_px_per_cell, 0.0) << "detector failed at every size";
    // Loose bound: the point is the printed table, not a threshold to tune against. This only
    // catches a gross regression in detection sensitivity.
    EXPECT_LT(smallest_ok_px_per_cell, 12.0)
        << "detection now needs " << smallest_ok_px_per_cell
        << " px/cell -- sensitivity regressed badly";
}

TEST(ScreenTracker, FollowsAMovingScreen) {
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    const int w = 800;
    const int h = 1100;
    double cx = 380;
    double cy = 520;

    int reacquisitions = 0;
    for (int i = 0; i < 12; ++i) {
        cx += 9.0;  // steady drift, well inside the search margin
        cy += 5.0;
        const Image8 img = Scene(RenderFrame(layout, static_cast<std::uint64_t>(200 + i)),
                                 QuadAt(cx, cy, 210, 350), w, h);
        const auto r = tr.value().Track(img.view());
        ASSERT_TRUE(r.ok) << "lost the screen at frame " << i;
        if (r.reacquired) ++reacquisitions;
        // The tracked quad must actually follow the screen, not lag on a stale position.
        EXPECT_NEAR((r.quad[0].x + r.quad[1].x) / 2.0, cx, 6.0) << "frame " << i;
    }
    EXPECT_EQ(reacquisitions, 1) << "steady motion should not force reacquisition";
}

TEST(ScreenTracker, ReacquiresAfterATeleport) {
    // A jump larger than the search window must produce a clean reacquisition, NOT a stale
    // or half-refined homography.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    const int w = 900;
    const int h = 1200;
    const Image8 a = Scene(RenderFrame(layout, 1), QuadAt(280, 380, 170, 280), w, h);
    ASSERT_TRUE(tr.value().Track(a.view()).ok);
    EXPECT_EQ(tr.value().full_acquisitions(), 1u);

    // Move the screen far outside the previous search window.
    const Image8 b = Scene(RenderFrame(layout, 2), QuadAt(640, 850, 170, 280), w, h);
    const auto r = tr.value().Track(b.view());
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.reacquired) << "teleport must force a full reacquisition";
    EXPECT_EQ(tr.value().full_acquisitions(), 2u);
    EXPECT_NEAR((r.quad[0].x + r.quad[1].x) / 2.0, 640.0, 6.0);
}

TEST(ScreenTracker, NeverReportsAStaleLockWhenTheScreenVanishes) {
    // The important safety property. When the screen disappears the tracker must report
    // failure, never keep returning the last good homography -- downstream stages cannot
    // tell a stale homography from a fresh one, and would decode noise with full confidence.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    const int w = 800;
    const int h = 1100;
    const Image8 present = Scene(RenderFrame(layout, 7), QuadAt(400, 550, 210, 350), w, h);
    ASSERT_TRUE(tr.value().Track(present.view()).ok);

    const Image8 gone(w, h, 3);  // dark frame: screen switched off / camera turned away
    for (int i = 0; i < 4; ++i) {
        const auto r = tr.value().Track(gone.view());
        EXPECT_FALSE(r.ok) << "reported a lock on an empty frame at iteration " << i;
        EXPECT_EQ(r.state, TrackState::kSearching);
    }
}

TEST(ScreenTracker, RejectsSnappingOntoADifferentBrightObject) {
    // A decoy inside the search window must not capture the lock. The corner-jump plausibility
    // check exists for exactly this.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    const int w = 900;
    const int h = 1200;
    const auto quad = QuadAt(430, 600, 200, 330);
    Image8 img = Scene(RenderFrame(layout, 11), quad, w, h);
    ASSERT_TRUE(tr.value().Track(img.view()).ok);

    // Now blank the screen and put a large bright rectangle nearby instead.
    Image8 decoy(w, h, 2);
    for (int y = 200; y < 520; ++y) {
        for (int x = 150; x < 420; ++x) decoy.set(x, y, 255);
    }
    const auto r = tr.value().Track(decoy.view());
    // Either it refuses outright, or it reacquired -- but it must NOT silently report a
    // tracked lock derived from the decoy.
    if (r.ok) {
        EXPECT_TRUE(r.reacquired) << "accepted a decoy as a tracked refinement";
    }
}

TEST(ScreenTracker, FallbackFrameIsChargedForBothAttempts) {
    // REGRESSION for a self-serving measurement bug. When local refinement fails the tracker
    // falls through to a full acquisition, so that frame genuinely costs annulus + full image.
    // The cost accumulator used to ASSIGN rather than ADD, so the wasted annulus scan vanished
    // from the total -- understating tracking's cost in exactly the case where tracking did
    // badly, and biasing the ADR-0006 comparison in its own favour.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    constexpr int w = 900;
    constexpr int h = 1200;
    const Image8 a = Scene(RenderFrame(layout, 31), QuadAt(280, 380, 170, 280), w, h);
    ASSERT_TRUE(tr.value().Track(a.view()).ok);
    const std::uint64_t acquire_only = tr.value().last_pixels_examined();
    EXPECT_EQ(acquire_only, static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h));

    // Teleport: refinement is attempted in the old window, fails, then reacquires.
    const Image8 b = Scene(RenderFrame(layout, 32), QuadAt(640, 850, 170, 280), w, h);
    const auto r = tr.value().Track(b.view());
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.reacquired);

    EXPECT_GT(tr.value().last_pixels_examined(), acquire_only)
        << "fallback frame was not charged for the failed refinement attempt";
}

TEST(ScreenTracker, RefinedAndSuccessfulCountsAreDistinct) {
    // successful_frames() counts BOTH paths; refined_frames() counts only local refinement.
    // Conflating them (as a single counter previously did) would double-count acquisitions
    // in any tracked-vs-acquired ratio.
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    constexpr int w = 700;
    constexpr int h = 1000;
    const auto quad = QuadAt(350, 500, 230, 390);
    for (int i = 0; i < 5; ++i) {
        const Image8 img =
            Scene(RenderFrame(layout, static_cast<std::uint64_t>(400 + i)), quad, w, h);
        ASSERT_TRUE(tr.value().Track(img.view()).ok) << "frame " << i;
    }

    EXPECT_EQ(tr.value().successful_frames(), 5u);
    EXPECT_EQ(tr.value().refined_frames(), 4u) << "first frame is an acquisition, not a refine";
    EXPECT_EQ(tr.value().full_acquisitions(), 1u);
    // The invariant that makes the numbers safe to divide.
    EXPECT_EQ(tr.value().refined_frames() + tr.value().full_acquisitions(),
              tr.value().successful_frames());
}

TEST(ScreenTracker, RejectsASearchMarginThatWouldInvertTheAnnulus) {
    // The inner annulus boundary is ScaleQuad(quad, 1 - margin). At margin >= 1.0 that
    // collapses to a point or flips through the centroid, and the annulus stops meaning
    // anything. Previously accepted up to 4.0.
    const auto layout = MakeLayout();
    EXPECT_FALSE(ScreenTracker::Create(layout, {.search_margin = 1.0}).ok());
    EXPECT_FALSE(ScreenTracker::Create(layout, {.search_margin = 2.5}).ok());
    EXPECT_TRUE(ScreenTracker::Create(layout, {.search_margin = 0.99}).ok());
}

TEST(ScreenTracker, ResetDropsTheLock) {
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());

    const int w = 700;
    const int h = 1000;
    const auto quad = QuadAt(350, 500, 230, 390);
    const Image8 img = Scene(RenderFrame(layout, 3), quad, w, h);

    ASSERT_TRUE(tr.value().Track(img.view()).ok);
    ASSERT_FALSE(tr.value().Track(img.view()).reacquired);

    tr.value().Reset();
    EXPECT_EQ(tr.value().state(), TrackState::kSearching);
    EXPECT_TRUE(tr.value().Track(img.view()).reacquired) << "Reset did not drop the lock";
}

TEST(ScreenTracker, HandlesAnEmptyImageWithoutLyingOrCrashing) {
    const auto layout = MakeLayout();
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());
    const auto r = tr.value().Track(ImageView8{});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(tr.value().last_pixels_examined(), 0u);
}

TEST(ScreenTracker, RefinesAScreenThatIsROTATEDInTheImage) {
    // Regression for F37, and the reason it escaped every existing test.
    //
    // Refinement rebuilds its quad from raw image extremes -- GEOMETRIC order -- while `last_quad_`
    // comes from the detector in GRID order, after the four-fold ambiguity has been resolved. Those
    // two orderings coincide only when the resolved rotation is zero, which is the case every test
    // here already covered and the case the simulator's upright render produces. On a real capture
    // the phones sat sideways in the sensor frame, the orderings differed by a quarter-turn, and
    // refinement failed 60 times out of 60 while reporting nothing worse than "corner jump".
    //
    // Presenting the screen at each of the four quarter-turns is therefore the whole point: an
    // upright screen cannot detect this bug.
    const int w = 320, h = 320;  // square, so a quarter-turn cannot change what fits in frame
    const auto upright = QuadAt(160, 160, 110, 110);

    for (int turn = 0; turn < 4; ++turn) {
        const FrameLayout layout = MakeLayout();
        auto tr_r = ScreenTracker::Create(layout, TrackerConfig{});
        ASSERT_TRUE(tr_r.ok());
        ScreenTracker tracker = std::move(tr_r).value();

        // Cyclically shifting which IMAGE corner each grid corner maps to presents the same screen
        // rotated by `turn` quarter-turns, which is what the detector then has to resolve.
        std::array<Point2, 4> quad{};
        for (std::size_t i = 0; i < 4; ++i) {
            quad[i] = upright[(i + static_cast<std::size_t>(turn)) % 4];
        }

        // A STATIC scene: identical geometry both frames, only the payload differs. That makes it
        // the easiest possible case for refinement, so a failure is unambiguously the tracker's.
        const Image8 first = Scene(RenderFrame(layout, 11), quad, w, h);
        ASSERT_TRUE(tracker.Track(first.view()).ok) << "turn " << turn << ": acquisition";
        ASSERT_EQ(tracker.full_acquisitions(), 1u);

        const Image8 second = Scene(RenderFrame(layout, 12), quad, w, h);
        const TrackResult t = tracker.Track(second.view());
        ASSERT_TRUE(t.ok) << "turn " << turn << ": second frame";

        EXPECT_EQ(tracker.refined_frames(), 1u)
            << "turn " << turn << ": the second frame of an unmoving screen must REFINE rather than "
               "pay another full acquisition";
        EXPECT_EQ(tracker.full_acquisitions(), 1u)
            << "turn " << turn << ": a refined frame must not also acquire";
        EXPECT_EQ(tracker.refine_rejects_corner_jump(), 0u)
            << "turn " << turn << ": the corners of an unmoving screen cannot have jumped";
    }
}

TEST(ScreenTracker, ResetClearsTheResolvedRotation) {
    // A rotation carried across a session boundary would be applied to a screen that may now be held
    // the other way up, mismatching every corner -- F37 reintroduced by the back door.
    const int w = 320, h = 320;
    const auto upright = QuadAt(160, 160, 110, 110);
    const FrameLayout layout = MakeLayout();
    auto tr_r = ScreenTracker::Create(layout, TrackerConfig{});
    ASSERT_TRUE(tr_r.ok());
    ScreenTracker tracker = std::move(tr_r).value();

    // Lock onto a screen presented at one quarter-turn.
    std::array<Point2, 4> turned{};
    for (std::size_t i = 0; i < 4; ++i) turned[i] = upright[(i + 1) % 4];
    ASSERT_TRUE(tracker.Track(Scene(RenderFrame(layout, 21), turned, w, h).view()).ok);

    // New session, screen now upright. Reacquisition must resolve the rotation afresh.
    tracker.Reset();
    ASSERT_TRUE(tracker.Track(Scene(RenderFrame(layout, 22), upright, w, h).view()).ok);
    const TrackResult t = tracker.Track(Scene(RenderFrame(layout, 23), upright, w, h).view());
    ASSERT_TRUE(t.ok);
    EXPECT_GT(tracker.refined_frames(), 0u)
        << "a stale rotation would make every corner look displaced";
}

TEST(ScreenTracker, SpeedIsPerSecondWhateverTheFrameRate) {
    // The live receiver turns speed() into "hold steadier". It must mean the same thing whether the
    // decoder kept up with every frame or only every third: per second, not per processed frame.
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout, 3);
    constexpr int kW = 700;
    constexpr int kH = 1000;
    constexpr double kPxPerSecond = 120.0;  // a steady pan

    auto measure = [&](double fps) {
        auto tr = ScreenTracker::Create(layout, {});
        EXPECT_TRUE(tr.ok());
        double last = 0.0;
        for (int i = 0; i < 10; ++i) {
            const double t = i / fps;
            const Image8 img = Scene(cells, QuadAt(250 + kPxPerSecond * t, 500, 150, 250), kW, kH);
            const auto r = tr.value().Track(img.view(), static_cast<std::int64_t>(t * 1e9) + 1);
            EXPECT_TRUE(r.ok) << "fps " << fps << " frame " << i;
            last = tr.value().speed();
        }
        return last;
    };

    const double fast = measure(30.0);
    const double slow = measure(10.0);
    const double truth = kPxPerSecond / std::hypot(300.0, 500.0);
    // Each against the TRUE speed, within what corner localisation allows: at 30 fps the screen
    // moves 4 px between frames, and a fifth of a pixel of corner error is 5% of that. The two
    // used to be compared with each other at 5%, which needed both errors to be small at once; on
    // GCC's debug build the 30 fps reading came out 4.9% low and the 10 fps one 0.3% high, and the
    // comparison failed on a measurement that was right. The bug this test exists for -- speed per
    // processed frame rather than per second -- would put the two readings 3x apart.
    EXPECT_NEAR(fast, truth, 0.08 * truth);
    EXPECT_NEAR(slow, truth, 0.08 * truth) << "speed depended on how many frames were processed";
}

TEST(ScreenTracker, ANewLockHasNoSpeed) {
    // A fresh acquisition must not inherit motion from a lock that may be seconds old and elsewhere.
    const auto layout = MakeLayout();
    const auto cells = RenderFrame(layout, 4);
    auto tr = ScreenTracker::Create(layout, {});
    ASSERT_TRUE(tr.ok());
    for (int i = 0; i < 5; ++i) {
        const Image8 img = Scene(cells, QuadAt(250 + 10.0 * i, 500, 150, 250), 700, 1000);
        (void)tr.value().Track(img.view(), (i + 1) * 33'333'333LL);
    }
    EXPECT_GT(tr.value().speed(), 0.0);
    tr.value().Reset();
    const Image8 img = Scene(cells, QuadAt(400, 500, 150, 250), 700, 1000);
    ASSERT_TRUE(tr.value().Track(img.view(), 10 * 33'333'333LL).ok);
    EXPECT_DOUBLE_EQ(tr.value().speed(), 0.0);
}
