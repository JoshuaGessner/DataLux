// Aiming analysis (feature UI-02): tell a user how to point the receiver at the transmitter.
//
// WHY THIS IS A COMPONENT AND NOT A UI DETAIL. Getting the two phones into a workable geometry took
// FIVE hardware iterations, and every failure arrived as the same useless line in a decode log:
// "geometry failures 20 of 20". The distinct causes were a locked screen, focus at infinity, gross
// overexposure, a square sensor mode wasting its pixels, and a rotated screen overflowing the frame
// (F32, F33). A user with no decode log and no tooling has strictly less information than that.
//
// So the receiver has to diagnose its own aim and say what to change. That judgement lives here, in
// portable C++, for the reason ADR-0014 gives: it is real logic with real edge cases, it must be
// tested off-device, and a Kotlin implementation could not be.
//
// WHAT THE ANALYSIS RESTS ON. Screen localisation fits the four lines of the always-bright boundary
// ring and intersects them (F10). Two things follow, and they are the whole basis of the advice:
//
//   1. **Every edge of that ring must be inside the frame.** Miss one and there are not four lines
//      to fit, so detection fails outright no matter how good everything else is. This is the
//      failure that masquerades as "too dense" or "out of focus".
//
//   2. **Rotation is nearly free for the decoder and expensive for framing.** Measured: roll is
//      tolerated to at least 40 degrees with header success 1.0000 and zero detection failures
//      (F32). But the axis-aligned bounding box of a rotated rectangle is much larger than the
//      rectangle -- 2.24x at 36 degrees -- and it is the BOX that has to fit. So the advice for a
//      rotated screen is never "stop rotating"; it is "you need more margin, or align to reclaim
//      it".
//
// Deliberately NOT a detector. It never tries to find corners, fit a homography or read a cell. It
// answers a cruder and more useful question -- is the geometry workable, and if not what should the
// user do -- from a single cheap pass over the luminance plane.
#pragma once

#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/result.h>

#include <cstdint>
#include <array>
#include <cstddef>
#include <string>

namespace fileflow {

// Ordered by what a user should fix FIRST. When several problems coexist, the earliest one in this
// enum is the one reported: telling someone to move closer while the screen is half out of frame
// produces a worse aim, not a better one.
enum class AimVerdict : std::uint8_t {
    kNoScreenFound,     // nothing bright enough to be a screen
    kClipped,           // the screen runs off an edge -- the ring is incomplete
    kTooFar,            // fully visible but too few pixels per cell to resolve
    kTooDark,           // underexposed: no separation to threshold
    kTooBright,         // overexposed: the dark level is gone
    kBlurred,           // resolvable pitch but the cell structure is not sharp
    kReady,             // geometry is workable
};

[[nodiscard]] std::string_view AimVerdictName(AimVerdict v) noexcept;

// Tile grid for the coverage map. Portrait, to match the shape of the screen being looked at.
// Deliberately coarse: it is an aiming aid for a hand-held rig, and a finer grid would report
// noise as structure while costing a histogram per tile.
inline constexpr int kAimTileCols = 6;
inline constexpr int kAimTileRows = 10;
inline constexpr std::size_t kAimTiles = static_cast<std::size_t>(kAimTileCols) * kAimTileRows;

// A tile at or below this separation cannot be demodulated and is shown as a warning. Matches the
// receiver's own erasure floor (PhotometricConfig::min_separation), so the map says the same thing
// the decoder will.
inline constexpr double kTileUsableSeparation = 12.0;

struct AimAdvice {
    AimVerdict verdict = AimVerdict::kNoScreenFound;

    // What was measured, so a caller can render a meter rather than only a sentence, and so a
    // report carries the evidence for its own conclusion.
    double lit_fraction = 0.0;       // share of the frame above the threshold
    int bbox_x = 0, bbox_y = 0, bbox_w = 0, bbox_h = 0;
    bool clipped_left = false, clipped_top = false, clipped_right = false, clipped_bottom = false;
    double mean_luminance = 0.0;
    int threshold = 0;

    // Estimated in-plane rotation, and the screen's true extent with the bounding-box inflation
    // taken out. `px_per_cell` is the corrected figure -- the bounding box overstates the screen
    // whenever it is rotated, so using the box directly would report a density the receiver does
    // not actually have.
    double rotation_deg = 0.0;
    double px_per_cell = 0.0;
    double bbox_inflation = 1.0;     // box area / screen area; 1.0 when square-on

    // Fraction of interior pixels that are neither clearly dark nor clearly bright. High means the
    // cell edges are smeared -- defocus, or a pitch below what the optics resolve.
    double mid_fraction = 0.0;
    /** Share of interior pixels at the bright level. Diagnostic only — payload-dependent (F39). */
    double bright_fraction = 0.0;

    /**
     * Distance between the two luminance levels, in sensor counts.
     *
     * THE focus metric, and the one to sweep against. Mid-fraction is not: it measures how many
     * pixels sit between the levels, which only means "sharp" when two levels actually exist. Where
     * focus is bad enough to collapse them, almost everything lands in one class and mid-fraction
     * goes to zero — so a badly defocused frame scores as the sharpest one. A focus sweep judged on
     * mid-fraction duly picked 40 cm for a subject at 20 cm (F40).
     */
    double level_separation = 0.0;
    // The two class means themselves. The receiver's exposure control steers on these: the bright
    // level away from clipping, the dark level down to black (C05).
    double dark_level = 0.0;
    double bright_level = 0.0;
    // The screen's WHITE: the 97th percentile of its own cells. What exposure steers on now. Under
    // M0 it sits a little above `bright_level`; under M5 the bright class is white, yellow, cyan and
    // green together, its mean far below white, and steering on it raised the Pixel's ISO from 60
    // to 121 for a screen that was already bright enough -- noise for nothing (F62).
    double white_level = 0.0;

