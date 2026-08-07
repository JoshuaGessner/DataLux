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

class FramePipeline {
  public:
    static Result<FramePipeline> Create(const FrameLayout& layout, PipelineConfig cfg = {});

    // Turn one image into per-cell samples.
    //
    // A frame that fails any stage still comes back as a CapturedFrame -- with all-NaN samples
    // and no ground truth -- rather than as an error. Losing a frame is normal operation on
    // this channel, and the fountain layer is built for it; the caller needs the index gap
    // recorded, not an exception. The reason is in diagnostics().
    [[nodiscard]] CapturedFrame Process(const ImageView8& img, std::uint64_t index,
                                        std::int64_t timestamp_ns);

    // Geometry from the most recent Process() call. Only meaningful when the last frame
    // decoded; check last_ok() first.
    [[nodiscard]] const Homography& last_homography() const noexcept { return last_h_; }
    [[nodiscard]] bool last_ok() const noexcept { return last_ok_; }
    [[nodiscard]] double last_marker_score() const noexcept { return last_score_; }

    [[nodiscard]] const PipelineDiagnostics& diagnostics() const noexcept { return diag_; }
    [[nodiscard]] const ScreenTracker& tracker() const noexcept { return tracker_; }
    [[nodiscard]] const FrameLayout& layout() const noexcept { return *layout_; }

    // Drop the geometry lock, e.g. across a session boundary.
    void Reset() noexcept { tracker_.Reset(); }

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

    FramePipeline(const FrameLayout& layout, PipelineConfig cfg, CellSampler sampler,
                  ScreenTracker tracker);

    const FrameLayout* layout_;
    PipelineConfig cfg_;
    CellSampler sampler_;
    ScreenTracker tracker_;

    Homography last_h_;

    std::optional<PhotometricField> last_field_;

    bool last_ok_ = false;
    double last_score_ = 0.0;
    PipelineDiagnostics diag_;
};

}  // namespace fileflow
