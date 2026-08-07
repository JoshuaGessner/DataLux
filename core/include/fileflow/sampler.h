// Rectifying cell sampler (component C08) — image pixels to per-cell samples.
//
// Deliberately samples the image THROUGH the homography rather than warping the image
// first. Warping to a rectified buffer costs a full-frame resample (every pixel touched,
// interpolated and written) to produce an intermediate we then reduce to ~24,000 numbers.
// Sampling directly touches only the points we actually need -- roughly 4 samples per cell
// instead of ~100 pixels per cell -- and it avoids the extra interpolation generation that a
// warp-then-sample pipeline bakes into every value. It is also the shape that ports
// unchanged to a GPU shader, which ADR-0005 says the >=120 fps path requires.
#pragma once

#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>

#include <vector>

namespace fileflow {

struct SamplerConfig {
    // Fraction of each cell trimmed from every edge before sampling, in cell units.
    //
    // This is the single most important knob in the receiver. A cell's edge pixels are
    // contaminated by its neighbours through defocus, display pixel structure and any
    // homography error -- exactly the spatial crosstalk that [VISUALMIMO-CISS11] identifies
    // as the limit on multiplexing gain. Sampling only the interior trades signal energy for
    // neighbour rejection. 0.25 keeps the central half of each axis.
    //
    // Swept by EXP-015. HYPOTHESIS, not a tuned value.
    double interior_margin = 0.25;

    // NxN subsample points per cell, averaged. N=2 is a reasonable default: it suppresses
    // sensor noise and moire without the cost of a full box filter. N=1 samples the centre
    // only and is the cheapest option; higher N approaches a box filter over the interior.
    int samples_per_axis = 2;
};

// Maps grid space -> image space and reduces the image to one sample per cell.
class CellSampler {
  public:
    static Result<CellSampler> Create(GridGeometry g, SamplerConfig cfg);

    // `grid_to_image` maps grid coordinates ([0,cols] x [0,rows]) to image pixels.
    //
    // Output is flat row-major, one double per cell, in raw image units (0-255). Cells whose
    // sample points fall outside the image, or land on the horizon line of a degenerate
    // homography, are NaN -- the erasure convention. Partial coverage is honoured per
    // sample point, so a frame half out of view still yields usable cells for the half that
    // is visible rather than being discarded wholesale.
    [[nodiscard]] std::vector<double> Sample(const ImageView8& img,
                                             const Homography& grid_to_image) const;

    // One cell, same rule as [Sample]: NaN when no sub-sample lands inside the frame.
    //
    // Exposed so a caller can evaluate a handful of cells cheaply. Sub-cell alignment needs to
    // score a candidate geometry against the ~80 pilots, and sampling all 31,200 cells to look
    // at 80 of them would make the search cost more than the decode it is helping.
    [[nodiscard]] double SampleCell(const ImageView8& img, const Homography& grid_to_image,
                                    std::uint32_t col, std::uint32_t row) const;

    [[nodiscard]] const GridGeometry& geometry() const noexcept { return g_; }
    [[nodiscard]] const SamplerConfig& config() const noexcept { return cfg_; }

  private:
    CellSampler(GridGeometry g, SamplerConfig cfg);

    GridGeometry g_;
    SamplerConfig cfg_;
    // Sub-cell offsets in [margin, 1-margin], precomputed once.
    std::vector<double> offsets_;
};

}  // namespace fileflow
