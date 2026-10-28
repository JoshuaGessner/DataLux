// The receiver's image-to-symbols pipeline (components C06 + C08 + C09 in one place).
//
// WHY THIS EXISTS AS ITS OWN TYPE: three different sources feed the decoder -- the Android
// camera, the channel simulator, and the replay harness -- and all three must run the SAME
// geometry and photometry. If each assembled its own tracker/sampler/field, they would drift,
// and the moment they drift a replayed capture stops proving anything about live behaviour.
// That substitutability is the entire premise of ADR-0010, so the shared stage is a shared
// object, not a shared convention.
//
// Everything here is deterministic: identical pixels in, identical samples out. The replay
// harness depends on that for regression testing, and the simulator depends on it for the
// determinism gate in CI.
#pragma once

#include <fileflow/capture_source.h>
#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/photometric.h>
#include <fileflow/result.h>
#include <fileflow/sampler.h>
#include <fileflow/tracker.h>

#include <cstdint>

namespace fileflow {

// Sub-cell alignment of the sampling grid (C08a).
//
// The homography from four corner markers can be right to within a pixel at the corners and still
// put the sampling points off-centre in the middle of the grid, because the error is not uniform
// across a projective map. At 8.85 px/cell a half-cell slip is 4.4 px, which is enough to make
// every sample a blend of a cell and its neighbour.
//
// That is the failure measured on the first above-cliff capture, and its signature is specific:
// GLOBAL Otsu separation of 155-166 counts on the same frames where the pilot lattice reported a
// LOCAL separation of 40.9. The image is sharp and well exposed; the grid reading it is not
// aligned to it. An aggregate erasure rate cannot distinguish that from a genuinely bad channel,
// which is why it went unnoticed through several rounds of photometric work.
struct SubcellConfig {
    bool enabled = true;
    // Searched each way, in cells. Deliberately under half a cell: at +/-0.5 the search cannot
    // lock onto a NEIGHBOURING cell, which would be a different (and wrong) alignment that can
    // still score well when the payload happens to be complementary.
    double range = 0.45;
    // Candidates per axis per pass. Odd, so the current estimate is always itself a candidate and
    // the search can decline to move.
    int steps = 7;
    // Coarse-to-fine. Two passes over +/-0.45 at 7 steps resolves to about 0.02 cells, which at
    // 8.85 px/cell is a fifth of a pixel -- far finer than the blur.
    int passes = 2;

    // After the global shift, move each of the four grid corners INDEPENDENTLY (F51).
    //
    // A single translation is the right correction only when the error is the same everywhere,
    // and the two errors measured on real optics are not: F34's rounded corners bias the quad's
    // extremes inward by an asymmetric 20-77 px, and a slightly wrong perspective is exactly
    // right at two corners and wrong at the others. Either leaves the middle of the grid on the
    // cells and an edge off them, so the global search finds a compromise that is wrong
    // everywhere. Eight degrees of freedom -- a full homography -- can fix both.
    //
    // Same objective as the shift (pilot contrast), so it inherits F15's rule: no payload
    // knowledge. Coordinate descent over the 8 corner offsets, coarse to fine, each bounded to
    // `corner_range` cells so it cannot walk onto a neighbouring cell, and capped at
    // `corner_evaluations` contrast evaluations per frame so its cost is bounded too.
    bool refine_corners = true;
    double corner_range = 0.6;
    int corner_evaluations = 240;

    // The alignment objective samples each pilot through a WIDER window than decoding does.
    //
    // The decode sampler deliberately reads only the middle of a cell (margin 0.3), which makes
    // it blind to any misalignment smaller than that margin: every sub-sample is still inside the
    // right cell, so pilot contrast is flat and the search has no gradient to follow. Measured:
    // corner refinement scored that way stalled at 0.22 cells of error. Reading nearly the whole
    // cell makes any offset pull neighbours into the average at once, which is the slope the
    // search needs -- and once aligned, the narrow decode window has margin to spare.
    //
    // Chosen from Alignment.AlignWindowSweep (4 scenes x 3 corner perturbations, worst case):
    //   margin 0.15, n=4, 512 anchors: 0.110 cells left, 0.095 drift on a correct grid, 4.9 ms
    //   margin 0.15, n=4, 256 anchors: 0.135, 0.122, 2.5 ms
    //   margin 0.30, n=3 (the decode window): 0.324 -- the plateau described above
    // [HYP]: simulated scenes. Both remaining errors are inside the decode window's 0.3-cell margin.
    double align_margin = 0.15;
    int align_samples = 4;

