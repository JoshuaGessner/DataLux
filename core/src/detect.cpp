#include <fileflow/detect.h>

#include <fileflow/modulation.h>  // kLevelBright

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fileflow {
namespace {

double QuadArea(const std::array<Point2, 4>& q) {
    // Shoelace.
    double a = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const Point2& p = q[i];
        const Point2& n = q[(i + 1) % 4];
        a += p.x * n.y - n.x * p.y;
    }
    return std::fabs(a) * 0.5;
}

}  // namespace

LitRegion LargestLitRegion(const ImageView8& img, std::uint32_t threshold) {
    LitRegion out;
    if (img.empty()) return out;
    const int w = img.width();
    const int h = img.height();
    const int longest = std::max(w, h);
    const int block = std::max(4, (longest + kLitRegionMaxBlocks - 1) / kLitRegionMaxBlocks);
    const int bw = (w + block - 1) / block;
    const int bh = (h + block - 1) / block;

    // Lit samples per block, on a stride of 2. The structure that matters -- the boundary ring --
    // is a cell wide, and a cell is several pixels at any density that can be decoded at all, so
    // the stride cannot miss it.
    std::vector<std::uint32_t> lit(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh), 0);
    for (int y = 0; y < h; y += 2) {
        std::uint32_t* row = &lit[static_cast<std::size_t>(y / block) * static_cast<std::size_t>(bw)];
        for (int x = 0; x < w; x += 2) {
            if (img.At(x, y) > threshold) ++row[x / block];
        }
    }
    // A few stray hot pixels must not bridge two separate objects.
    const std::uint32_t samples = static_cast<std::uint32_t>((block / 2) * (block / 2));
    const std::uint32_t min_lit = std::max<std::uint32_t>(2, samples / 64);

    // 8-connected components over marked blocks; keep the one with the most lit samples.
    std::vector<int> label(lit.size(), -1);
    std::vector<int> stack;
    std::uint64_t best_weight = 0;
    std::uint64_t best_extent = 0;
    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    int next = 0;
    for (int sy = 0; sy < bh; ++sy) {
        for (int sx = 0; sx < bw; ++sx) {
            const std::size_t s = static_cast<std::size_t>(sy) * static_cast<std::size_t>(bw) +
                                  static_cast<std::size_t>(sx);
            if (lit[s] < min_lit || label[s] >= 0) continue;
            std::uint64_t weight = 0;
            int cx0 = sx, cy0 = sy, cx1 = sx, cy1 = sy;
            label[s] = next;
            stack.assign(1, static_cast<int>(s));
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int ix = i % bw;
                const int iy = i / bw;
                weight += lit[static_cast<std::size_t>(i)];
                cx0 = std::min(cx0, ix);
                cx1 = std::max(cx1, ix);
                cy0 = std::min(cy0, iy);
                cy1 = std::max(cy1, iy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = ix + dx;
                        const int ny = iy + dy;
                        if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                        const std::size_t n = static_cast<std::size_t>(ny) *
                                                  static_cast<std::size_t>(bw) +
                                              static_cast<std::size_t>(nx);
                        if (lit[n] < min_lit || label[n] >= 0) continue;
                        label[n] = next;
                        stack.push_back(static_cast<int>(n));
                    }
                }
            }
            ++next;
            // Ranked by EXTENT, not by how much of it is lit. A frame of zeros lights little more
            // than its ring and pilots, and a solid window would outweigh it by lit count; by
            // extent the screen wins, because it is the thing the person has filled the view with.
            const std::uint64_t extent = static_cast<std::uint64_t>(cx1 - cx0 + 1) *
                                         static_cast<std::uint64_t>(cy1 - cy0 + 1);
            if (extent > best_extent || (extent == best_extent && weight > best_weight)) {
                best_extent = extent;
                best_weight = weight;
                bx0 = cx0;
                by0 = cy0;
                bx1 = cx1;
                by1 = cy1;
            }
        }
    }
    if (best_weight == 0) return out;
    // Padded by one block. The tip of a rotated screen's corner is a sliver that can fall below
    // `min_lit` in its block, and the corner extremes need exactly that sliver.
    out.x0 = std::max(0, (bx0 - 1) * block);
    out.y0 = std::max(0, (by0 - 1) * block);
    out.x1 = std::min(w - 1, (bx1 + 2) * block - 1);
    out.y1 = std::min(h - 1, (by1 + 2) * block - 1);
    out.valid = true;
    return out;
}

