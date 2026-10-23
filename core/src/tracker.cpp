#include <fileflow/tracker.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fileflow {
namespace {

struct Bounds {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] double width() const noexcept { return x1 - x0; }
    [[nodiscard]] double height() const noexcept { return y1 - y0; }
};

Bounds QuadBounds(const std::array<Point2, 4>& q) {
    Bounds b{q[0].x, q[0].y, q[0].x, q[0].y};
    for (const auto& p : q) {
        b.x0 = std::min(b.x0, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x);
        b.y1 = std::max(b.y1, p.y);
    }
    return b;
}

// Characteristic size of a quad, used to normalise motion thresholds so they mean the same
// thing whether the screen is near or far.
double QuadScale(const std::array<Point2, 4>& q) {
    const Bounds b = QuadBounds(q);
    return std::hypot(b.width(), b.height());
}

}  // namespace

std::string_view TrackStateName(TrackState s) noexcept {
    switch (s) {
        case TrackState::kSearching: return "searching";
        case TrackState::kTracking: return "tracking";
        case TrackState::kDegraded: return "degraded";
        case TrackState::kLost: return "lost";
    }
    return "unknown";
}

ScreenTracker::ScreenTracker(const FrameLayout& layout, TrackerConfig cfg,
                             ScreenDetector detector)
    : layout_(&layout), cfg_(cfg), detector_(std::move(detector)) {}

Result<ScreenTracker> ScreenTracker::Create(const FrameLayout& layout, TrackerConfig cfg) {
    // Must be strictly below 1.0: the inner annulus boundary is ScaleQuad(quad, 1 - margin),
    // so margin >= 1.0 collapses it to a point or flips it through the centroid, and the
    // "annulus" stops meaning anything.
    if (!(cfg.search_margin > 0.0) || cfg.search_margin >= 1.0) return Error::kValueOutOfRange;
    if (!(cfg.degrade_score >= 0.0) || cfg.degrade_score > 1.0) return Error::kValueOutOfRange;
    if (cfg.max_degraded_frames < 0) return Error::kValueOutOfRange;
    if (!(cfg.max_corner_jump > 0.0)) return Error::kValueOutOfRange;
    if (!(cfg.velocity_tau_s > 0.0)) return Error::kValueOutOfRange;
    if (!(cfg.speed_margin_gain >= 0.0)) return Error::kValueOutOfRange;
    // Same bound as search_margin, for the same reason: the inner quad must not collapse. A cap
    // below the base margin is not an error -- it just means the window never widens.
    if (!(cfg.max_search_margin > 0.0) || cfg.max_search_margin >= 1.0) {
        return Error::kValueOutOfRange;
    }

    FF_ASSIGN_OR_RETURN(auto detector, ScreenDetector::Create(layout, cfg.detection));
    return ScreenTracker(layout, cfg, std::move(detector));
}

std::array<Point2, 4> ScreenTracker::Predicted(double dt_s) const noexcept {
    if (!cfg_.predict_motion) return last_quad_;
    std::array<Point2, 4> p{};
    for (std::size_t i = 0; i < 4; ++i) {
        p[i] = {last_quad_[i].x + velocity_[i].x * dt_s, last_quad_[i].y + velocity_[i].y * dt_s};
    }
    return p;
}

void ScreenTracker::UpdateMotion(const std::array<Point2, 4>& quad, double dt_s,
                                 bool fresh) noexcept {
    if (fresh || !(dt_s > 0.0)) {
        // A new lock carries no motion history. Inventing one from the last lock -- possibly
        // seconds ago and somewhere else entirely -- would aim the next search at nothing.
        velocity_ = {};
        return;
    }
    const double a = 1.0 - std::exp(-dt_s / cfg_.velocity_tau_s);
    for (std::size_t i = 0; i < 4; ++i) {
        velocity_[i].x = a * (quad[i].x - last_quad_[i].x) / dt_s + (1.0 - a) * velocity_[i].x;
        velocity_[i].y = a * (quad[i].y - last_quad_[i].y) / dt_s + (1.0 - a) * velocity_[i].y;
    }
}

double ScreenTracker::speed() const noexcept {
    if (state_ != TrackState::kTracking && state_ != TrackState::kDegraded) return 0.0;
    const double scale = QuadScale(last_quad_);
    if (!(scale > 1e-6)) return 0.0;
    double worst = 0.0;
    for (const Point2& v : velocity_) worst = std::max(worst, std::hypot(v.x, v.y));
    return worst / scale;
}

void ScreenTracker::Reset() noexcept {
    state_ = TrackState::kSearching;
    degraded_run_ = 0;
    velocity_ = {};
    last_ts_ = 0;
    // Cleared with the lock it belongs to. A rotation carried across a session boundary would be
    // applied to a screen that may now be held the other way up, silently mismatching every corner
    // -- which is the F37 failure reintroduced by the back door.
    rotation_ = 0;
}

