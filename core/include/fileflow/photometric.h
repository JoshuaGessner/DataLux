// Spatially-varying photometric normalisation (component C09).
//
// M0Modulator::EstimateReference gives ONE dark/bright pair for the whole frame. That is
// only correct if the illumination field is flat, and it is not: lens vignetting, uneven
// backlight, glare, and -- specifically for the Galaxy S26 Ultra -- the Privacy Display's
// restricted angular emission (RISK-025) all produce a brightness that varies across the
// frame. At 30 cm a 6.9" screen subtends a wide angle, so the receiver sees the corners
// substantially further off-axis than the centre, producing a radial falloff that belongs
// to the TRANSMITTER rather than the lens.
//
// A single global threshold placed halfway between the frame-wide dark and bright levels
// will therefore sit above the local bright level in dim corners, turning every cell there
// into a confident WRONG decision -- the worst possible failure, because a confident error
// costs the FEC layer roughly twice what an erasure costs.
//
// This class estimates dark and bright levels as smooth FIELDS interpolated from the
// distributed pilot lattice. It never assumes a cause, so it corrects lens vignetting and
// panel angular falloff identically. That property is the concrete reason layout Candidate B
// (pilots throughout the payload area) is expected to beat Candidate A (pilots on the
// border, forcing extrapolation inward). See docs/specifications/OPTICAL-FRAME-CANDIDATES.md.
#pragma once

#include <fileflow/grid.h>
#include <fileflow/modulation.h>
#include <fileflow/result.h>

#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

struct PhotometricConfig {
    // Lattice nodes searched in each direction when gathering pilots for a node estimate.
    // 2 covers the immediate neighbourhood plus one ring of fallback, which matters where
    // markers and the header band punch holes in the lattice.
    int search_radius = 2;

    // Below this dark/bright gap the cell is unreadable and must be ERASED, not guessed.
    // Erasures are cheap for the fountain layer; confident wrong bits are not.
    // Lowered from 12.0 after measurement on the first above-cliff capture. Sweeping it against
    // header yield on 226 real frames, 4.0 beat every larger value (H 0.1753 vs 0.1649 at 8.0 and
    // 0.1340 at 30.0): cells between 4 and 12 counts of separation still carried more signal than
    // noise, and discarding them cost headers (F48).
    double min_separation = 4.0;

    // Inverse-distance weighting exponent. 2.0 is the standard Shepard choice.
    double idw_power = 2.0;

    // Erase a region when its pilots disagree with their own interpolated level by more
    // than this fraction of the local separation.
    //
    // Without this, interpolation is DANGEROUSLY well-behaved: an occluded patch, a glare
    // hotspot or a finger over the screen produces pilots that read nothing like their
    // expected level, but IDW simply blends in neighbours from outside the damaged area and
    // hands back a confident, wrong reference. The payload cells there then decode to
    // confident wrong bits. Residual is how the field notices that its own model does not
    // fit, which is the only local evidence available that a region is untrustworthy.
    // Effectively OFF, and that is an empirical result rather than a surrender.
    //
    // 0.25 was a hypothesis and it was wrong by a wide margin. On the first above-cliff capture it
    // erased 70% of the lattice and held header yield at H = 0.0258; relaxing it walked yield
    // monotonically up to H = 0.1753 with the test disabled, a 6.8x improvement, while erasure fell
    // from 0.7577 to 0.3477. The test was not rejecting bad references, it was rejecting cells
    // whose references were fine.
    //
    // The reason is the model, not the threshold: the residual is measured about a locally fitted
    // PLANE, and the real field is not planar over the fitting neighbourhood -- it varies by 6x
    // across the frame with structure a plane cannot follow. A high residual therefore reports
    // "this field is not a plane", which is true everywhere and says nothing about whether a cell
    // is readable. Restoring a residual test needs a better field model first (EXP-026).
    double max_pilot_residual_ratio = 1.2;
};