std::uint32_t ScreenDetector::OtsuThreshold(const ImageView8& img, std::uint32_t stride) {
    std::array<std::uint64_t, 256> hist{};
    if (img.empty()) return 128;
    const int step = static_cast<int>(stride == 0 ? 1 : stride);

    std::uint64_t total = 0;
    for (int y = 0; y < img.height(); y += step) {
        for (int x = 0; x < img.width(); x += step) {
            ++hist[img.At(x, y)];
            ++total;
        }
    }
    if (total == 0) return 128;

    double sum_all = 0.0;
    for (std::size_t i = 0; i < 256; ++i) {
        sum_all += static_cast<double>(i) * static_cast<double>(hist[i]);
    }

    double sum_bg = 0.0;
    std::uint64_t w_bg = 0;
    double best_var = -1.0;
    std::uint32_t best_t = 128;

    for (std::size_t t = 0; t < 256; ++t) {
        w_bg += hist[t];
        if (w_bg == 0) continue;
        const std::uint64_t w_fg = total - w_bg;
        if (w_fg == 0) break;

        sum_bg += static_cast<double>(t) * static_cast<double>(hist[t]);
        const double mean_bg = sum_bg / static_cast<double>(w_bg);
        const double mean_fg = (sum_all - sum_bg) / static_cast<double>(w_fg);
        const double diff = mean_bg - mean_fg;
        const double var = static_cast<double>(w_bg) * static_cast<double>(w_fg) * diff * diff;

        if (var > best_var) {
            best_var = var;
            best_t = static_cast<std::uint32_t>(t);
        }
    }
    return best_t;
}

Result<ScreenDetector> ScreenDetector::Create(const FrameLayout& layout, DetectionConfig cfg) {
    if (!(cfg.min_area_fraction > 0.0) || cfg.min_area_fraction >= 1.0) {
        return Error::kValueOutOfRange;
    }
    if (!(cfg.min_marker_score > 0.0) || cfg.min_marker_score > 1.0) {
        return Error::kValueOutOfRange;
    }
    if (!(cfg.min_rotation_margin >= 0.0) || cfg.min_rotation_margin > 1.0) {
        return Error::kValueOutOfRange;
    }
    if (layout.config().boundary_width == 0) {
        // Localisation depends on the boundary ring existing.
        return Error::kDegenerateParameters;
    }
    return ScreenDetector(layout, cfg);
}

double ScreenDetector::ScoreMarkers(const ImageView8& img, const Homography& h,
                                    std::uint32_t threshold) const {
    const GridGeometry& g = layout_->geometry();
    std::size_t hits = 0;
    std::size_t total = 0;

    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            if (layout_->role(c, r) != CellRole::kMarker) continue;
            const Point2 p =
                h.Apply({static_cast<double>(c) + 0.5, static_cast<double>(r) + 0.5});
            const double v = img.SampleBilinear(p.x, p.y);
            if (std::isnan(v)) continue;  // off-image marker cells simply do not vote
            ++total;
            const bool observed_bright = v > static_cast<double>(threshold);
            const bool expected_bright = layout_->MarkerValue(c, r) == kLevelBright;
            if (observed_bright == expected_bright) ++hits;
        }
    }
    if (total == 0) return 0.0;
    return static_cast<double>(hits) / static_cast<double>(total);
}