    /**
     * Level separation measured per TILE over the screen's bounding box, row-major.
     *
     * A single frame-wide separation number cannot see the failure that is actually blocking the
     * link. On the first above-cliff capture the frame reported a healthy 155-166 globally while a
     * band across the upper third was separating at under 12 -- unreadable -- because the two
     * phones were not parallel and that part of the screen sat outside a very shallow depth of
     * field. Summed over the frame, defocus in one region and a clean frame look the same (F47).
     *
     * A tile map does not need interpreting: the operator tilts the phone until no tile is dark. It
     * is the same measurement that found the problem offline, moved to where it can be acted on
     * while the phones are still in someone's hands.
     *
     * Zero means the tile had no two-level structure at all, which is the reading a defocused or
     * occluded region gives.
     */
    std::array<double, kAimTiles> tile_separation{};

    // One imperative sentence for a user, naming the ACTION rather than the measurement.
    std::string guidance;

    [[nodiscard]] bool ready() const noexcept { return verdict == AimVerdict::kReady; }
    [[nodiscard]] bool clipped() const noexcept {
        return clipped_left || clipped_top || clipped_right || clipped_bottom;
    }
};

// Absolute luminance guards, in 8-bit sensor counts.
//
// These are ABSOLUTE rather than relative because the failures they catch are absolute. A frame can
// be perfectly bimodal and still useless: if the dark level has lifted to 150 the "dark" cells are
// brighter than a correctly exposed bright cell, and the photometric field has no black reference
// left to work from (F7 showed that a spatially varying black-level lift is what actually defeats a
// threshold). Relative measures cannot see that, because the two levels are still separated.
inline constexpr double kMinSeparation = 25.0;    // below this there is no two-level structure
inline constexpr double kMinBrightLevel = 60.0;   // nothing bright enough to be a lit screen
inline constexpr double kMaxDarkLevel = 140.0;    // the dark level has washed out

struct AimConfig {
    // The MEASURED density cliff, not the detector's theoretical floor.
    //
    // 4.0 was the floor below which no detector can work at all: the boundary ring is one cell
    // wide, so at 3 px/cell the ring is 3 px and its own edges are indistinguishable from noise.
    // That number is still true and it was the wrong threshold to aim by. Real hardware puts the
    // cliff far above it (EXP-001, 2026-08-06):
    //
    //     7.30 px/cell -> local separation 142.0, 14 headers decoded
    //     5.93 px/cell -> local separation   4.5,  0 headers decoded
    //
    // Between those two the link stops working entirely, so a floor of 4.0 returned "Ready" for
    // framing that provably decodes nothing -- the guidance was passing setups the decoder cannot
    // use. 7.5 sits just above the highest density known to fail, which is the most this evidence
    // supports.
    //
    // Two captures, one device pair, one distance, no repeats: this is a WORKING FLOOR with one
    // observation behind it, not a characterised limit, and EXP-001's registered threshold (locate
    // the cliff within one sweep step) is still unmet. Revise it when the sweep runs.
    double min_px_per_cell = 7.5;
    // Target for a comfortable margin on all four sides, as a fraction of the frame's short axis.
    double target_margin = 0.06;
    // Exposure guards. A frame with almost everything on one side of the threshold has no second
    // level left to estimate, and the photometric field needs both (F7).
    // Lit fraction is NOT a usable floor: it depends on the payload.
    //
    // A frame carrying a zero fountain symbol is legitimately almost all dark -- only the boundary
    // ring, corner markers and pilots are lit -- and it is a perfectly decodable frame. Judging aim
    // by how much of the screen is bright therefore reports "too dark" for a frame that is fine,
    // which is F15's lesson (localisation must never depend on payload content) reappearing in the
    // aiming analyser. Kept only as a floor against a frame with no lit structure at all.
    double min_bright_fraction = 0.01;
    double max_bright_fraction = 0.98;

    // What actually decides readability, and it is payload-independent: the DISTANCE between the two
    // luminance levels. A frame can be 5% lit or 60% lit and be equally readable, provided the lit
    // and unlit levels are far apart. Overexposure is the case where they collapse together at the
    // top of the range, and that is what this catches.
    double min_level_separation = 40.0;
    double max_mid_fraction = 0.45;
    // Sampling stride for the threshold histogram. The full pass is for the bounding box; the
    // threshold does not need every pixel and paying for it would put this out of reach of a
    // per-frame preview.
    int histogram_stride = 7;

    [[nodiscard]] Status Validate() const noexcept;
};

// Analyse one frame and say what to do about it.
//
// Single pass for the bounding box plus a strided pass for the threshold, no allocation beyond the
// advice string. Cheap enough to run on preview frames, which is the point: guidance that arrives
// after the capture is guidance about a capture that already failed.
[[nodiscard]] Result<AimAdvice> AnalyseAim(const ImageView8& img, const GridGeometry& grid,
                                           AimConfig cfg = {});

}  // namespace fileflow
