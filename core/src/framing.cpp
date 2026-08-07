#include <fileflow/framing.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace fileflow {

std::string_view AimVerdictName(AimVerdict v) noexcept {
    switch (v) {
        case AimVerdict::kNoScreenFound: return "no screen found";
        case AimVerdict::kClipped:       return "clipped";
        case AimVerdict::kTooFar:        return "too far";
        case AimVerdict::kTooDark:       return "too dark";
        case AimVerdict::kTooBright:     return "too bright";
        case AimVerdict::kBlurred:       return "blurred";
        case AimVerdict::kReady:         return "ready";
    }
    return "unknown";
}

Status AimConfig::Validate() const noexcept {
    if (min_px_per_cell <= 0.0) return Error::kValueOutOfRange;
    if (target_margin < 0.0 || target_margin >= 0.5) return Error::kValueOutOfRange;
    if (min_bright_fraction < 0.0 || min_bright_fraction >= 1.0) return Error::kValueOutOfRange;
    if (max_bright_fraction <= min_bright_fraction || max_bright_fraction > 1.0) {
        return Error::kValueOutOfRange;
    }
    if (max_mid_fraction <= 0.0 || max_mid_fraction > 1.0) return Error::kValueOutOfRange;
    if (min_level_separation <= 0.0 || min_level_separation > 255.0) {
        return Error::kValueOutOfRange;
    }
    if (histogram_stride <= 0) return Error::kValueOutOfRange;
    return Status::Ok();
}

namespace {

// Otsu's split, returning the CLASS MEANS as well as the threshold.
//
// The means are what the rest of this file classifies against, and that is a correctness matter
// rather than a refinement. Classifying "dark" and "bright" as a fixed band either side of the
// THRESHOLD is wrong, because Otsu's threshold sits near the smaller class: when a screen occupies a
// quarter of the frame the threshold lands close to the bright level, a fixed band above it reaches
// past 255, and genuinely bright pixels get counted as neither bright nor dark. The result was a
// perfectly sharp synthetic frame reporting 46% "blur".
//
// `valid` is false when no split exists at all -- a uniform frame. Without that, a flat dark frame
// gets threshold 0, every pixel counts as lit, and the analyser confidently reports a screen filling
// the whole view.
struct Split {
    bool valid = false;
    int threshold = 0;
    double dark_mean = 0.0;
    double bright_mean = 0.0;
};

Split OtsuSplit(const std::array<std::uint64_t, 256>& hist) {
    std::uint64_t total = 0;
    double sum_all = 0.0;
    for (int i = 0; i < 256; ++i) {
        const auto n = hist[static_cast<std::size_t>(i)];
        total += n;
        sum_all += static_cast<double>(i) * static_cast<double>(n);
    }
    Split s;
    if (total == 0) return s;

    std::uint64_t w_b = 0;
    double sum_b = 0.0;
    double best_var = 0.0;
    for (int t = 0; t < 256; ++t) {
        const auto n = hist[static_cast<std::size_t>(t)];
        w_b += n;
        sum_b += static_cast<double>(t) * static_cast<double>(n);
        const std::uint64_t w_f = total - w_b;
        if (w_b == 0 || w_f == 0) continue;
        const double m_b = sum_b / static_cast<double>(w_b);
        const double m_f = (sum_all - sum_b) / static_cast<double>(w_f);
        const double var = static_cast<double>(w_b) * static_cast<double>(w_f) *
                           (m_b - m_f) * (m_b - m_f);
        if (var > best_var) {
            best_var = var;
            s.valid = true;
            s.threshold = t;
            s.dark_mean = m_b;
            s.bright_mean = m_f;
        }
    }
    return s;
}

// Recover the rotation and true extent of a rectangle of known aspect from its axis-aligned box.
//
// For a rectangle L by r*L rotated by theta the box is
//     L*(cos + r*sin)  by  L*(sin + r*cos)
// so the box's own aspect determines theta, and theta then un-inflates L.
//
// Worth being precise about the size of this correction, because it is easy to overstate: at 35
// degrees the box is 2.24x the screen's AREA but only 1.08x its LONG AXIS. Since px/cell is a linear
// measure, the correction to it is the 8%, not the 124%. The area figure is the one that matters for
// whether the screen fits in frame; the linear one is what matters for density.
struct Deskew {
    double rotation_deg = 0.0;
    double true_long = 0.0;
    double inflation = 1.0;
};

Deskew Unrotate(double long_px, double short_px, double aspect) {
    Deskew d;
    d.true_long = long_px;
    if (long_px <= 0.0 || short_px <= 0.0 || aspect <= 0.0) return d;

    const double ratio = short_px / long_px;
    // A box less elongated than the screen itself is the signature of rotation. A box MORE elongated
    // than the screen cannot be explained by rotation, so no angle is invented for it -- that would
    // mean perspective, or that the bright region is not the screen.
    if (ratio <= aspect) return d;

    const double denom = 1.0 - ratio * aspect;
    if (std::abs(denom) < 1e-9) return d;
    const double t = (ratio - aspect) / denom;
    if (!(t > 0.0)) return d;

    const double theta = std::atan(t);
    const double spread = std::cos(theta) + aspect * std::sin(theta);
    if (spread < 1e-9) return d;

    d.rotation_deg = theta * 180.0 / 3.14159265358979323846;
    d.true_long = long_px / spread;
    const double screen_area = d.true_long * d.true_long * aspect;
    if (screen_area > 0.0) d.inflation = (long_px * short_px) / screen_area;
    return d;
}

}  // namespace