Result<Detection> ScreenDetector::Detect(const ImageView8& img) const {
    if (img.empty()) return Error::kMarkersNotFound;

    const std::uint32_t thr = OtsuThreshold(img, cfg_.histogram_stride);

    // --- Locate the screen from lit pixels, in ONE streaming pass with no allocation ---
    // The screen is the brightest large structure because exposure is locked FOR the screen
    // (docs/research/android-camera-pipeline.md), which under-exposes the surroundings.
    // This is a stated assumption, not a universal truth: a screen against a brightly lit
    // background is the known failure case and is why verification below is mandatory.
    //
    // Confined to the largest connected lit structure, so a lamp or window elsewhere in the frame
    // cannot drag a corner out to itself (F56).
    const LitRegion region = LargestLitRegion(img, thr);
    if (!region.valid) return Error::kMarkersNotFound;
    CornerExtremes e;
    for (int y = region.y0; y <= region.y1; ++y) {
        for (int x = region.x0; x <= region.x1; ++x) {
            if (img.At(x, y) > thr) {
                e.Add(static_cast<double>(x), static_cast<double>(y));
            }
        }
    }

    if (!e.valid()) return Error::kMarkersNotFound;

    const std::array<Point2, 4> quad = e.quad();

    // The size test belongs on the QUAD AREA, never on the lit-pixel count.
    //
    // This previously rejected frames whose LIT-PIXEL COUNT fell below min_area_fraction of
    // the image. That is a different quantity entirely, and a payload-dependent one: a frame
    // carrying mostly zeros lights only the boundary, markers, pilots and header, so it can
    // sit below the threshold while the screen still occupies most of the image. Measured on
    // real transmitter output, 54 of 60 frames were refused this way -- every one of them a
    // systematic symbol in the zero-padded tail (finding F15).
    //
    // The consequences were worse than the failure rate suggests: it made SCREEN LOCALISATION
    // DEPEND ON PAYLOAD DATA, which is precisely what the persistent boundary ring and the
    // corner markers exist to prevent, and it would have surfaced in the field as
    // unreproducible flakiness on files containing runs of zeros.
    //
    // Quad area measures what the check was always meant to measure -- how much of the image
    // the screen occupies -- and is independent of what is being transmitted. Dropping the
    // early-out costs nothing: the pixel scan above runs either way, and what it skipped was
    // only a homography solve and marker scoring. A spurious quad from scattered noise still
    // fails marker verification below, which is the real gate.
    const double img_area = static_cast<double>(img.width()) * static_cast<double>(img.height());
    if (QuadArea(quad) < cfg_.min_area_fraction * img_area) return Error::kMarkersNotFound;

    // --- Resolve the 4-fold rotation ambiguity ---
    const GridGeometry& g = layout_->geometry();
    const std::array<Point2, 4> grid_corners{
        Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
        Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
        Point2{0, static_cast<double>(g.rows)}};

    Detection best;
    best.marker_score = -1.0;
    double runner_up = -1.0;

    for (int rot = 0; rot < 4; ++rot) {
        std::array<Point2, 4> rotated{};
        for (std::size_t i = 0; i < 4; ++i) {
            rotated[i] = quad[(i + static_cast<std::size_t>(rot)) % 4];
        }
        auto h = Homography::FromCorrespondences(
            std::span<const Point2, 4>(grid_corners), std::span<const Point2, 4>(rotated));
        if (!h.ok()) continue;

        const double score = ScoreMarkers(img, h.value(), thr);
        if (score > best.marker_score) {
            runner_up = best.marker_score;
            best.marker_score = score;
            best.grid_to_image = h.value();
            best.quad = rotated;
            best.rotation = rot;
        } else if (score > runner_up) {
            runner_up = score;
        }
    }

    if (best.marker_score < 0.0) return Error::kMarkersNotFound;

    best.runner_up_score = std::max(runner_up, 0.0);
    best.threshold = thr;

    // --- Verification. Refusing beats being confidently wrong. ---
    if (best.marker_score < cfg_.min_marker_score) return Error::kMarkersNotFound;
    if (best.marker_score - best.runner_up_score < cfg_.min_rotation_margin) {
        return Error::kMarkersNotFound;
    }

    return best;
}

}  // namespace fileflow