// Dark and bright reference levels sampled over the frame, plus per-region noise.
class PhotometricField {
  public:
    // `samples` is one raw value per grid cell, flat row-major, as produced by CellSampler.
    // Fails with kDegenerateParameters if the lattice yields no usable pilots of either
    // class -- which means the frame is unreadable and the caller must not fabricate one.
    static Result<PhotometricField> Estimate(const FrameLayout& layout,
                                             std::span<const double> samples,
                                             PhotometricConfig cfg = {});

    [[nodiscard]] PhotometricRef RefAt(std::uint32_t col, std::uint32_t row) const noexcept;

    // Local pilot-fit residual, in luminance units, measured about a locally fitted PLANE rather
    // than a locally constant level -- so a smooth brightness gradient across the frame is absorbed
    // by the fit instead of being reported as damage (F45).
    // High means "the pilots here do not
    // agree with each other" -- occlusion, glare or a tracking error. Exposed so the
    // adaptive link controller and telemetry can see WHY cells were erased.
    [[nodiscard]] double ResidualAt(std::uint32_t col, std::uint32_t row) const noexcept;

    // Normalise raw samples to a 0..255 scale against the LOCAL references, so downstream
    // stages that expect a global reference remain correct. Cells whose local separation is
    // below threshold become NaN (erasure).
    [[nodiscard]] std::vector<double> Normalise(std::span<const double> samples) const;

    // The reference a global estimator would have used. Kept for A/B comparison against the
    // field: EXP-003 measures whether the field actually pays for itself on real captures.
    [[nodiscard]] PhotometricRef global_ref() const noexcept { return global_; }

    // --- diagnostics; these are telemetry, not decoration ---
    [[nodiscard]] std::size_t bright_pilots_used() const noexcept { return n_bright_; }
    [[nodiscard]] std::size_t dark_pilots_used() const noexcept { return n_dark_; }
    // Ratio of largest to smallest bright level across the frame. ~1.0 means a flat field;
    // markedly above 1 is the signature RISK-025 predicts for the S26 Ultra.
    [[nodiscard]] double bright_nonuniformity() const noexcept;

    // Mean pilot-fit residual over the lattice, in luminance units, and the mean local
    // separation. Together these say WHICH of the two erasure mechanisms is firing when a frame
    // comes back almost entirely erased -- and they are different problems with different fixes.
    //
    // Low separation means the two levels are not far enough apart to threshold: blur, defocus, or
    // an exposure that has crushed one level. High residual means the pilots disagree with their own
    // fitted field: occlusion, glare, or a tracking error that has shifted the sampling grid off the
    // cells (F8). Without both numbers a 0.9 erasure rate is a dead end, which is exactly where the
    // first real capture left us.
    [[nodiscard]] double mean_residual() const noexcept;
    [[nodiscard]] double mean_separation() const noexcept;

    // How many lattice nodes FAIL each test, not just the average of the tests.
    //
    // A mean hides the thing that matters here. A frame can show healthy mean separation and a
    // residual comfortably inside budget while half its cells still erase, because erasure is
    // decided per node and the failures are spatially clustered -- one bad corner, a glare spot, a
    // region where the homography has drifted. Reporting only the means says "nothing is wrong"
    // about a frame that is losing half its payload, which is worse than saying nothing at all
    // (measured: mean separation 142 and residual 21.6 against a 35.5 budget, with a 0.5456 erasure
    // rate).
    [[nodiscard]] std::size_t node_count() const noexcept { return residual_.size(); }
    [[nodiscard]] std::size_t nodes_below_separation(double min_sep) const noexcept;
    [[nodiscard]] std::size_t nodes_above_residual(double ratio) const noexcept;

  private:
    PhotometricField() = default;

    GridGeometry g_{};
    std::uint32_t pitch_ = 16;
    std::uint32_t lat_cols_ = 0;
    std::uint32_t lat_rows_ = 0;
    std::vector<double> dark_;      // lattice-resolution field
    std::vector<double> bright_;    // lattice-resolution field
    std::vector<double> residual_;  // lattice-resolution pilot-fit residual
    PhotometricRef global_{};
    PhotometricConfig cfg_{};
    std::size_t n_bright_ = 0;
    std::size_t n_dark_ = 0;
};

}  // namespace fileflow
