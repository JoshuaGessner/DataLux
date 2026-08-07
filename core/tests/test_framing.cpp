// Aiming analysis (UI-02). Every case here is one that actually happened on hardware.
//
// The value of this component is not that it computes a bounding box. It is that it names the RIGHT
// problem when several are true at once, because on real hardware they were: a rotated screen that
// was also clipped, and a clipped screen whose measured density looked fine. Getting the priority
// wrong sends a user the wrong way, which is worse than saying nothing.
#include <fileflow/framing.h>

#include <gtest/gtest.h>

#include <cmath>

using namespace fileflow;

namespace {

constexpr GridGeometry kGrid{120, 260};  // aspect 0.4615, the square-cell S26 grid of F31

// Paint a screen onto a dark frame: a bright rectangle of the grid's aspect, optionally rotated,
// optionally carrying a cell pattern.
//
// Everything is inverse-mapped from destination pixels into the rectangle's own frame, so a rotated
// screen has the SAME area and the SAME cell pattern as an unrotated one. That matters: the analyser
// is supposed to recover the true extent from an inflated bounding box, and a helper that changed the
// screen's size along with its angle would make that untestable.
//
// `pitch <= 0` gives a uniform bright rectangle. `bright_bias` paints that fraction of cells bright
// rather than half, which is how overexposure actually presents -- dark cells washing out rather
// than the picture merely being brighter.
Image8 Screen(int w, int h, double cx, double cy, double long_px, double rot_deg,
              int pitch = 0, std::uint8_t bright = 230, std::uint8_t dark = 12,
              double bright_bias = 0.5) {
    Image8 img(w, h, dark);
    const double aspect = static_cast<double>(kGrid.cols) / static_cast<double>(kGrid.rows);
    const double half_l = long_px / 2.0;
    const double half_s = long_px * aspect / 2.0;
    const double th = rot_deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    // One cell of always-bright boundary ring, as every real frame carries (F10).
    const double ring = pitch > 0 ? static_cast<double>(pitch) : 0.0;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double dx = static_cast<double>(x) - cx;
            const double dy = static_cast<double>(y) - cy;
            const double u = dx * c + dy * s;   // long axis in the screen's own frame
            const double v = -dx * s + dy * c;  // short axis
            if (std::abs(u) > half_l || std::abs(v) > half_s) continue;

            if (pitch <= 0) {
                img.set(x, y, bright);
                continue;
            }
            const bool in_ring = std::abs(u) > half_l - ring || std::abs(v) > half_s - ring;
            if (in_ring) {
                img.set(x, y, bright);
                continue;
            }
            const long cu = static_cast<long>(std::floor((u + half_l) / ring));
            const long cv = static_cast<long>(std::floor((v + half_s) / ring));
            // A deterministic hash rather than parity, so `bright_bias` can be honoured without the
            // pattern degenerating into stripes.
            const unsigned long key = static_cast<unsigned long>(cu * 73856093L ^ cv * 19349663L);
            const double r = static_cast<double>(key % 1000UL) / 1000.0;
            img.set(x, y, r < bright_bias ? bright : dark);
        }
    }
    return img;
}

}  // namespace