bool ScreenTracker::RefineLocally(const ImageView8& img, TrackResult* out) {
    // --- Scan only an ANNULUS around the expected boundary ring ---
    //
    // The four corner extremes lie ON the boundary, so scanning the quad's interior finds
    // nothing and costs everything. Measured before this change (finding F13): tracking saved
    // NOTHING once the screen filled ~64% of the frame, because the dilated bounding box
    // covered the whole image -- and filling the frame is the configuration that maximises
    // pixels per cell, so the saving vanished precisely in the regime we expect to operate in.
    //
    // The annulus is built by scaling the quad about its own centroid rather than by insetting
    // a bounding box: a bbox annulus does not track a ROTATED quad's boundary, and an angled
    // receiver is the common case, not the exception.
    //
    // Centred on where the motion model EXPECTS the screen, and widened with its speed (F53): a
    // screen moving at 10% of its size per frame needs that much more window or it falls out of
    // the annulus and costs a full-image reacquisition every frame it keeps moving.
    const std::array<Point2, 4> expected = Predicted(dt_s_);
    // speed() is per second; the window has to cover the motion expected over THIS gap.
    const double margin = std::min(
        std::max(cfg_.max_search_margin, cfg_.search_margin),
        cfg_.search_margin +
            (cfg_.predict_motion ? cfg_.speed_margin_gain * speed() * dt_s_ : 0.0));
    const std::array<Point2, 4> outer = ScaleQuad(expected, 1.0 + margin);
    const std::array<Point2, 4> inner = ScaleQuad(expected, 1.0 - margin);

    const Bounds ob = QuadBounds(outer);
    const int y_lo = std::max(0, static_cast<int>(std::floor(ob.y0)));
    const int y_hi = std::min(img.height() - 1, static_cast<int>(std::ceil(ob.y1)));
    if (y_hi < y_lo) return false;

    CornerExtremes e;
    std::uint64_t examined = 0;

    for (int y = y_lo; y <= y_hi; ++y) {
        const RowSpan os = QuadRowSpan(outer, static_cast<double>(y));
        if (os.empty) continue;
        const RowSpan is = QuadRowSpan(inner, static_cast<double>(y));

        const int ox0 = std::max(0, static_cast<int>(std::floor(os.x0)));
        const int ox1 = std::min(img.width() - 1, static_cast<int>(std::ceil(os.x1)));
        if (ox1 < ox0) continue;

        // Two x-intervals where the inner quad splits the row, one where it does not.
        int spans[2][2];
        int n_spans = 0;
        if (is.empty) {
            spans[0][0] = ox0;
            spans[0][1] = ox1;
            n_spans = 1;
        } else {
            const int ix0 = static_cast<int>(std::floor(is.x0));
            const int ix1 = static_cast<int>(std::ceil(is.x1));
            if (ix0 > ox0) {
                spans[n_spans][0] = ox0;
                spans[n_spans][1] = std::min(ox1, ix0);
                ++n_spans;
            }
            if (ix1 < ox1) {
                spans[n_spans][0] = std::max(ox0, ix1);
                spans[n_spans][1] = ox1;
                ++n_spans;
            }
        }

        for (int s = 0; s < n_spans; ++s) {
            for (int x = spans[s][0]; x <= spans[s][1]; ++x) {
                ++examined;
                if (img.At(x, y) > threshold_) {
                    e.Add(static_cast<double>(x), static_cast<double>(y));
                }
            }
        }
    }

    // ACCUMULATE, never assign. When refinement fails the caller falls through to a full
    // acquisition, and the pixels scanned here were still spent. Overwriting would hide that
    // cost and bias the ADR-0006 comparison in favour of tracking -- the direction any
    // measurement of one's own design should be most suspicious of.
    last_px_ += examined;
    if (!e.valid()) {
        ++rj_extremes_;
        return false;
    }

    // Geometric order from the extremes, then rotated into GRID order using the rotation the
    // acquisition established.
    //
    // Both steps are needed and for different reasons. The comparison below is against
    // `last_quad_`, which is in grid order; and the homography maps grid corners to image points,
    // so it needs grid order too. Skipping this made refinement fail 60 times out of 60 on a real
    // capture while passing in simulation, because the simulator renders the screen upright where
    // the two orderings happen to coincide (F37).
    const std::array<Point2, 4> geometric{e.tl, e.tr, e.br, e.bl};
    std::array<Point2, 4> quad{};
    for (std::size_t i = 0; i < 4; ++i) {
        quad[i] = geometric[(i + static_cast<std::size_t>(rotation_)) % 4];
    }

    // Plausibility: the screen cannot teleport between frames. This is what stops the tracker
    // snapping onto a different bright object that happens to sit inside the window. Measured
    // against the PREDICTION, so a fast but steady pan is not mistaken for a teleport.
    const double scale = QuadScale(last_quad_);
    if (scale > 1e-6) {
        for (std::size_t i = 0; i < 4; ++i) {
            const double jump = std::hypot(quad[i].x - expected[i].x,
                                           quad[i].y - expected[i].y);
            if (jump > cfg_.max_corner_jump * scale) {
                ++rj_jump_;
                return false;
            }
        }
    }

    const GridGeometry& g = layout_->geometry();
    const std::array<Point2, 4> grid_corners{
        Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
        Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
        Point2{0, static_cast<double>(g.rows)}};

    auto h_full = Homography::FromCorrespondences(std::span<const Point2, 4>(grid_corners),
                                                  std::span<const Point2, 4>(quad));
    if (!h_full.ok()) {
        ++rj_homography_;
        return false;
    }

    // VERIFY against the markers. Refinement without verification would drift silently, and
    // orientation is NOT rechecked here -- a tracked frame inherits the rotation established
    // at acquisition, so a low score means the geometry is wrong, not merely rotated.
    const double score = detector_.ScoreMarkers(img, h_full.value(), threshold_);
    if (score < cfg_.detection.min_marker_score) {
        ++rj_score_;
        // Kept so the gap between "nearly passed" and "hopeless" is visible. A score just under the
        // bound means the threshold is mistuned for real optics; a score near zero means the refined
        // geometry is wrong, and those need opposite responses.
        if (score > worst_rj_score_) worst_rj_score_ = score;
        return false;
    }

    out->ok = true;
    out->grid_to_image = h_full.value();
    out->quad = quad;
    out->marker_score = score;
    return true;
}

