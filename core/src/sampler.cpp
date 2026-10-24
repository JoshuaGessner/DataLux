#include <fileflow/sampler.h>

#include <cmath>
#include <cstddef>

namespace fileflow {

CellSampler::CellSampler(GridGeometry g, SamplerConfig cfg) : g_(g), cfg_(cfg) {
    const int n = cfg_.samples_per_axis;
    offsets_.reserve(static_cast<std::size_t>(n));
    if (n == 1) {
        offsets_.push_back(0.5);  // cell centre
    } else {
        // Evenly spaced across the interior window, inset by half a step so the points sit
        // symmetrically inside [margin, 1-margin] rather than on its boundary.
        const double lo = cfg_.interior_margin;
        const double hi = 1.0 - cfg_.interior_margin;
        const double step = (hi - lo) / static_cast<double>(n);
        for (int i = 0; i < n; ++i) {
            offsets_.push_back(lo + step * (static_cast<double>(i) + 0.5));
        }
    }
}

Result<CellSampler> CellSampler::Create(GridGeometry g, SamplerConfig cfg) {
    FF_TRY(g.Validate());
    if (cfg.samples_per_axis < 1 || cfg.samples_per_axis > 8) {
        return Error::kValueOutOfRange;
    }
    // margin >= 0.5 would collapse the sampling window to nothing or invert it.
    if (!(cfg.interior_margin >= 0.0) || cfg.interior_margin >= 0.5) {
        return Error::kValueOutOfRange;
    }
    return CellSampler(g, cfg);
}

double CellSampler::SampleCell(const ImageView8& img, const Homography& grid_to_image,
                               std::uint32_t col, std::uint32_t row) const {
    if (img.empty()) return std::nan("");
    const auto n = static_cast<std::size_t>(cfg_.samples_per_axis);

    // Fast path: project the window's centre once and walk the sub-samples along the
    // homography's derivative there -- one divide per cell rather than one per sample, and no
    // per-sample bounds checks. This is most of a phone's decode time: alignment alone samples
    // ~2.8 million points a frame, and a Pixel 8's mid cores took 60 ms a frame doing it point
    // by point.
    //
    // Exact for an affine view; under perspective the error is second order in how much the
    // projective divisor changes across the window, so the path is taken only when that is under
    // a part in a thousand -- a few thousandths of a pixel. Anything else, and any window not
    // wholly inside the frame, takes the exact per-sample path below.
    {
        const auto& m = grid_to_image.m();
        const double mid = 0.5 * (offsets_.front() + offsets_.back());
        const double half = 0.5 * (offsets_.back() - offsets_.front());
        const double cx = static_cast<double>(col) + mid;
        const double cy = static_cast<double>(row) + mid;
        const double w = m[6] * cx + m[7] * cy + m[8];
        const double dw = (std::fabs(m[6]) + std::fabs(m[7])) * half;
        if (std::fabs(w) > 1e-12 && dw < 1e-3 * std::fabs(w)) {
            const double inv = 1.0 / w;
            const double x = (m[0] * cx + m[1] * cy + m[2]) * inv;
            const double y = (m[3] * cx + m[4] * cy + m[5]) * inv;
            const double xu = (m[0] - x * m[6]) * inv;  // d(image x) / d(grid x)
            const double xv = (m[1] - x * m[7]) * inv;  // d(image x) / d(grid y)
            const double yu = (m[3] - y * m[6]) * inv;
            const double yv = (m[4] - y * m[7]) * inv;
            // Affine, so the window's extremes are at its corners.
            const double ex = (std::fabs(xu) + std::fabs(xv)) * half;
            const double ey = (std::fabs(yu) + std::fabs(yv)) * half;
            if (x - ex >= 0.0 && y - ey >= 0.0 && x + ex < static_cast<double>(img.width() - 1) &&
                y + ey < static_cast<double>(img.height() - 1)) {
                double sum = 0.0;
                for (std::size_t oy = 0; oy < n; ++oy) {
                    const double dv = offsets_[oy] - mid;
                    const double rx = x + xv * dv;
                    const double ry = y + yv * dv;
                    for (std::size_t ox = 0; ox < n; ++ox) {
                        const double du = offsets_[ox] - mid;
                        sum += img.SampleBilinearInside(rx + xu * du, ry + yu * du);
                    }
                }
                return sum / static_cast<double>(n * n);
            }
        }
    }

    double sum = 0.0;
    int hits = 0;
    for (std::size_t oy = 0; oy < n; ++oy) {
        const double gy = static_cast<double>(row) + offsets_[oy];
        for (std::size_t ox = 0; ox < n; ++ox) {
            const double gx = static_cast<double>(col) + offsets_[ox];
            const Point2 p = grid_to_image.Apply({gx, gy});
            const double v = img.SampleBilinear(p.x, p.y);
            if (std::isnan(v)) continue;  // outside the frame: not a sample
            sum += v;
            ++hits;
        }
    }
    // Any usable sub-sample yields a cell value. Requiring all of them would discard whole
    // cells at the frame edge where partial coverage is normal and the available samples are
    // perfectly good.
    return hits > 0 ? sum / static_cast<double>(hits) : std::nan("");
}

std::vector<double> CellSampler::Sample(const ImageView8& img,
                                        const Homography& grid_to_image) const {
    std::vector<double> out(static_cast<std::size_t>(g_.cells()), std::nan(""));
    if (img.empty()) return out;

    for (std::uint32_t r = 0; r < g_.rows; ++r) {
        for (std::uint32_t c = 0; c < g_.cols; ++c) {
            const double v = SampleCell(img, grid_to_image, c, r);
            if (!std::isnan(v)) out[static_cast<std::size_t>(r) * g_.cols + c] = v;
        }
    }
    return out;
}

}  // namespace fileflow