TEST(AimAnalysis, TileMapLocalisesADefocusedBandThatTheFrameAverageHides) {
    // The exact failure that blocked the first above-cliff capture, and the exact reason a single
    // frame-wide number was not enough to see it (F47).
    //
    // A band of the screen is flattened to a uniform mid-grey, which is what a region outside a
    // shallow depth of field actually delivers: the two levels blur into one and nothing there can
    // be demodulated. The rest of the screen is untouched and perfectly readable.
    Image8 img = Screen(1600, 1600, 800, 800, 1400, 0.0, 8);
    auto probe = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(probe.ok());
    const int y0 = probe.value().bbox_y;
    const int bh = probe.value().bbox_h;
    // Cover tile rows 4 and 5 exactly, so the assertion is about fully-affected tiles rather than
    // about where a partially-covered tile happens to land.
    const int band_lo = y0 + (bh * 4) / kAimTileRows;
    const int band_hi = y0 + (bh * 6) / kAimTileRows;
    for (int y = band_lo; y < band_hi; ++y) {
        for (int x = 0; x < 1600; ++x) img.set(x, y, 120);
    }

    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    const AimAdvice& a = r.value();

    // THE POINT: the frame-wide figure still looks healthy. Anyone reading only this would conclude
    // the rig was fine and go looking for the fault in the decoder, which is what happened.
    EXPECT_GT(a.level_separation, 100.0)
        << "frame average should still look healthy -- that is why it hid the band";

    for (int tx = 0; tx < kAimTileCols; ++tx) {
        for (const int ty : {4, 5}) {
            const std::size_t i = static_cast<std::size_t>(ty) * kAimTileCols +
                                  static_cast<std::size_t>(tx);
            EXPECT_LE(a.tile_separation[i], kTileUsableSeparation)
                << "tile (" << tx << "," << ty << ") is inside the flattened band and must read as "
                << "unusable, got " << a.tile_separation[i];
        }
        for (const int ty : {0, 1, 8, 9}) {
            const std::size_t i = static_cast<std::size_t>(ty) * kAimTileCols +
                                  static_cast<std::size_t>(tx);
            EXPECT_GT(a.tile_separation[i], 100.0)
                << "tile (" << tx << "," << ty << ") is untouched and must read as healthy, got "
                << a.tile_separation[i];
        }
    }
}

TEST(AimAnalysis, RejectsUnusableConfiguration) {
    Image8 img = Screen(400, 800, 200, 400, 600, 0.0, 6);
    EXPECT_FALSE(AnalyseAim(img.view(), kGrid, {.min_px_per_cell = 0.0}).ok());
    EXPECT_FALSE(AnalyseAim(img.view(), kGrid, {.target_margin = 0.9}).ok());
    EXPECT_FALSE(AnalyseAim(img.view(), kGrid, {.histogram_stride = 0}).ok());
    EXPECT_FALSE(AnalyseAim(ImageView8{}, kGrid).ok());
}

TEST(AimAnalysis, ReportsNoScreenOnADarkFrame) {
    const Image8 img(600, 1200, 5);
    auto a = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().verdict, AimVerdict::kNoScreenFound);
    EXPECT_NE(a.value().guidance.find("Point the camera"), std::string::npos);
}

TEST(AimAnalysis, ClippingIsReportedBeforeDensity) {
    // THE inversion that cost several hardware iterations (F33). A screen running off the edge has a
    // perfectly healthy measured px/cell over the part that IS visible, so an analyser that checks
    // density first tells the user to move CLOSER — the exact opposite of the fix. The measured
    // pitch here is deliberately comfortable, so only the ordering can produce the right answer.
    // Long axis well beyond the frame, but with a pitch that is comfortable over the visible part —
    // so only the ORDERING of the checks can produce the right answer.
    Image8 img = Screen(2000, 2000, 1000, 1000, 2600, 0.0, 8);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    const AimAdvice& a = r.value();

    EXPECT_EQ(a.verdict, AimVerdict::kClipped);
    EXPECT_TRUE(a.clipped());
    EXPECT_GT(a.px_per_cell, 4.0) << "the visible pitch is fine; ordering is what must save us";
    EXPECT_NE(a.guidance.find("Move back"), std::string::npos);
    EXPECT_EQ(a.guidance.find("Move closer"), std::string::npos);
}

TEST(AimAnalysis, NamesWhichEdgesAreClipped) {
    // Pushed off the top only. Naming the edge is the difference between actionable and not.
    Image8 img = Screen(1200, 1200, 600, 100, 900, 0.0, 6);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().clipped_top);
    EXPECT_FALSE(r.value().clipped_bottom);
    EXPECT_NE(r.value().guidance.find("top"), std::string::npos);
}

