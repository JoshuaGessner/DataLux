#include <fileflow/pipeline.h>

#include <fileflow/modulation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

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
    return PilotContrast(img, ShiftGrid(h, dx, dy));
}

Homography FramePipeline::RefineCorners(const ImageView8& img, const Homography& h,
                                        double* gain) const {
    const GridGeometry g = layout_->geometry();
    const auto cols = static_cast<double>(g.cols);
    const auto rows = static_cast<double>(g.rows);
    const std::array<Point2, 4> grid{Point2{0, 0}, Point2{cols, 0}, Point2{cols, rows},
                                     Point2{0, rows}};

    // Offsets are in GRID cells at each corner, mapped through the current estimate -- so a step
    // means the same fraction of a cell at every corner however foreshortened the view is.
    std::array<double, 8> d{};
    auto build = [&](const std::array<double, 8>& off) -> std::optional<Homography> {
        std::array<Point2, 4> img_pts{};
        for (std::size_t i = 0; i < 4; ++i) {
            img_pts[i] = h.Apply({grid[i].x + off[2 * i], grid[i].y + off[2 * i + 1]});
        }
        auto r = Homography::FromCorrespondences(grid, img_pts);
        if (!r.ok()) return std::nullopt;
        return r.value();
    };

    const double base = PilotContrast(img, h);
    *gain = 1.0;
    if (!(base > 0.0)) return h;
    double best = base;
    int evals = 0;
    const double range = cfg_.subcell.corner_range;
    for (double step = range / 2.0; step >= 0.02 && evals < cfg_.subcell.corner_evaluations;
         step /= 2.0) {
        bool improved = true;
        for (int round = 0; improved && round < 3; ++round) {
            improved = false;
            for (std::size_t k = 0; k < d.size(); ++k) {
                for (const double sign : {1.0, -1.0}) {
                    if (evals >= cfg_.subcell.corner_evaluations) break;
                    const double cand = d[k] + sign * step;
                    if (std::abs(cand) > range) continue;
                    auto trial = d;
                    trial[k] = cand;
                    auto th = build(trial);
                    if (!th) continue;
                    ++evals;
                    const double s = PilotContrast(img, *th);
                    if (s > best) {
                        best = s;
                        d = trial;
                        improved = true;
                        break;
                    }
                }
            }
        }
    }
    auto out = build(d);
    if (!out) return h;
    *gain = best / base;
    return *out;
}

Homography FramePipeline::Align(const ImageView8& img, const Homography& h0) {
    Homography h = h0;
    if (!cfg_.subcell.enabled) return h;

    // --- Global shift ---
    //
    // Scored on the PILOTS, because their levels are known by construction: the objective is the
    // gap between the mean of the cells that must be bright and the mean of those that must be
    // dark. When the grid sits on the cells that gap is maximal; when it straddles them each
    // sample is a blend and the gap collapses. It optimises exactly the quantity that was
    // failing, and it needs no payload knowledge, so F15's rule holds (F47).
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

    // --- Corners (F51) ---
    double corner_gain = 1.0;
    if (cfg_.subcell.refine_corners) {
        h = RefineCorners(img, h, &corner_gain);
        if (corner_gain > 1.0) ++diag_.corner_refined_frames;
    }
    diag_.sum_corner_gain += corner_gain;
    return h;
}

double FramePipeline::PilotContrast(const ImageView8& img, const Homography& shifted) const {
    double sb = 0.0;
    double sd = 0.0;
    int nb = 0;
    int nd = 0;
    for (const Anchor& a : anchors_) {
        const double v = align_sampler_.SampleCell(img, shifted, a.col, a.row);
        if (std::isnan(v)) continue;
        if (a.bright) {
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
                             CellSampler align_sampler, CellSampler chroma_sampler,
                             ScreenTracker tracker, std::vector<Anchor> anchors)
    : layout_(&layout),
      cfg_(cfg),
      sampler_(std::move(sampler)),
      align_sampler_(std::move(align_sampler)),
      chroma_sampler_(std::move(chroma_sampler)),
      anchors_(std::move(anchors)),
      tracker_(std::move(tracker)) {}

Result<FramePipeline> FramePipeline::Create(const FrameLayout& layout, PipelineConfig cfg) {
    FF_ASSIGN_OR_RETURN(auto sampler, CellSampler::Create(layout.geometry(), cfg.sampler));
    FF_ASSIGN_OR_RETURN(
        auto align_sampler,
        CellSampler::Create(layout.geometry(),
                            SamplerConfig{.interior_margin = cfg.subcell.align_margin,
                                          .samples_per_axis = cfg.subcell.align_samples}));
    FF_ASSIGN_OR_RETURN(
        auto chroma_sampler,
        CellSampler::Create(layout.geometry(),
                            SamplerConfig{.interior_margin = 0.3, .samples_per_axis = 4}));
    FF_ASSIGN_OR_RETURN(auto tracker, ScreenTracker::Create(layout, cfg.tracker));

    // The known cells, levels taken from the modulator's own render of the structure so they can
    // never disagree with what a transmitter draws. Header and payload are left out: unknown.
    const GridGeometry g = layout.geometry();
    CellMatrix structure(g.cols, g.rows);
    FF_TRY(M0Modulator(layout).Render({}, {}, &structure));
    std::vector<Anchor> pilots;
    std::vector<Anchor> edges;
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            const CellRole role = layout.role(c, r);
            // Colour pilots too: under M5 they show colours, not the M0 levels rendered here.
            if (role == CellRole::kPayload || role == CellRole::kHeader ||
                role == CellRole::kColorPilot) {
                continue;
            }
            const Anchor a{c, r, structure.at(c, r) == kLevelBright};
            (role == CellRole::kPilot ? pilots : edges).push_back(a);
        }
    }
    std::vector<Anchor> anchors = std::move(pilots);
    const std::size_t room =
        cfg.subcell.max_anchors > anchors.size() ? cfg.subcell.max_anchors - anchors.size() : 0;
    if (!edges.empty() && room > 0) {
        const double stride = std::max(1.0, static_cast<double>(edges.size()) /
                                                static_cast<double>(room));
        for (double i = 0.0; i < static_cast<double>(edges.size()); i += stride) {
            anchors.push_back(edges[static_cast<std::size_t>(i)]);
        }
    }

    return FramePipeline(layout, cfg, std::move(sampler), std::move(align_sampler),
                         std::move(chroma_sampler), std::move(tracker), std::move(anchors));
}


