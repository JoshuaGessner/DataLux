#include <fileflow/pipeline.h>

#include <cmath>

namespace fileflow {
namespace {

// Translate in GRID space, then map to the image: H' = H * T.
//
// `Compose` reads "apply this, then the argument", so translating first and mapping second is
// `T.Compose(H)`. Getting that order backwards shifts by a distance in PIXELS instead of cells,
// which on a rotated rig moves along the wrong axis entirely.
Homography ShiftGrid(const Homography& h, double dx, double dy) noexcept {
    const Homography t(std::array<double, 9>{1, 0, dx, 0, 1, dy, 0, 0, 1});
    return t.Compose(h);
}

}  // namespace

double FramePipeline::PilotContrast(const ImageView8& img, const Homography& h, double dx,
                                    double dy) const {
    const Homography shifted = ShiftGrid(h, dx, dy);
    const GridGeometry g = layout_->geometry();
    double sb = 0.0;
    double sd = 0.0;
    int nb = 0;
    int nd = 0;
    for (const std::uint32_t idx : layout_->pilot_cells()) {
        const std::uint32_t c = idx % g.cols;
        const std::uint32_t r = idx / g.cols;
        const double v = sampler_.SampleCell(img, shifted, c, r);
        if (std::isnan(v)) continue;
        if (layout_->PilotValue(c, r) == kLevelBright) {
            sb += v;
            ++nb;
        } else {
            sd += v;
            ++nd;
        }
    }
    // A candidate that cannot see both levels is not scoreable, and must not win by default.
    if (nb == 0 || nd == 0) return -1e300;
    return sb / static_cast<double>(nb) - sd / static_cast<double>(nd);
}

FramePipeline::FramePipeline(const FrameLayout& layout, PipelineConfig cfg, CellSampler sampler,
                             ScreenTracker tracker)
    : layout_(&layout),
      cfg_(cfg),
      sampler_(std::move(sampler)),
      tracker_(std::move(tracker)) {}

Result<FramePipeline> FramePipeline::Create(const FrameLayout& layout, PipelineConfig cfg) {
    FF_ASSIGN_OR_RETURN(auto sampler, CellSampler::Create(layout.geometry(), cfg.sampler));
    FF_ASSIGN_OR_RETURN(auto tracker, ScreenTracker::Create(layout, cfg.tracker));
    return FramePipeline(layout, cfg, std::move(sampler), std::move(tracker));
}

CapturedFrame FramePipeline::Process(const ImageView8& img, std::uint64_t index,
                                     std::int64_t timestamp_ns) {
    const GridGeometry g = layout_->geometry();

    CapturedFrame out;
    out.index = index;
    out.timestamp_ns = timestamp_ns;
    out.phase = FramePhase::kUnknown;
    out.cell_samples.assign(static_cast<std::size_t>(g.cells()), std::nan(""));

    ++diag_.frames_in;
    last_ok_ = false;
    last_score_ = 0.0;

    // --- Geometry ---
    if (cfg_.disable_tracking) tracker_.Reset();
    const TrackResult tr = tracker_.Track(img);
    diag_.total_pixels_examined += tracker_.last_pixels_examined();

    if (!tr.ok) {
        ++diag_.geometry_failures;
        return out;
    }
    Homography h = tr.grid_to_image;

    // --- Sub-cell alignment ---
    //
    // Scored on the PILOTS, because their levels are known by construction: the objective is the
    // gap between the mean of the cells that must be bright and the mean of those that must be
    // dark. When the grid sits on the cells that gap is maximal; when it straddles them each
    // sample is a blend and the gap collapses. It optimises exactly the quantity that was
    // failing, and it needs no payload knowledge, so F15's rule holds (F47).
    if (cfg_.subcell.enabled) {
        double best_dx = 0.0;
        double best_dy = 0.0;
        double best_sep = PilotContrast(img, h, 0.0, 0.0);
        const double base_sep = best_sep;
        double range = cfg_.subcell.range;
        for (int pass = 0; pass < cfg_.subcell.passes; ++pass) {
            const int steps = cfg_.subcell.steps;
            const double step = (2.0 * range) / static_cast<double>(steps - 1);
            const double cx = best_dx;
            const double cy = best_dy;
            for (int iy = 0; iy < steps; ++iy) {
                const double dy = cy - range + step * static_cast<double>(iy);
                for (int ix = 0; ix < steps; ++ix) {
                    const double dx = cx - range + step * static_cast<double>(ix);
                    const double sep = PilotContrast(img, h, dx, dy);
                    if (sep > best_sep) {
                        best_sep = sep;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }
            range = step;  // next pass searches one coarse step either side
        }
        if (best_dx != 0.0 || best_dy != 0.0) {
            h = ShiftGrid(h, best_dx, best_dy);
        }
        ++diag_.subcell_frames;
        diag_.sum_subcell_shift += std::sqrt(best_dx * best_dx + best_dy * best_dy);
        if (base_sep > 0.0) diag_.sum_subcell_gain += best_sep / base_sep;
    }

    last_h_ = h;
    last_score_ = tr.marker_score;

    // --- Rectify and sample ---
    const auto raw = sampler_.Sample(img, h);

    // --- Photometry ---
    auto field = PhotometricField::Estimate(*layout_, raw, cfg_.photometric);
    if (!field.ok()) {
        // The screen was located but cannot be read -- a different failure from not finding
        // it, and worth separating: this one points at exposure or lighting, not tracking.
        ++diag_.photometric_failures;
        return out;
    }

    const PhotometricField& pf = field.value();
    last_field_ = pf;
    ++diag_.photometric_frames;
    diag_.sum_bright_pilots += static_cast<double>(pf.bright_pilots_used());
    diag_.sum_dark_pilots += static_cast<double>(pf.dark_pilots_used());
    diag_.sum_separation += pf.mean_separation();
    diag_.sum_residual += pf.mean_residual();
    diag_.sum_bright_nonuniformity += pf.bright_nonuniformity();
    diag_.lattice_nodes += pf.node_count();
    diag_.nodes_low_separation += pf.nodes_below_separation(cfg_.photometric.min_separation);
    diag_.nodes_high_residual +=
        pf.nodes_above_residual(cfg_.photometric.max_pilot_residual_ratio);

    out.cell_samples = pf.Normalise(raw);
    out.phase = FramePhase::kClean;  // temporal classification is component C07, not here
    last_ok_ = true;
    ++diag_.frames_decoded;
    return out;
}

}  // namespace fileflow
