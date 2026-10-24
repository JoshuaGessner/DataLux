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
                                        double* gain, double first_step,
                                        int max_evaluations) const {
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
    for (double step = first_step; step >= 0.02 && evals < max_evaluations; step /= 2.0) {
        bool improved = true;
        for (int round = 0; improved && round < 3; ++round) {
            improved = false;
            for (std::size_t k = 0; k < d.size(); ++k) {
                for (const double sign : {1.0, -1.0}) {
                    if (evals >= max_evaluations) break;
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
    h = SearchShift(img, h, cfg_.subcell.range, cfg_.subcell.steps, cfg_.subcell.passes);

    // --- Corners (F51) ---
    double corner_gain = 1.0;
    if (cfg_.subcell.refine_corners) {
        h = RefineCorners(img, h, &corner_gain, cfg_.subcell.corner_range / 2.0,
                          cfg_.subcell.corner_evaluations);
        if (corner_gain > 1.0) ++diag_.corner_refined_frames;
    }
    diag_.sum_corner_gain += corner_gain;
    return h;
}

Homography FramePipeline::SearchShift(const ImageView8& img, const Homography& h0, double range,
                                      int steps, int passes) {
    Homography h = h0;
    double best_dx = 0.0;
    double best_dy = 0.0;
    double best_sep = PilotContrast(img, h, 0.0, 0.0);
    const double base_sep = best_sep;
    for (int pass = 0; pass < passes; ++pass) {
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
    return h;
}

void FramePipeline::RememberCorrection(const Homography& h0, const Homography& h) {
    correction_valid_ = false;
    auto inv = h0.Inverse();
    if (!inv.ok()) return;
    const GridGeometry g = layout_->geometry();
    const std::array<Point2, 4> grid{Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
                                     Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
                                     Point2{0, static_cast<double>(g.rows)}};
    for (std::size_t i = 0; i < 4; ++i) {
        const Point2 p = inv.value().Apply(h.Apply(grid[i]));
        const double ex = p.x - grid[i].x;
        const double ey = p.y - grid[i].y;
        // Never carry a correction bigger than the search could have made: that is a wrong one.
        if (!std::isfinite(ex) || !std::isfinite(ey) || std::abs(ex) > 2.0 || std::abs(ey) > 2.0) {
            return;
        }
        correction_[2 * i] = ex;
        correction_[2 * i + 1] = ey;
    }
    correction_valid_ = true;
}

std::optional<Homography> FramePipeline::ApplyCorrection(const Homography& h0) const {
    if (!correction_valid_) return std::nullopt;
    const GridGeometry g = layout_->geometry();
    const std::array<Point2, 4> grid{Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
                                     Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
                                     Point2{0, static_cast<double>(g.rows)}};
    std::array<Point2, 4> img_pts{};
    for (std::size_t i = 0; i < 4; ++i) {
        img_pts[i] = h0.Apply({grid[i].x + correction_[2 * i], grid[i].y + correction_[2 * i + 1]});
    }
    auto r = Homography::FromCorrespondences(grid, img_pts);
    if (!r.ok()) return std::nullopt;
    return r.value();
}

Homography FramePipeline::TuneLens(const ImageView8& img, const Homography& h) {
    const GridGeometry g = layout_->geometry();
    const std::array<Point2, 4> grid{Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
                                     Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
                                     Point2{0, static_cast<double>(g.rows)}};
    // Where the grid's corners ARE in the image under the current model; a candidate strength
    // keeps them there and re-bends only what lies between.
    std::array<Point2, 4> at{};
    for (std::size_t i = 0; i < 4; ++i) at[i] = lens_.Apply(h.Apply(grid[i]));
    const SubcellConfig& sc = cfg_.subcell;

    // Only where the bow matters. It grows with the cube of the distance from the centre: a screen
    // whose corners stay well inside the picture is barely bent, the score is flat there, and
    // steering on it moved the strength the wrong way and cost a far frame its decode.
    double reach = 0.0;
    for (const Point2& p : at) {
        const double dx = p.x - lens_.cx;
        const double dy = p.y - lens_.cy;
        reach = std::max(reach, std::sqrt((dx * dx + dy * dy) * lens_.inv_norm2));
    }
    if (reach < sc.lens_min_reach) return h;

    const double base = PilotContrast(img, h);
    if (!(base > 0.0)) return h;

    double best = base;
    double best_k = lens_k1_;
    Homography best_h = h;
    const RadialDistortion current = lens_;
    for (const double sign : {-1.0, 1.0}) {
        const double k = std::clamp(lens_k1_ + sign * lens_step_, sc.lens_k1_min, sc.lens_k1_max);
        if (k == lens_k1_) continue;
        RadialDistortion trial = current;
        trial.k1 = k;
        std::array<Point2, 4> ideal{};
        for (std::size_t i = 0; i < 4; ++i) ideal[i] = trial.Invert(at[i]);
        auto r = Homography::FromCorrespondences(grid, ideal);
        if (!r.ok()) continue;
        // Scored after its own short corner refinement: the right strength also wants the
        // corners re-fitted, and scoring it on the old corners favours not moving at all.
        lens_ = trial;
        double gain = 1.0;
        const Homography refined =
            RefineCorners(img, r.value(), &gain, sc.warm_corner_step, sc.lens_corner_evaluations);
        const double c = PilotContrast(img, refined);
        lens_ = current;
        if (c > best) {
            best = c;
            best_k = k;
            best_h = refined;
        }
    }
    // Move only on a real gain: where the screen sits in the middle of the picture distortion
    // barely changes the score, and the estimate must not wander there.
    if (best > base * (1.0 + sc.lens_min_gain) && best_k != lens_k1_) {
        lens_k1_ = best_k;
        lens_.k1 = best_k;
        lens_step_ = std::min(lens_step_ * 1.5, sc.lens_step);
        RememberCorrection(best_h, best_h);  // the carried correction was relative to the old model
        correction_valid_ = false;
        return best_h;
    }
    lens_step_ = std::max(lens_step_ * 0.5, sc.lens_min_step);
    return h;
}

Homography FramePipeline::AlignTracked(const ImageView8& img, const Homography& h0,
                                       bool reacquired) {
    if (!cfg_.subcell.enabled) return h0;
    const SubcellConfig& sc = cfg_.subcell;
    if (sc.warm_start && !reacquired && warm_frames_ < sc.cold_every) {
        if (const auto hw = ApplyCorrection(h0)) {
            if (PilotContrast(img, *hw) >= PilotContrast(img, h0)) {
                Homography h = SearchShift(img, *hw, sc.warm_range, sc.warm_steps, 1);
                double corner_gain = 1.0;
                if (sc.refine_corners) {
                    h = RefineCorners(img, h, &corner_gain, sc.warm_corner_step,
                                      sc.warm_corner_evaluations);
                    if (corner_gain > 1.0) ++diag_.corner_refined_frames;
                }
                diag_.sum_corner_gain += corner_gain;
                ++warm_frames_;
                RememberCorrection(h0, h);
                return h;
            }
        }
    }
    const Homography h = Align(img, h0);
    warm_frames_ = 0;
    RememberCorrection(h0, h);
    return h;
}

double FramePipeline::PilotContrast(const ImageView8& img, const Homography& shifted) const {
    double sb = 0.0;
    double sd = 0.0;
    int nb = 0;
    int nd = 0;
    for (const Anchor& a : anchors_) {
        const double v = align_sampler_.SampleCell(img, shifted, a.col, a.row, lens_);
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
    // The lens model in chroma pixels: the same distortion, about the same point.
    RadialDistortion lens_c = lens_;
    lens_c.cx = (lens_.cx - 0.5) / 2.0;
    lens_c.cy = (lens_.cy - 0.5) / 2.0;
    lens_c.inv_norm2 = lens_.inv_norm2 * 4.0;
    const std::vector<double> u = chroma_sampler_.Sample(chroma.u, hc, lens_c);
    const std::vector<double> v = chroma_sampler_.Sample(chroma.v, hc, lens_c);
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
    // This frame's lens model: distortion about the image centre, normalised to the half-diagonal,
    // with this lane's current estimate of its strength (TuneLens).
    {
        const double hw = img.width() / 2.0;
        const double hh = img.height() / 2.0;
        lens_ = RadialDistortion{hw, hh, 1.0 / (hw * hw + hh * hh), lens_k1_};
    }
    // The tracker fits its homography to where the markers ARE, in the distorted image. Re-fit it
    // to where they would be through an ideal lens, so homography-then-distortion lands back on
    // them; otherwise the distortion moves the corners further than alignment searches.
    Homography h0 = tr.grid_to_image;
    if (lens_.active()) {
        const GridGeometry gg = layout_->geometry();
        const std::array<Point2, 4> grid{Point2{0, 0}, Point2{static_cast<double>(gg.cols), 0},
                                         Point2{static_cast<double>(gg.cols), static_cast<double>(gg.rows)},
                                         Point2{0, static_cast<double>(gg.rows)}};
        std::array<Point2, 4> ideal{};
        for (std::size_t i = 0; i < 4; ++i) ideal[i] = lens_.Invert(h0.Apply(grid[i]));
        if (auto r = Homography::FromCorrespondences(grid, ideal); r.ok()) h0 = r.value();
    }
    Homography h = AlignTracked(img, h0, tr.reacquired);
    if (warm_frames_ == 0 && cfg_.subcell.fit_lens) h = TuneLens(img, h);

    last_h_ = h;
    last_score_ = tr.marker_score;

    // --- Rectify and sample ---
    const auto raw = sampler_.Sample(img, h, lens_);

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
