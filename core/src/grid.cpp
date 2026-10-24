#include <fileflow/grid.h>

namespace fileflow {

Status GridGeometry::Validate() const noexcept {
    if (cols == 0 || rows == 0) return Error::kValueOutOfRange;
    if (cols > kMaxCols || rows > kMaxRows) return Error::kValueOutOfRange;
    return Status::Ok();
}

std::uint8_t FrameLayout::MarkerValue(std::uint32_t col, std::uint32_t row) const noexcept {
    const std::uint32_t m = cfg_.marker_size;
    const bool left = col < m;
    const bool top = row < m;

    // Local coordinates within the marker block.
    const std::uint32_t lc = left ? col : (g_.cols - 1 - col);
    const std::uint32_t lr = top ? row : (g_.rows - 1 - row);

    // Corner fiducial. Since screen LOCALISATION comes from the persistent boundary ring,
    // these markers have exactly one job: resolve the 4-fold rotation ambiguity. They are
    // therefore designed purely as a 4-way code with large Hamming distance, not as a
    // detection target.
    //
    // The previous design used a corner-specific NOTCH of 0-3 cells, which gave a minimum
    // pairwise distance of **1 cell out of 36** and a worst-case rotation margin of 2.8% --
    // a signal a single noisy cell could flip, so orientation was effectively a coin toss
    // (finding F9). This encodes a 2-bit corner ID across the whole inner block instead:
    // minimum pairwise distance 8/36, worst-case rotation margin 22.2%.
    //
    //   d == 0  outer edge  : always bright, so every corner anchors the boundary ring
    //   d == 1  separator   : always dark, so a corner always has visible dark structure
    //   d >= 2  inner block : the 2-bit corner ID
    const std::uint32_t d = (lc < lr) ? lc : lr;
    if (d == 0) return 255;
    if (d == 1) return 0;

    const std::uint32_t corner_id = (left ? 0u : 1u) + (top ? 0u : 2u);
    const std::uint32_t a = lc - 2;
    const std::uint32_t b = lr - 2;
    switch (corner_id) {
        case 0: return 255;                              // solid bright
        case 1: return 0;                                // solid dark      (distance 16)
        case 2: return ((a + b) % 2 == 0) ? 255 : 0;     // checker         (distance 8)
        default: return ((a + b) % 2 == 0) ? 0 : 255;    // inverse checker (distance 8/16)
    }
}

void FrameLayout::Build() {
    const std::uint32_t cols = g_.cols;
    const std::uint32_t rows = g_.rows;
    roles_.assign(static_cast<std::size_t>(cols) * rows, CellRole::kPayload);

    const std::uint32_t m = cfg_.marker_size;
    const std::uint32_t guard = cfg_.guard_width;

    auto mark = [&](std::uint32_t c, std::uint32_t r, CellRole role) {
        roles_[static_cast<std::size_t>(r) * cols + c] = role;
    };

    // --- Corner markers, with a guard ring so payload never abuts them ---
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            const bool in_c = (c < m) || (c >= cols - m);
            const bool in_r = (r < m) || (r >= rows - m);
            if (in_c && in_r) {
                mark(c, r, CellRole::kMarker);
                continue;
            }
            const bool guard_c = (c < m + guard) || (c + m + guard >= cols);
            const bool guard_r = (r < m + guard) || (r + m + guard >= rows);
            if (guard_c && guard_r) mark(c, r, CellRole::kGuard);
        }
    }

    // --- Persistent boundary ring: the screen-localisation signal ---
    // Claims perimeter cells not already taken by a marker. Always bright, so the detector
    // sees four long straight edges it can fit as lines and intersect for subpixel corners.
    const std::uint32_t bw = cfg_.boundary_width;
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            const bool on_ring = (c < bw) || (c + bw >= cols) || (r < bw) || (r + bw >= rows);
            if (!on_ring) continue;
            const CellRole existing = roles_[static_cast<std::size_t>(r) * cols + c];

            // Claims GUARD cells as well as payload, so the ring is UNBROKEN.
            //
            // The marker guard ring reaches the perimeter at eight points, and skipping those
            // left eight single-cell dark gaps in the boundary. Extremes-based detection does
            // not care -- the corners it keys on are marker cells -- but the ring exists to
            // support subpixel EDGE-LINE fitting, and a continuous edge is materially easier
            // to fit than a dotted one. A guard on the outermost row or column also has no
            // payload neighbour outside it, so it is not buying the isolation guards exist
            // for. Markers still win, since they carry the orientation code.
            if (existing == CellRole::kPayload || existing == CellRole::kGuard) {
                mark(c, r, CellRole::kBoundary);
            }
        }
    }

    // --- Timing tracks, just inside the boundary ring, between the markers ---
    if (rows > 2 * bw) {
        for (std::uint32_t c = m + guard; c + m + guard < cols; ++c) {
            if (roles_[static_cast<std::size_t>(bw) * cols + c] == CellRole::kPayload) {
                mark(c, bw, CellRole::kTimingTrack);
            }
            const std::uint32_t br = rows - 1 - bw;
            if (roles_[static_cast<std::size_t>(br) * cols + c] == CellRole::kPayload) {
                mark(c, br, CellRole::kTimingTrack);
            }
        }
    }

    // --- Header band, centred vertically: highest-SNR region, best `H` ---
    const std::uint32_t hdr_start = (rows > cfg_.header_rows) ? (rows - cfg_.header_rows) / 2 : 0;
    const std::uint32_t hdr_end = hdr_start + cfg_.header_rows;
    for (std::uint32_t r = hdr_start; r < hdr_end && r < rows; ++r) {
        for (std::uint32_t c = m + guard; c + m + guard < cols; ++c) {
            mark(c, r, CellRole::kHeader);
        }
    }
    // Guard rows above and below the header band.
    for (std::uint32_t g = 1; g <= guard; ++g) {
        if (hdr_start >= g) {
            for (std::uint32_t c = m + guard; c + m + guard < cols; ++c) {
                if (roles_[static_cast<std::size_t>(hdr_start - g) * cols + c] == CellRole::kPayload)
                    mark(c, hdr_start - g, CellRole::kGuard);
            }
        }
        if (hdr_end + g - 1 < rows) {
            for (std::uint32_t c = m + guard; c + m + guard < cols; ++c) {
                if (roles_[static_cast<std::size_t>(hdr_end + g - 1) * cols + c] ==
                    CellRole::kPayload)
                    mark(c, hdr_end + g - 1, CellRole::kGuard);
            }
        }
    }

    // --- Distributed pilot lattice: the defining feature of Candidate B ---
    const std::uint32_t pitch = cfg_.pilot_pitch == 0 ? 16 : cfg_.pilot_pitch;
    for (std::uint32_t r = pitch / 2; r < rows; r += pitch) {
        for (std::uint32_t c = pitch / 2; c < cols; c += pitch) {
            if (roles_[static_cast<std::size_t>(r) * cols + c] == CellRole::kPayload) {
                // Every fourth lattice point is reserved for colour, unused until M3.
                //
                // The rule MUST draw equally from both pilot parities. PilotValue()
                // alternates bright/dark on (i+j) parity, so the obvious `(i+j)%4==0`
                // reserves ONLY points that would have been bright, leaving the white level
                // estimated from half as many pilots as the black level (measured 1:2).
                // That is backwards: the white level is the one degraded most by angular
                // luminance falloff (RISK-025), so it needs MORE support, not less.
                // `(i+2j)%4==0` selects the same 25% while splitting evenly across parities.
                const std::uint32_t li = c / pitch;
                const std::uint32_t lj = r / pitch;
                const bool color = (li + 2 * lj) % 4 == 0;
                mark(c, r, color ? CellRole::kColorPilot : CellRole::kPilot);
            }
        }
    }

    // --- Index lists in raster order (transmission order) ---
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            const auto idx = static_cast<std::uint32_t>(r * cols + c);
            switch (roles_[idx]) {
                case CellRole::kPayload: payload_.push_back(idx); break;
                case CellRole::kHeader: header_.push_back(idx); break;
                case CellRole::kPilot: pilots_.push_back(idx); break;
                case CellRole::kColorPilot: color_pilots_.push_back(idx); break;
                default: break;
            }
        }
    }
}

Result<FrameLayout> FrameLayout::Create(GridGeometry g, LayoutConfig cfg) {
    FF_TRY(g.Validate());
    // Markers plus guards must leave a usable interior.
    const std::uint32_t consumed = 2 * (cfg.marker_size + cfg.guard_width);
    if (consumed + 4 >= g.cols || consumed + cfg.header_rows + 4 >= g.rows) {
        return Error::kValueOutOfRange;
    }
    FrameLayout l(g, cfg);
    l.Build();
    return l;
}

}  // namespace fileflow