TEST(AimAnalysis, RecoversRotationAndDoesNotOverstateDensity) {
    // The correction that matters. A 35-degree rotation inflates the bounding box by over 2x in
    // area, so px/cell read straight off the box would claim a density the receiver does not have.
    const double kLong = 700.0;
    Image8 upright = Screen(1600, 1600, 800, 800, kLong, 0.0, 6);
    Image8 tilted = Screen(1600, 1600, 800, 800, kLong, 35.0, 6);

    auto up = AnalyseAim(upright.view(), kGrid);
    auto tl = AnalyseAim(tilted.view(), kGrid);
    ASSERT_TRUE(up.ok());
    ASSERT_TRUE(tl.ok());

    EXPECT_LT(up.value().rotation_deg, 3.0);
    EXPECT_NEAR(tl.value().rotation_deg, 35.0, 4.0);
    EXPECT_GT(tl.value().bbox_inflation, 1.8);

    // The same physical screen, so the same true density, despite very different bounding boxes.
    EXPECT_NEAR(tl.value().px_per_cell, up.value().px_per_cell, 0.6)
        << "rotation must not change the reported px/cell of an unchanged screen";
    // And the naive figure really would have been wrong — though by the LINEAR inflation, not the
    // area one. At 35 degrees the box is 2.24x the screen's area but only ~1.08x its long axis, and
    // px/cell is a linear measure. Conflating the two is easy and was done once here already.
    const double naive = static_cast<double>(std::max(tl.value().bbox_w, tl.value().bbox_h)) /
                         kGrid.rows;
    EXPECT_GT(naive, tl.value().px_per_cell * 1.05);
    EXPECT_LT(naive, tl.value().px_per_cell * 1.15) << "linear inflation, not area";
}

TEST(AimAnalysis, DoesNotTellAUserToStopRotating) {
    // Measured: roll is tolerated to at least 40 degrees with header success 1.0000 and zero
    // detection failures (F32). So rotation on its own is NOT a problem, and advice that treats it
    // as one would send a user chasing an alignment that costs them nothing.
    // Big enough to resolve (1950 px over 260 rows = 7.5 px/cell, the measured floor) and
    // carrying real cells, so the only unusual thing about it is the 30 degrees. At 30 degrees the
    // bounding box is about 2139x1754, which still sits inside the frame with room to spare.
    Image8 img = Screen(2400, 2400, 1200, 1200, 1950, 30.0, 6);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_NEAR(r.value().rotation_deg, 30.0, 5.0);
    EXPECT_NE(r.value().verdict, AimVerdict::kClipped);
    EXPECT_GT(r.value().px_per_cell, 7.5);
    // Fully visible and resolvable while rotated: nothing to complain about.
    EXPECT_EQ(r.value().verdict, AimVerdict::kReady) << r.value().guidance;
}

TEST(AimAnalysis, MentionsAlignmentOnlyWhenRotationIsCostingFrameRoom) {
    // When a rotated screen IS clipped, alignment is genuinely worth suggesting -- it buys margin
    // for free. The suggestion is tied to that situation rather than offered whenever a tilt exists.
    Image8 clipped_tilted = Screen(1000, 1000, 500, 500, 1100, 35.0, 6);
    auto r = AnalyseAim(clipped_tilted.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().verdict, AimVerdict::kClipped);
    EXPECT_NE(r.value().guidance.find("orientation"), std::string::npos);

    Image8 clipped_upright = Screen(1000, 1000, 500, 500, 1100, 0.0, 6);
    auto u = AnalyseAim(clipped_upright.view(), kGrid);
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(u.value().guidance.find("orientation"), std::string::npos)
        << "an upright clipped screen has nothing to gain from rotating";
}