    // Cells the alignment objective reads. ALL structural cells are known by construction --
    // markers, boundary ring, timing tracks and guards as well as pilots -- and they matter more
    // than their number suggests: the ~80 pilots are interior, while corner refinement moves the
    // CORNERS, which only the edge structure constrains. Scored on pilots alone it left 0.34-0.55
    // cells of error in the worst scene. Every pilot is kept; the rest are thinned evenly to this
    // cap, which bounds the per-frame cost.
    std::size_t max_anchors = 512;

    // Warm start, for live frames (F71). A hand-held phone's frames sit within a hair of each
    // other, and so does the correction the tracker's estimate needs, so each frame starts from
    // the last one's and searches only near it: a 5x5 shift over +/-warm_range, then corners from
    // warm_corner_step down, at most warm_corner_evaluations. Measured on a Pixel 8, the full
    // search was ~19 ms of a ~58 ms frame. A full search still runs when the tracker had to
    // reacquire, when the carried correction scores worse than none, and every cold_every frames,
    // so an error cannot outlive a few frames.
    bool warm_start = true;
    int cold_every = 8;

    // The lens's barrel distortion, learned (F72). A homography maps a flat screen through an
    // IDEAL lens; a phone's wide lens bows straight lines, and once the screen fills the picture
    // the bow reaches a cell or more between the corners a homography pins. A Pixel 8 held close
    // to the sender read every header and decoded nothing -- 78% of cells wrong, banded across the
    // frame -- and the same frame decoded with 1.3% wrong once the bow was modelled. The camera's
    // own correction (DISTORTION_CORRECTION_MODE) reported on and was not applied to the stream.
    //
    // So every lens is measured, not trusted: on each full alignment the strength k1 is tried a
    // step either way, with the grid's corners held where they are seen, and kept where the pilots
    // separate better. The score peaks within ~0.005 of the strength that decodes best.
    bool fit_lens = true;
    // Phone main cameras bow outward (barrel, negative); a large positive strength is never right.
    double lens_k1_min = -0.12;
    double lens_k1_max = 0.05;
    double lens_step = 0.02;
    double lens_min_step = 0.0025;
    double lens_min_gain = 0.002;  // a move must raise pilot contrast by this fraction
    int lens_corner_evaluations = 48;  // corner refinement each candidate strength gets
    // Learned only while the grid's corners reach this far out, as a fraction of the half-diagonal.
    double lens_min_reach = 0.55;
    // This many frames in a row without a payload and the lane goes back to the last strength any
    // lane decoded with (LensFeedback).
    int lens_revert_after = 12;
    // Where learning starts: the strength this camera was last read with, when the app knows it.
    // A lens does not change between transfers, so a close-up can read from its first frames.
    double lens_k1_initial = 0.0;
    double warm_range = 0.1;
    int warm_steps = 5;
    double warm_corner_step = 0.075;
    int warm_corner_evaluations = 96;
};

struct PipelineConfig {
    TrackerConfig tracker;
    SamplerConfig sampler{.interior_margin = 0.3, .samples_per_axis = 3};
    PhotometricConfig photometric;
    SubcellConfig subcell{};

    // Force full-image acquisition every frame, disabling the tracker's fast path.
    // The A/B switch for ADR-0006.
    bool disable_tracking = false;
};

// Per-stage failure accounting. Kept separate rather than lumped into one "failures" counter
// because the stages fail for entirely different reasons and fixing the wrong one is easy:
// geometry failures mean the screen was not found, photometric failures mean it was found but
// could not be read.
struct PipelineDiagnostics {
    std::uint64_t frames_in = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t geometry_failures = 0;
    std::uint64_t photometric_failures = 0;
    std::uint64_t total_pixels_examined = 0;

    [[nodiscard]] std::uint64_t failures() const noexcept {
        return geometry_failures + photometric_failures;
    }

    // Photometric telemetry, summed over frames that reached the photometry stage.
    //
    // Kept here rather than left to the caller because a frame that localises and then erases
    // almost every cell is the hardest failure to attribute, and the numbers that attribute it are
    // discarded the moment the field goes out of scope. F21 is the same lesson from the FEC layer:
    // telemetry that exists only on the success path is missing where it matters.
    std::uint64_t photometric_frames = 0;
    double sum_bright_pilots = 0.0;
    double sum_dark_pilots = 0.0;
    double sum_separation = 0.0;
    double sum_residual = 0.0;
    double sum_bright_nonuniformity = 0.0;
    // Node-level failure counts, so a frame that erases half its cells while looking healthy on
    // average is attributable rather than mysterious.
    std::uint64_t lattice_nodes = 0;
    std::uint64_t nodes_low_separation = 0;
    std::uint64_t nodes_high_residual = 0;