std::vector<double> FramePipeline::SampleColor(const ChromaPlanes& chroma, const Homography& h,
                                               const std::vector<double>& raw_y) const {
    const std::size_t n = raw_y.size();
    std::vector<double> rgb(n * 3, std::nan(""));

    // Grid to CHROMA pixels: chroma pixel j covers luma pixels 2j and 2j+1, whose centres average
    // to 2j + 0.5, so x_chroma = (x_luma - 0.5) / 2 -- folded into the homography's rows.
    const auto& m = h.m();
    std::array<double, 9> cm{};
    for (std::size_t k = 0; k < 3; ++k) {
        cm[k] = 0.5 * m[k] - 0.25 * m[6 + k];
        cm[3 + k] = 0.5 * m[3 + k] - 0.25 * m[6 + k];
        cm[6 + k] = m[6 + k];
    }
    const Homography hc(cm);
    const std::vector<double> u = chroma_sampler_.Sample(chroma.u, hc);
    const std::vector<double> v = chroma_sampler_.Sample(chroma.v, hc);
    if (u.size() != n || v.size() != n) return rgb;

    // Camera RGB, BT.601 full range -- what Camera2's YUV_420_888 carries. Exactness does not
    // matter: the demodulator learns the colours per frame and needs only that they differ.
    std::array<std::vector<double>, 3> ch;
    for (auto& c : ch) c.assign(n, std::nan(""));
    for (std::size_t i = 0; i < n; ++i) {
        const double y = raw_y[i];
        const double cb = u[i] - 128.0;
        const double cr = v[i] - 128.0;
        if (std::isnan(y) || std::isnan(cb) || std::isnan(cr)) continue;
        ch[0][i] = y + 1.402 * cr;
        ch[1][i] = y - 0.344136 * cb - 0.714136 * cr;
        ch[2][i] = y + 1.772 * cb;
    }

    // Each channel gets its own black and white from the pilots, because the camera's white
    // balance is wherever it was left, and a white cell reads nothing like (255, 255, 255).
    //
    // NOT normalised against the luma field first, as it used to be. That assumed shading falls
    // on all three channels alike; through an OLED seen at an angle it does not -- its colour
    // shifts across the screen -- and subtracting luma's local black and dividing by luma's local
    // contrast then threw each channel's scale around cell by cell. On every saved Pixel 8 frame
    // the residual colour noise was lower without it (225 to 96, 50 to 28), and a frame that
    // failed now decodes (F70). Shading that does vary across the screen is the demodulator's
    // job: it learns the colours per tile.
    const GridGeometry g = layout_->geometry();
    for (std::size_t k = 0; k < 3; ++k) {
        const std::vector<double>& norm = ch[k];
        std::vector<double> bright;
        std::vector<double> dark;
        for (const std::uint32_t idx : layout_->pilot_cells()) {
            if (idx >= norm.size() || std::isnan(norm[idx])) continue;
            const bool is_bright = layout_->PilotValue(idx % g.cols, idx / g.cols) == kLevelBright;
            (is_bright ? bright : dark).push_back(norm[idx]);
        }
        if (bright.empty() || dark.empty()) return rgb;
        auto median = [](std::vector<double>& s) {
            const auto mid = static_cast<std::ptrdiff_t>(s.size() / 2);
            std::nth_element(s.begin(), s.begin() + mid, s.end());
            return s[static_cast<std::size_t>(mid)];
        };
        const double white = median(bright);
        const double black = median(dark);
        // A channel with no contrast between the pilots carries nothing: leave it erased.
        if (!(white - black > 8.0)) return rgb;
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isnan(norm[i])) rgb[i * 3 + k] = (norm[i] - black) / (white - black) * 255.0;
        }
    }
    return rgb;
}

CapturedFrame FramePipeline::Process(const ImageView8& img, std::uint64_t index,
                                     std::int64_t timestamp_ns, const ChromaPlanes* chroma) {
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
    const TrackResult tr = tracker_.Track(img, timestamp_ns);
    diag_.total_pixels_examined += tracker_.last_pixels_examined();

    if (!tr.ok) {
        ++diag_.geometry_failures;
        return out;
    }
    // --- Sub-cell alignment: global shift, then the corners independently ---
    const Homography h = Align(img, tr.grid_to_image);

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
    if (chroma != nullptr && !chroma->u.empty() && !chroma->v.empty()) {
        out.cell_rgb = SampleColor(*chroma, h, raw);
    }
    out.phase = FramePhase::kClean;  // temporal classification is component C07, not here
    last_ok_ = true;
    ++diag_.frames_decoded;
    return out;
}

}  // namespace fileflow