TEST(AimAnalysis, TooFarWhenThePitchIsUnresolvable) {
    // Small, fully visible, and hopeless: 260 rows across ~260 px is one pixel per cell.
    Image8 img = Screen(1200, 1200, 600, 600, 260, 0.0, 6);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().clipped());
    EXPECT_EQ(r.value().verdict, AimVerdict::kTooFar);
    EXPECT_NE(r.value().guidance.find("Move closer"), std::string::npos);
}

TEST(AimAnalysis, DetectsOverexposure) {
    // Overexposure is the two LEVELS converging, not more cells being lit.
    //
    // This test used to model it as "90% of cells bright" with the dark level still at 30 -- which
    // contradicted its own comment ("overexposure destroys the DARK level") and, worse, encoded a
    // payload property as an exposure fault. A frame is legitimately 90% bright whenever the payload
    // says so. The check it was pinning had to be removed for exactly that reason (F39), and this
    // test failed, correctly, as soon as it was.
    //
    // Real overexposure: the dark cells lift toward the bright ones until no threshold separates
    // them. Measured on the first two-device capture as 66% bright against 9% dark with the
    // photometric field unable to place a reference (F33).
    Image8 img = Screen(2400, 2400, 1200, 1200, 1950, 0.0, 6, 250, 205);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().verdict, AimVerdict::kTooBright) << r.value().guidance;
    EXPECT_NE(r.value().guidance.find("brightness"), std::string::npos);
}

TEST(AimAnalysis, AMostlyDarkPayloadIsNotAnAimingFault) {
    // F15's rule, in the aiming analyser: nothing may depend on payload content.
    //
    // A frame carrying a ZERO fountain symbol is almost entirely dark -- only the boundary ring,
    // corner markers and pilots are lit -- and it is perfectly decodable. On real hardware 22 of 64
    // source symbols were zero-padded, producing bursts of frames at mean luminance 8.6 against 48
    // for the rest, and the analyser called every one of them "too dark". The verdict flickered
    // between Ready and TooDark several times a second on a stationary rig, which is unusable
    // guidance and sent the operator looking for a fault that was not there (F39).
    //
    // 8% of cells lit, with both levels healthy and far apart.
    Image8 img = Screen(2400, 2400, 1200, 1200, 1950, 0.0, 6, 230, 12, 0.08);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().verdict, AimVerdict::kTooDark)
        << "a sparse payload is not a dark screen: " << r.value().guidance;
    EXPECT_GT(r.value().px_per_cell, 7.5);
}

TEST(AimAnalysis, AMostlyBrightPayloadIsNotOverexposure) {
    // The same rule from the other side. 92% of cells lit, levels still far apart.
    Image8 img = Screen(2400, 2400, 1200, 1200, 1950, 0.0, 6, 230, 12, 0.92);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().verdict, AimVerdict::kTooBright)
        << "a dense payload is not an overexposed screen: " << r.value().guidance;
}

TEST(AimAnalysis, AcceptsAWellFramedSharpScreen) {
    Image8 img = Screen(2400, 2400, 1200, 1200, 1950, 0.0, 6);
    auto r = AnalyseAim(img.view(), kGrid);
    ASSERT_TRUE(r.ok());
    const AimAdvice& a = r.value();
    EXPECT_EQ(a.verdict, AimVerdict::kReady) << a.guidance;
    EXPECT_FALSE(a.clipped());
    EXPECT_GT(a.px_per_cell, 7.5);
    EXPECT_LT(a.mid_fraction, 0.45);
}

TEST(AimAnalysis, IsStrideSafe) {
    // Fed from a camera Y plane whose stride is chosen by the driver and is very often not the
    // width. Code assuming stride == width works on one device and shears on the next (image.h).
    Image8 padded = Screen(1700, 1600, 750, 800, 1200, 0.0, 6);
    const ImageView8 narrow(padded.data().data(), 1600, 1600, 1700);
    auto r = AnalyseAim(narrow, kGrid);
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().verdict, AimVerdict::kNoScreenFound);
}