    // Sub-cell alignment telemetry. The GAIN is the interesting one: it is the factor by which
    // pilot contrast improved when the grid was nudged, so a value near 1.0 says the homography
    // was already on the cells and a large value says it was not -- which distinguishes "the
    // channel is bad" from "we were reading the wrong pixels".
    std::uint64_t subcell_frames = 0;
    double sum_subcell_shift = 0.0;   // magnitude, in cells
    double sum_subcell_gain = 0.0;    // aligned contrast / unaligned contrast
    // Corner refinement (F51): frames where moving a corner helped, and the further contrast
    // factor it bought on top of the global shift. Near 1.0 everywhere says the error really was
    // a translation; well above 1.0 says it was not, and the shift alone was leaving cells behind.
    std::uint64_t corner_refined_frames = 0;
    double sum_corner_gain = 0.0;

    [[nodiscard]] double mean_corner_gain() const noexcept {
        return subcell_frames ? sum_corner_gain / static_cast<double>(subcell_frames) : 0.0;
    }

    [[nodiscard]] double mean_subcell_shift() const noexcept {
        return subcell_frames ? sum_subcell_shift / static_cast<double>(subcell_frames) : 0.0;
    }
    [[nodiscard]] double mean_subcell_gain() const noexcept {
        return subcell_frames ? sum_subcell_gain / static_cast<double>(subcell_frames) : 0.0;
    }