TrackResult ScreenTracker::Track(const ImageView8& img, std::int64_t timestamp_ns) {
    TrackResult out;
    last_px_ = 0;  // per-frame cost accumulator; both paths below ADD to it

    // Elapsed time since the last lock. Real timestamps when they are usable, a nominal frame
    // otherwise; clamped so one stale timestamp cannot fling the prediction across the image.
    constexpr double kNominalFrameS = 1.0 / 30.0;
    const bool timed = timestamp_ns > 0 && last_ts_ > 0 && timestamp_ns > last_ts_;
    dt_s_ = timed ? std::min(static_cast<double>(timestamp_ns - last_ts_) * 1e-9, 0.5)
                  : kNominalFrameS;

    if (img.empty()) {
        state_ = (state_ == TrackState::kSearching) ? TrackState::kSearching : TrackState::kLost;
        out.state = state_;
        return out;
    }

    const bool have_lock = (state_ == TrackState::kTracking || state_ == TrackState::kDegraded);

    // --- Fast path: refine within a window around the previous quad ---
    if (have_lock && RefineLocally(img, &out)) {
        UpdateMotion(out.quad, dt_s_, /*fresh=*/false);
        last_quad_ = out.quad;
        last_ts_ = timestamp_ns;
        ++successful_;
        ++refined_;

        if (out.marker_score < cfg_.degrade_score) {
            ++degraded_run_;
            state_ = (degraded_run_ > cfg_.max_degraded_frames) ? TrackState::kLost
                                                                : TrackState::kDegraded;
            if (state_ == TrackState::kLost) {
                ++losses_;
                // Report the geometry we found but mark the lock dead, so the caller can use
                // this frame and the next one reacquires. Holding a failing lock is the thing
                // this state machine exists to prevent.
                degraded_run_ = 0;
            }
        } else {
            degraded_run_ = 0;
            state_ = TrackState::kTracking;
        }
        out.state = state_;
        return out;
    }

    if (have_lock) {
        // The window failed. Fall through to full reacquisition rather than trusting the
        // stale homography for another frame.
        ++losses_;
        degraded_run_ = 0;
    }

    // --- Slow path: full-image acquisition ---
    last_px_ += static_cast<std::uint64_t>(img.width()) * static_cast<std::uint64_t>(img.height());
    ++acquisitions_;
    out.reacquired = true;

    auto d = detector_.Detect(img);
    if (!d.ok()) {
        state_ = TrackState::kSearching;
        out.state = state_;
        return out;
    }

    UpdateMotion(d.value().quad, dt_s_, /*fresh=*/true);
    last_quad_ = d.value().quad;
    last_ts_ = timestamp_ns;
    threshold_ = d.value().threshold;  // carried into subsequent tracked frames
    // Carried for the same reason as the threshold: refinement must reproduce the orientation the
    // acquisition resolved, not re-derive it. A tracked frame deliberately does not rescore all four
    // rotations -- that is what makes it cheap -- so it has to be told which one won.
    rotation_ = d.value().rotation;
    state_ = TrackState::kTracking;
    degraded_run_ = 0;
    ++successful_;

    out.ok = true;
    out.grid_to_image = d.value().grid_to_image;
    out.quad = d.value().quad;
    out.marker_score = d.value().marker_score;
    out.state = state_;
    return out;
}

}  // namespace fileflow