Result<AimAdvice> AnalyseAim(const ImageView8& img, const GridGeometry& grid, AimConfig cfg) {
    FF_TRY(cfg.Validate());
    FF_TRY(grid.Validate());
    if (img.empty()) return Error::kValueOutOfRange;

    const int w = img.width();
    const int h = img.height();

    // --- threshold and class levels, from a strided histogram ---
    std::array<std::uint64_t, 256> hist{};
    std::uint64_t sampled = 0;
    double sum_lum = 0.0;
    for (int y = 0; y < h; y += cfg.histogram_stride) {
        for (int x = 0; x < w; x += cfg.histogram_stride) {
            const std::uint8_t v = img.At(x, y);
            ++hist[v];
            ++sampled;
            sum_lum += static_cast<double>(v);
        }
    }
    const Split split = OtsuSplit(hist);

    AimAdvice a;
    a.threshold = split.threshold;
    a.level_separation = split.bright_mean - split.dark_mean;
    a.mean_luminance = sampled > 0 ? sum_lum / static_cast<double>(sampled) : 0.0;

    // A frame with no two-level structure at all has no screen in it, whatever its brightness.
    // Checked before the bounding box, because a uniform frame would otherwise produce a box
    // covering everything.
    const double separation = split.bright_mean - split.dark_mean;
    if (!split.valid || separation < kMinSeparation) {
        a.verdict = AimVerdict::kNoScreenFound;
        a.guidance = "Point the camera at the sending phone's screen.";
        return a;
    }

    // --- bounding box of the bright region ---
    //
    // Streaming, O(1) state, zero allocation. F12 recorded what the alternative costs: collecting a
    // point per lit pixel reached 762 MB per frame at full sensor resolution, with the allocation
    // size chosen by attacker-controlled pixel content.
    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    std::uint64_t lit = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (img.At(x, y) > a.threshold) {
                ++lit;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    const double frame_area = static_cast<double>(w) * static_cast<double>(h);
    a.lit_fraction = static_cast<double>(lit) / frame_area;

    if (max_x < 0) {
        a.verdict = AimVerdict::kNoScreenFound;
        a.guidance = "Point the camera at the sending phone's screen.";
        return a;
    }

    a.bbox_x = min_x;
    a.bbox_y = min_y;
    a.bbox_w = max_x - min_x + 1;
    a.bbox_h = max_y - min_y + 1;
    a.clipped_left = (min_x == 0);
    a.clipped_top = (min_y == 0);
    a.clipped_right = (max_x == w - 1);
    a.clipped_bottom = (max_y == h - 1);

    const double long_px = static_cast<double>(std::max(a.bbox_w, a.bbox_h));
    const double short_px = static_cast<double>(std::min(a.bbox_w, a.bbox_h));
    const double aspect = static_cast<double>(grid.cols) / static_cast<double>(grid.rows);
    const Deskew d = Unrotate(long_px, short_px, aspect);
    a.rotation_deg = d.rotation_deg;
    a.bbox_inflation = d.inflation;
    a.px_per_cell = d.true_long / static_cast<double>(grid.rows);

    // --- per-tile coverage map ---
    //
    // Over the BOUNDING BOX rather than the frame: the question is which part of the screen is
    // unreadable, and background beyond the screen would dilute the edge tiles toward a verdict
    // about nothing. Sampled on a stride, because this runs several times a second beside the
    // analysis it advises about.
    for (int ty = 0; ty < kAimTileRows; ++ty) {
        for (int tx = 0; tx < kAimTileCols; ++tx) {
            const int x0 = min_x + (a.bbox_w * tx) / kAimTileCols;
            const int x1 = min_x + (a.bbox_w * (tx + 1)) / kAimTileCols;
            const int y0 = min_y + (a.bbox_h * ty) / kAimTileRows;
            const int y1 = min_y + (a.bbox_h * (ty + 1)) / kAimTileRows;
            std::array<std::uint64_t, 256> th{};
            for (int y = y0; y < y1 && y < h; y += cfg.histogram_stride) {
                for (int x = x0; x < x1 && x < w; x += cfg.histogram_stride) {
                    ++th[img.At(x, y)];
                }
            }
            const Split ts = OtsuSplit(th);
            const std::size_t slot =
                static_cast<std::size_t>(ty) * kAimTileCols + static_cast<std::size_t>(tx);
            // An invalid split means one class was empty: no two-level structure, which is exactly
            // what a defocused or occluded tile looks like. Reported as 0, not skipped, so the map
            // has no holes for a user to misread as "fine".
            a.tile_separation[slot] = ts.valid ? ts.bright_mean - ts.dark_mean : 0.0;
        }
    }

    // --- interior levels ---
    //
    // Classified against the two CLASS MEANS, a quarter of the separation in from each, so the bands
    // scale with whatever contrast the frame actually has. Measured over the middle half of the
    // bounding box so the boundary ring and the background beyond it cannot contribute.
    const double lo = split.dark_mean + 0.25 * separation;
    const double hi = split.bright_mean - 0.25 * separation;
    const int qx = a.bbox_w / 4;
    const int qy = a.bbox_h / 4;
    std::uint64_t dark = 0, bright = 0, mid = 0;
    for (int y = min_y + qy; y < min_y + qy + a.bbox_h / 2 && y < h; ++y) {
        for (int x = min_x + qx; x < min_x + qx + a.bbox_w / 2 && x < w; ++x) {
            const double v = static_cast<double>(img.At(x, y));
            if (v < lo) ++dark;
            else if (v > hi) ++bright;
            else ++mid;
        }
    }
    const double interior = static_cast<double>(dark + bright + mid);
    a.mid_fraction = interior > 0.0 ? static_cast<double>(mid) / interior : 1.0;
    // Reported, never gated on. It is a property of the PAYLOAD -- a zero fountain symbol renders
    // almost entirely dark -- so it is useful for a human reading a report and unusable as a
    // pass/fail criterion (F39).
    a.bright_fraction = interior > 0.0 ? static_cast<double>(bright) / interior : 0.0;

    // --- the verdict, in fix-this-first order ---
    //
    // Clipping is checked before density on purpose. A clipped screen fails detection outright, and
    // its measured px/cell is meaningless because part of the screen is not in the picture -- so
    // acting on density first would send the user in the wrong direction. That exact inversion cost
    // several hardware iterations (F33).
    if (a.clipped()) {
        a.verdict = AimVerdict::kClipped;
        std::string edges;
        if (a.clipped_top) edges += "top ";
        if (a.clipped_bottom) edges += "bottom ";
        if (a.clipped_left) edges += "left ";
        if (a.clipped_right) edges += "right ";
        a.guidance = "Move back a little — the screen runs off the " + edges +
                     "of the view and the whole edge must be visible.";
        if (a.rotation_deg > 12.0) {
            a.guidance += " Turning this phone to match the sender's orientation would also free up "
                          "room, since a tilted screen needs more space.";
        }
        return a;
    }

    // Density is the last measured thing standing between this rig and a payload, so the guidance
    // is specific about the target rather than just the direction. "Move closer" was acted on and
    // still landed below the cliff, because a cell is not a unit anyone can see.
    if (a.px_per_cell < cfg.min_px_per_cell) {
        a.verdict = AimVerdict::kTooFar;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "Move closer — %.1f pixels per cell, need %.1f. The sender's screen should "
                      "nearly fill the view.",
                      a.px_per_cell, cfg.min_px_per_cell);
        a.guidance = buf;
        return a;
    }

    // Exposure is judged on the DARK level and the interior balance, not on overall brightness.
    //
    // The signature of overexposure here is not a bright picture -- it is a LIFTED dark level, where
    // cells that should be black are no longer distinguishable from those that should be white. On
    // the first two-device capture the interior came back 66% bright against 9% dark (F33), which is
    // what "the dark cells washed out" looks like as a number.
    // Judged on LEVELS and their separation, never on how much of the screen happens to be lit.
    //
    // The lit fraction is a property of the PAYLOAD. A frame carrying a zero fountain symbol is
    // legitimately almost all dark -- only the boundary ring, corner markers and pilots are lit --
    // and it is perfectly decodable. Gating on `bright_frac` therefore reported "too dark" for good
    // frames, and made the verdict flicker as the transmitter cycled through its symbols: measured
    // on this rig, 22 of 64 source symbols were zero-padded and produced bursts of frames at mean
    // luminance 8.6 against 48 for the rest.
    //
    // That is F15's rule -- nothing the receiver does may depend on payload content -- broken again
    // in a new component (F39). The levels themselves are payload-independent: the pilot lattice and
    // boundary ring are present in every frame by construction, so Otsu still finds both classes.
    if (split.bright_mean < kMinBrightLevel) {
        a.verdict = AimVerdict::kTooDark;
        a.guidance = "Too dark — turn the sender's brightness up, or reduce room light behind it.";
        return a;
    }
    if (split.dark_mean > kMaxDarkLevel || separation < cfg.min_level_separation) {
        a.verdict = AimVerdict::kTooBright;
        a.guidance = "Too bright — the light and dark cells are washing together. Lower the "
                     "sender's brightness.";
        return a;
    }

    if (a.mid_fraction > cfg.max_mid_fraction) {
        a.verdict = AimVerdict::kBlurred;
        a.guidance = "Blurred — hold both phones still, and move slightly closer or further to let "
                     "focus settle.";
        return a;
    }

    a.verdict = AimVerdict::kReady;
    a.guidance = "Ready — hold still.";
    return a;
}

}  // namespace fileflow