    [[nodiscard]] double mean_bright_pilots() const noexcept {
        return photometric_frames ? sum_bright_pilots / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_dark_pilots() const noexcept {
        return photometric_frames ? sum_dark_pilots / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_separation() const noexcept {
        return photometric_frames ? sum_separation / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double mean_residual() const noexcept {
        return photometric_frames ? sum_residual / static_cast<double>(photometric_frames) : 0.0;
    }
    [[nodiscard]] double fraction_low_separation() const noexcept {
        return lattice_nodes ? static_cast<double>(nodes_low_separation) /
                                   static_cast<double>(lattice_nodes)
                             : 0.0;
    }
    [[nodiscard]] double fraction_high_residual() const noexcept {
        return lattice_nodes ? static_cast<double>(nodes_high_residual) /
                                   static_cast<double>(lattice_nodes)
                             : 0.0;
    }
    [[nodiscard]] double mean_bright_nonuniformity() const noexcept {
        return photometric_frames
                   ? sum_bright_nonuniformity / static_cast<double>(photometric_frames)
                   : 0.0;
    }
};

// Chroma defaults to centred YUV 4:2:0, as Android and JPEG site it. RGB capture sources can
// preserve their existing colour detail with full-size, co-sited U/V planes instead.
struct ChromaPlanes {
    ImageView8 u;
    ImageView8 v;
    bool full_resolution = false;
};

class FramePipeline {
  public:
    static Result<FramePipeline> Create(const FrameLayout& layout, PipelineConfig cfg = {});

    // Turn one image into per-cell samples.
    //
    // A frame that fails any stage still comes back as a CapturedFrame -- with all-NaN samples
    // and no ground truth -- rather than as an error. Losing a frame is normal operation on
    // this channel, and the fountain layer is built for it; the caller needs the index gap
    // recorded, not an exception. The reason is in diagnostics().
    //
    // With `chroma`, also the colour of every cell (CapturedFrame::cell_rgb), for M5.
    [[nodiscard]] CapturedFrame Process(const ImageView8& img, std::uint64_t index,
                                        std::int64_t timestamp_ns,
                                        const ChromaPlanes* chroma = nullptr);

    // Geometry from the most recent Process() call. Only meaningful when the last frame
    // decoded; check last_ok() first.
    [[nodiscard]] const Homography& last_homography() const noexcept { return last_h_; }
    // The lens distortion this pipeline has learned (SubcellConfig::fit_lens); 0 = none.
    [[nodiscard]] double lens_strength() const noexcept { return lens_k1_; }

    // How the frame this pipeline just processed fared downstream, so the lens is learned only
    // from frames that read (F72): steering on frames that did not -- motion, refocusing, a clipped
    // screen -- is steering on noise, and on a Pixel 8 it walked the strength from -0.03 to +0.10
    // and no header read again. `fallback` is the strength the receiver last decoded a payload
    // with; after SubcellConfig::lens_revert_after frames without one, the lane returns to it.
    // Until anything has decoded (`have_fallback` false) learning runs on every frame: a strongly
    // bowed close-up reads no header until the bow is corrected, and there is nothing to protect.
    void LensFeedback(bool header_read, bool payload_ok, double fallback, bool have_fallback) noexcept;
    [[nodiscard]] bool last_ok() const noexcept { return last_ok_; }
    [[nodiscard]] double last_marker_score() const noexcept { return last_score_; }

    [[nodiscard]] const PipelineDiagnostics& diagnostics() const noexcept { return diag_; }
    [[nodiscard]] const ScreenTracker& tracker() const noexcept { return tracker_; }
    [[nodiscard]] const FrameLayout& layout() const noexcept { return *layout_; }

    // Drop the geometry lock, e.g. across a session boundary.
    void Reset() noexcept { tracker_.Reset(); }

    // Sub-cell alignment of a grid-to-image estimate against this image's pilots: the global
    // shift, then corner refinement, as configured. Process() runs this on the tracker's output;
    // it is public so the alignment can be tested against a known-wrong homography directly,
    // without depending on what errors the tracker happens to make.
    [[nodiscard]] Homography Align(const ImageView8& img, const Homography& h);

    // The last frame's photometric field, when one was estimated.
    //
    // Retained because every aggregate so far has been unable to tell three very different
    // failures apart: a grid drifting against the cells, glare over part of the screen, and moire
    // between panel and sensor all produce "high residual, high erasure". They do not look alike
    // in SPACE. Summing over the frame is what made them indistinguishable.
    [[nodiscard]] const PhotometricField* last_field() const noexcept {
        return last_field_ ? &*last_field_ : nullptr;
    }

  private:
    // Mean bright-pilot level minus mean dark-pilot level, with the grid shifted by (dx, dy)
    // cells. The objective the sub-cell search maximises.
    [[nodiscard]] double PilotContrast(const ImageView8& img, const Homography& h, double dx,
                                       double dy) const;
    [[nodiscard]] double PilotContrast(const ImageView8& img, const Homography& h) const;

    // Per-cell colour from the chroma planes and the cells' raw luma (CapturedFrame::cell_rgb).
    [[nodiscard]] std::vector<double> SampleColor(const ChromaPlanes& chroma, const Homography& h,
                                                  const std::vector<double>& raw_y) const;

    // Coordinate descent over the four corners. Returns `h` unchanged if nothing improves.
    [[nodiscard]] Homography RefineCorners(const ImageView8& img, const Homography& h,
                                           double* gain, double first_step,
                                           int max_evaluations) const;

    // Process()'s alignment: warm-started from the previous frame's correction (SubcellConfig).
    [[nodiscard]] Homography AlignTracked(const ImageView8& img, const Homography& h0,
                                          bool reacquired);
    // The shift search over steps x steps candidates, `passes` times, coarse to fine.
    [[nodiscard]] Homography SearchShift(const ImageView8& img, const Homography& h, double range,
                                         int steps, int passes);
    // One step of the lens-strength search (SubcellConfig::fit_lens): returns the homography to
    // use with the (possibly updated) lens_.
    [[nodiscard]] Homography TuneLens(const ImageView8& img, const Homography& h);
    // The correction `h` makes to `h0`, as grid-cell offsets of the four corners; and back.
    void RememberCorrection(const Homography& h0, const Homography& h);
    [[nodiscard]] std::optional<Homography> ApplyCorrection(const Homography& h0) const;

    // A cell whose level is known by construction, for the alignment objective.
    struct Anchor {
        std::uint32_t col = 0;
        std::uint32_t row = 0;
        bool bright = false;
    };

    FramePipeline(const FrameLayout& layout, PipelineConfig cfg, CellSampler sampler,
                  CellSampler align_sampler, CellSampler chroma_sampler, ScreenTracker tracker,
                  std::vector<Anchor> anchors);

    const FrameLayout* layout_;
    PipelineConfig cfg_;
    CellSampler sampler_;
    CellSampler align_sampler_;  // wide window: the alignment objective only
    // Colour (M5): more points over more of the cell. Chroma is half resolution and, with the
    // ISP's noise reduction off, far noisier than luma; averaging is the cheap remedy, and the
    // colour of a cell's middle does not depend on exactly where its edges are.
    CellSampler chroma_sampler_;
    std::vector<Anchor> anchors_;
    ScreenTracker tracker_;

    Homography last_h_;
    RadialDistortion lens_{};  // this frame's lens model, in luma pixels
    double lens_k1_ = 0.0;     // this lane's estimate of the lens's strength
    double lens_step_ = 0.02;  // and how far its next search reaches
    bool lens_reading_ = false;  // the last frame read its header: safe to learn from the next
    bool lens_bootstrap_ = true;  // nothing has decoded yet: learn regardless
    int lens_failures_ = 0;      // frames since the last payload
    std::array<double, 8> correction_{};  // AlignTracked's last result, relative to its input
    bool correction_valid_ = false;
    int warm_frames_ = 0;  // since the last full search

    std::optional<PhotometricField> last_field_;

    bool last_ok_ = false;
    double last_score_ = 0.0;
    PipelineDiagnostics diag_;
};

}  // namespace fileflow
