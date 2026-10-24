// Grid geometry and cell classification — the optical frame's spatial structure.
//
// Implements Candidate B ("distributed pilot lattice") from
// docs/specifications/OPTICAL-FRAME-CANDIDATES.md. B is the modelled favourite because it
// INTERPOLATES the illumination field rather than extrapolating it from a border, and it
// puts the header in the highest-SNR central region.
//
// No winner is declared yet -- EXP-013 sweeps A/B/C. The CellRole abstraction exists so a
// second layout can be added without touching the modulator or the FEC layer.
#pragma once

#include <fileflow/result.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fileflow {

enum class CellRole : std::uint8_t {
    kPayload = 0,
    kMarker,      // asymmetric corner fiducial
    kPilot,       // known luminance, drives photometric normalisation
    kColorPilot,  // reserved in every layout; unused until M3 (see MODULATION-SPEC)
    kHeader,
    kGuard,       // crosstalk isolation
    kTimingTrack,
    kBoundary,    // persistent always-bright perimeter ring; the screen-localisation signal
};

struct GridGeometry {
    std::uint32_t cols = 120;
    std::uint32_t rows = 200;

    // Candidate grids from the charter. Real limits come from EXP-001, which must also
    // locate the DENSITY CLIFF -- prior art shows goodput collapses past a threshold.
    static constexpr std::uint32_t kMaxCols = 512;
    static constexpr std::uint32_t kMaxRows = 512;

    [[nodiscard]] std::uint32_t cells() const noexcept { return cols * rows; }
    [[nodiscard]] Status Validate() const noexcept;
};

struct LayoutConfig {
    // One pilot every N cells in each axis. Tuned by EXP-013.
    std::uint32_t pilot_pitch = 16;
    std::uint32_t marker_size = 8;   // cells per side of each corner marker
    std::uint32_t header_rows = 6;   // header band height, in cells
    std::uint32_t guard_width = 1;   // guard ring around markers and header

    // Persistent always-bright perimeter ring, in cells.
    //
    // Required by the optical-frame spec ("persistent screen boundary") and the thing that
    // makes screen localisation cheap: four long straight high-contrast edges can be fitted
    // as LINES and intersected, which locates the corners far more accurately than any
    // blob centroid, because every pixel along an edge constrains the fit. Corner markers
    // then only have to resolve the 4-fold rotation ambiguity, which is what asymmetric
    // fiducials are actually good at.
    //
    // Costs 2*(cols+rows)-4 cells: ~2.7% at 120x200. Cheap for what it buys.
    std::uint32_t boundary_width = 1;
};

// Precomputed role for every cell, plus the payload cell index list.
// Built once per session, not per frame -- this is pure geometry.
class FrameLayout {
  public:
    static Result<FrameLayout> Create(GridGeometry g, LayoutConfig cfg);

    [[nodiscard]] const GridGeometry& geometry() const noexcept { return g_; }
    [[nodiscard]] const LayoutConfig& config() const noexcept { return cfg_; }

    [[nodiscard]] CellRole role(std::uint32_t col, std::uint32_t row) const noexcept {
        return roles_[static_cast<std::size_t>(row) * g_.cols + col];
    }

    // Flat indices (row-major) of cells carrying payload, in transmission order.
    [[nodiscard]] const std::vector<std::uint32_t>& payload_cells() const noexcept {
        return payload_;
    }
    [[nodiscard]] const std::vector<std::uint32_t>& header_cells() const noexcept {
        return header_;
    }
    // Reserved colour-reference cells, raster order. M0 draws them as ordinary pilots; M5 shows
    // the eight colours on them in turn, so the receiver has every colour's reference in-frame.
    [[nodiscard]] const std::vector<std::uint32_t>& color_pilot_cells() const noexcept {
        return color_pilots_;
    }
    [[nodiscard]] const std::vector<std::uint32_t>& pilot_cells() const noexcept {
        return pilots_;
    }

    // Non-payload cell fraction -- the `O` term in the goodput model.
    [[nodiscard]] double overhead_fraction() const noexcept {
        return 1.0 - static_cast<double>(payload_.size()) / static_cast<double>(g_.cells());
    }

    // Expected luminance for a pilot cell.
    //
    // MUST alternate across LATTICE INDICES, not raw cell coordinates. Lattice points sit
    // at c,r == pitch/2 (mod pitch), so any parity function of (col+row) is constant over
    // the whole lattice -- which would make every pilot the same level and leave the
    // receiver unable to estimate one of the two reference levels. That defeats the entire
    // point of the distributed pilot lattice. (Caught by
    // M0.DegenerateSeparationForcesErasures.)
    [[nodiscard]] std::uint8_t PilotValue(std::uint32_t col, std::uint32_t row) const noexcept {
        const std::uint32_t pitch = cfg_.pilot_pitch == 0 ? 16 : cfg_.pilot_pitch;
        return (((col / pitch) + (row / pitch)) % 2 == 0) ? 255 : 0;
    }

    // Marker cells form an asymmetric pattern per corner, so orientation is recoverable
    // from ANY THREE markers -- tolerating one occluded corner.
    [[nodiscard]] std::uint8_t MarkerValue(std::uint32_t col, std::uint32_t row) const noexcept;

  private:
    FrameLayout(GridGeometry g, LayoutConfig cfg) : g_(g), cfg_(cfg) {}
    void Build();

    GridGeometry g_;
    LayoutConfig cfg_;
    std::vector<CellRole> roles_;
    std::vector<std::uint32_t> payload_;
    std::vector<std::uint32_t> header_;
    std::vector<std::uint32_t> color_pilots_;
    std::vector<std::uint32_t> pilots_;
};

// A rendered optical frame: one luminance byte per cell.
class CellMatrix {
  public:
    CellMatrix() = default;
    CellMatrix(std::uint32_t cols, std::uint32_t rows)
        : cols_(cols), rows_(rows), v_(static_cast<std::size_t>(cols) * rows, 0) {}

    [[nodiscard]] std::uint32_t cols() const noexcept { return cols_; }
    [[nodiscard]] std::uint32_t rows() const noexcept { return rows_; }

    [[nodiscard]] std::uint8_t at(std::uint32_t c, std::uint32_t r) const noexcept {
        return v_[static_cast<std::size_t>(r) * cols_ + c];
    }
    void set(std::uint32_t c, std::uint32_t r, std::uint8_t val) noexcept {
        v_[static_cast<std::size_t>(r) * cols_ + c] = val;
    }

    [[nodiscard]] std::uint8_t flat(std::uint32_t i) const noexcept { return v_[i]; }
    void set_flat(std::uint32_t i, std::uint8_t val) noexcept { v_[i] = val; }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return v_; }
    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return v_; }

  private:
    std::uint32_t cols_ = 0;
    std::uint32_t rows_ = 0;
    std::vector<std::uint8_t> v_;
};

}  // namespace fileflow
