#include <fileflow/sim/render.h>

#include <fileflow/fountain.h>  // SplitMix64: motion phases must be reproducible
#include <fileflow/modulation.h>  // ColorChannelOn

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace fileflow::sim {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Head-on, the screen height covers this fraction of the image at distance 1.
constexpr double kFillFraction = 0.8;

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

Vec3 Rotate(Vec3 p, double yaw, double pitch, double roll) {
    // Rz(roll) * Ry(yaw) * Rx(pitch)
    const double cx = std::cos(pitch), sx = std::sin(pitch);
    Vec3 a{p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx};

    const double cy = std::cos(yaw), sy = std::sin(yaw);
    Vec3 b{a.x * cy + a.z * sy, a.y, -a.x * sy + a.z * cy};

    const double cz = std::cos(roll), sz = std::sin(roll);
    return {b.x * cz - b.y * sz, b.x * sz + b.y * cz, b.z};
}

}  // namespace

std::array<Point2, 4> GridCorners(const GridGeometry& g) {
    return {Point2{0, 0}, Point2{static_cast<double>(g.cols), 0},
            Point2{static_cast<double>(g.cols), static_cast<double>(g.rows)},
            Point2{0, static_cast<double>(g.rows)}};
}

std::array<Point2, 4> QuadFromView(const ViewConfig& v, const GridGeometry& g) {
    // Screen as a rectangle of unit height in the z=0 plane, aspect from the grid.
    const double h = 1.0;
    const double w = (g.rows > 0) ? h * static_cast<double>(g.cols) / static_cast<double>(g.rows)
                                  : h;

    const std::array<Vec3, 4> corners{
        Vec3{-w / 2, -h / 2, 0}, Vec3{w / 2, -h / 2, 0},
        Vec3{w / 2, h / 2, 0}, Vec3{-w / 2, h / 2, 0}};

    const double yaw = v.yaw_deg * kPi / 180.0;
    const double pitch = v.pitch_deg * kPi / 180.0;
    const double roll = v.roll_deg * kPi / 180.0;

    // Focal length fixed so distance actually changes apparent size.
    const double f = kFillFraction * static_cast<double>(v.image_height);
    const double cx = 0.5 * static_cast<double>(v.image_width) +
                      v.offset_x * static_cast<double>(v.image_width);
    const double cy = 0.5 * static_cast<double>(v.image_height) +
                      v.offset_y * static_cast<double>(v.image_height);

    std::array<Point2, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        Vec3 p = Rotate(corners[i], yaw, pitch, roll);
        p.z += v.distance;
        if (p.z <= 1e-6) return {Point2{0, 0}, Point2{0, 0}, Point2{0, 0}, Point2{0, 0}};
        out[i] = {f * p.x / p.z + cx, f * p.y / p.z + cy};
    }
    return out;
}

Point2 DistortPoint(Point2 p, const OpticalRenderConfig& cfg) noexcept {
    if (cfg.lens_k1 == 0.0) return p;
    const double cx = 0.5 * static_cast<double>(cfg.view.image_width);
    const double cy = 0.5 * static_cast<double>(cfg.view.image_height);
    const double norm = std::hypot(cx, cy);
    const double ux = (p.x - cx) / norm;
    const double uy = (p.y - cy) / norm;
    const double s = 1.0 + cfg.lens_k1 * (ux * ux + uy * uy);
    return {cx + ux * s * norm, cy + uy * s * norm};
}

namespace {

// Inverse of DistortPoint by fixed-point iteration: the renderer walks OUTPUT pixels, so it needs
// to know which undistorted point each recorded pixel saw. Converges in a few steps for the
// |k1| <= ~0.1 any real corrected lens leaves behind.
Point2 UndistortPoint(Point2 d, const OpticalRenderConfig& cfg) noexcept {
    if (cfg.lens_k1 == 0.0) return d;
    const double cx = 0.5 * static_cast<double>(cfg.view.image_width);
    const double cy = 0.5 * static_cast<double>(cfg.view.image_height);
    const double norm = std::hypot(cx, cy);
    const double dx = (d.x - cx) / norm;
    const double dy = (d.y - cy) / norm;
    double ux = dx;
    double uy = dy;
    for (int i = 0; i < 6; ++i) {
        const double s = 1.0 + cfg.lens_k1 * (ux * ux + uy * uy);
        ux = dx / s;
        uy = dy / s;
    }
    return {cx + ux * norm, cy + uy * norm};
}

// The emitted luminance at image pixel (x, y) seen through `inv` (image -> grid), before noise.
// Returns false, with the background in `*out`, where the pixel misses the screen. The hit is
// reported separately because a dark cell and the background can have the same value.
bool Shade(const CellMatrix& cells, const Homography& inv, const OpticalRenderConfig& cfg, int x,
           int y, double* out) {
    *out = cfg.background;
    const Point2 g =
        inv.Apply(UndistortPoint({static_cast<double>(x), static_cast<double>(y)}, cfg));
    if (!std::isfinite(g.x) || !std::isfinite(g.y) || g.x < 0 || g.y < 0) return false;
    const auto c = static_cast<std::int64_t>(g.x);
    const auto r = static_cast<std::int64_t>(g.y);
    if (c >= cells.cols() || r >= cells.rows()) return false;

    double v = cells.at(static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(r));
    const double gcx = 0.5 * static_cast<double>(cells.cols());
    const double gcy = 0.5 * static_cast<double>(cells.rows());
    const double max_r = std::hypot(gcx, gcy);
    if (cfg.corner_falloff != 1.0 && max_r > 0.0) {
        const double rad = std::hypot(g.x - gcx, g.y - gcy) / max_r;
        v *= 1.0 + (cfg.corner_falloff - 1.0) * rad * rad;
    }
    v += cfg.black_lift;
    if (cfg.glare_lift != 0.0 && cells.cols() > 0) {
        const double t = 1.0 - std::clamp(g.x / static_cast<double>(cells.cols()), 0.0, 1.0);
        v += cfg.glare_lift * t;
    }
    *out = v;
    return true;
}

// Quantise, add the deterministic sensor noise, then the defocus blur -- shared by the static and
// moving renderers so the two differ ONLY in geometry. `on_screen` marks pixels that saw the
// screen; noise is applied there alone, as the static renderer always did.
Image8 Finish(const std::vector<double>& radiance, const std::vector<std::uint8_t>& on_screen,
              const OpticalRenderConfig& cfg) {
    const int w = cfg.view.image_width;
    const int h = cfg.view.image_height;
    Image8 img(w, h, cfg.background);

    std::uint64_t rng = cfg.seed | 1ULL;
    const auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                  static_cast<std::size_t>(x);
            if (!on_screen[i]) continue;
            double v = radiance[i];
            if (cfg.noise_amplitude > 0.0) {
                const double u = static_cast<double>(next() % 2001) / 1000.0 - 1.0;
                v += u * cfg.noise_amplitude;
            }
            img.set(x, y, static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0)));
        }
    }

    if (cfg.blur_radius > 0) {
        Image8 blurred(img.width(), img.height(), cfg.background);
        const int k = cfg.blur_radius;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                int sum = 0;
                int n = 0;
                for (int dy = -k; dy <= k; ++dy) {
                    for (int dx = -k; dx <= k; ++dx) {
                        const int sx = x + dx;
                        const int sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= img.width() || sy >= img.height()) {
                            continue;
                        }
                        sum += img.at(sx, sy);
                        ++n;
                    }
                }
                blurred.set(x, y, static_cast<std::uint8_t>(n > 0 ? sum / n : 0));
            }
        }
        return blurred;
    }
    return img;
}

Homography ViewHomography(const ViewConfig& v, const GridGeometry& g, bool* ok) {
    const auto quad = QuadFromView(v, g);
    const auto corners = GridCorners(g);
    auto h = Homography::FromCorrespondences(std::span<const Point2, 4>(corners),
                                             std::span<const Point2, 4>(quad));
    *ok = h.ok();
    return h.ok() ? h.value() : Homography::Identity();
}

}  // namespace

YuvImage RenderColorView(const CellMatrix& cells, const GridGeometry& g,
                         const OpticalRenderConfig& cfg, const ColorChannelConfig& color,
                         Homography* used_transform) {
    // Each display channel through the optics on its own: blur, falloff, glare and noise apply
    // to light, channel by channel, before the sensor's filters see it.
    std::array<Image8, 3> plane;
    for (int k = 0; k < 3; ++k) {
        CellMatrix ch(cells.cols(), cells.rows());
        for (std::uint32_t r = 0; r < cells.rows(); ++r) {
            for (std::uint32_t c = 0; c < cells.cols(); ++c) {
                ch.set(c, r, ColorChannelOn(cells.at(c, r), k) ? 255 : 0);
            }
        }
        OpticalRenderConfig one = cfg;
        one.seed = cfg.seed + static_cast<std::uint64_t>(k) * 7919U;
        plane[static_cast<std::size_t>(k)] =
            RenderView(ch, g, one, k == 0 ? used_transform : nullptr);
    }

    const int w = cfg.view.image_width;
    const int h = cfg.view.image_height;
    YuvImage out{Image8(w, h), Image8(w / 2, h / 2), Image8(w / 2, h / 2)};
    std::vector<double> us(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<double> vs(us.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::array<double, 3> cam{};
            for (std::size_t i = 0; i < 3; ++i) {
                double acc = 0.0;
                for (std::size_t j = 0; j < 3; ++j) {
                    acc += color.mix[i][j] * static_cast<double>(plane[j].at(x, y));
                }
                cam[i] = std::clamp(acc * color.gain[i], 0.0, 255.0);
            }
            const double yy = 0.299 * cam[0] + 0.587 * cam[1] + 0.114 * cam[2];
            out.y.set(x, y, static_cast<std::uint8_t>(std::lround(std::clamp(yy, 0.0, 255.0))));
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                  static_cast<std::size_t>(x);
            us[i] = 128.0 - 0.168736 * cam[0] - 0.331264 * cam[1] + 0.5 * cam[2];
            vs[i] = 128.0 + 0.5 * cam[0] - 0.418688 * cam[1] - 0.081312 * cam[2];
        }
    }
    // 4:2:0: each chroma sample the mean of its 2x2 block.
    for (int y = 0; y < h / 2; ++y) {
        for (int x = 0; x < w / 2; ++x) {
            double su = 0.0;
            double sv = 0.0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t i =
                        static_cast<std::size_t>(2 * y + dy) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(2 * x + dx);
                    su += us[i];
                    sv += vs[i];
                }
            }
            out.u.set(x, y, static_cast<std::uint8_t>(std::lround(std::clamp(su / 4.0, 0.0, 255.0))));
            out.v.set(x, y, static_cast<std::uint8_t>(std::lround(std::clamp(sv / 4.0, 0.0, 255.0))));
        }
    }
    return out;
}

Image8 RenderOptical(const CellMatrix& cells, const Homography& grid_to_image,
                     const OpticalRenderConfig& cfg) {
    auto inv_r = grid_to_image.Inverse();
    if (!inv_r.ok()) return Image8(cfg.view.image_width, cfg.view.image_height, cfg.background);
    const Homography inv = inv_r.value();

    const auto n = static_cast<std::size_t>(cfg.view.image_width) *
                   static_cast<std::size_t>(cfg.view.image_height);
    std::vector<double> radiance(n, cfg.background);
    std::vector<std::uint8_t> on_screen(n, 0);
    for (int y = 0; y < cfg.view.image_height; ++y) {
        for (int x = 0; x < cfg.view.image_width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(cfg.view.image_width) +
                                  static_cast<std::size_t>(x);
            if (Shade(cells, inv, cfg, x, y, &radiance[i])) on_screen[i] = 1;
        }
    }
    return Finish(radiance, on_screen, cfg);
}

ViewConfig ViewAt(const ViewConfig& base, const MotionConfig& m, double t) noexcept {
    // Two incommensurate sway frequencies per axis plus tremor, with phases from the seed, so the
    // path is smooth and never periodic over a transfer.
    SplitMix64 rng(m.phase_seed);
    auto phase = [&rng] { return rng.NextDouble() * 2.0 * kPi; };
    const double p[8] = {phase(), phase(), phase(), phase(), phase(), phase(), phase(), phase()};
    const double s = m.shake;
    auto wave = [t](double amp, double hz, double ph) {
        return amp * std::sin(2.0 * kPi * hz * t + ph);
    };

    // Triangle wave: constant speed `rate`, turning round at +/-range.
    auto sweep = [&m, t](double rate) {
        if (rate == 0.0 || !(m.pan_range > 0.0)) return 0.0;
        const double period = 4.0 * m.pan_range / std::abs(rate);
        const double u = std::fmod(t, period) / period;  // [0, 1)
        const double tri = u < 0.25 ? u * 4.0 : (u < 0.75 ? 2.0 - u * 4.0 : u * 4.0 - 4.0);
        return m.pan_range * tri;
    };

    ViewConfig v = base;
    v.offset_x += sweep(m.pan_x) + s * (wave(0.015, 1.1, p[0]) + wave(0.0025, 9.0, p[1]));
    v.offset_y += sweep(m.pan_y) + s * (wave(0.010, 0.8, p[2]) + wave(0.0025, 8.3, p[3]));
    v.roll_deg += s * (wave(0.8, 0.7, p[4]) + wave(0.15, 9.7, p[5]));
    v.yaw_deg += s * wave(1.0, 0.45, p[6]);
    v.distance *= 1.0 + s * wave(0.02, 0.5, p[7]);
    return v;
}

Image8 RenderMoving(const CellMatrix& cells, const GridGeometry& g,
                    const OpticalRenderConfig& cfg, const MotionConfig& m, double t_start,
                    Homography* truth) {
    const int w = cfg.view.image_width;
    const int h = cfg.view.image_height;
    const int k = std::max(1, m.subframes);
    const int bands = std::clamp(m.bands, 1, std::max(1, h));

    // Truth at the middle of the exposure AND the readout: the one homography a receiver could
    // most nearly recover. Rows above and below it saw the screen elsewhere.
    bool ok = false;
    const Homography mid =
        ViewHomography(ViewAt(cfg.view, m, t_start + 0.5 * (m.exposure_s + m.readout_s)), g, &ok);
    if (truth != nullptr) *truth = mid;

    const auto n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<double> radiance(n, 0.0);
    std::vector<std::uint8_t> hits(n, 0);
    for (int s = 0; s < k; ++s) {
        const double ts = t_start + m.exposure_s * (static_cast<double>(s) + 0.5) /
                                        static_cast<double>(k);
        for (int b = 0; b < bands; ++b) {
            const int y0 = b * h / bands;
            const int y1 = (b + 1) * h / bands;
            const double row_t = m.readout_s * (0.5 * (y0 + y1)) / static_cast<double>(h);
            bool vok = false;
            const Homography hb = ViewHomography(ViewAt(cfg.view, m, ts + row_t), g, &vok);
            auto inv = hb.Inverse();
            if (!vok || !inv.ok()) continue;
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                          static_cast<std::size_t>(x);
                    double v = 0.0;
                    // A pixel the screen crossed during the exposure is a screen pixel: it carries
                    // the blur's mix of screen and background, and noise like any other.
                    if (Shade(cells, inv.value(), cfg, x, y, &v)) hits[i] = 1;
                    radiance[i] += v;
                }
            }
        }
    }
    for (double& v : radiance) v /= static_cast<double>(k);
    return Finish(radiance, hits, cfg);
}

Image8 RenderUnsynced(const CellMatrix& (*state)(void* ctx, std::int64_t k), void* ctx, double fd,
                      const GridGeometry& g, const OpticalRenderConfig& cfg,
                      const CaptureTiming& ct, double t_start, Homography* truth,
                      int* states_hit) {
    const int w = cfg.view.image_width;
    const int h = cfg.view.image_height;
    bool ok = false;
    const Homography hom = ViewHomography(cfg.view, g, &ok);
    if (truth != nullptr) *truth = hom;
    auto inv = hom.Inverse();
    const auto n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<double> radiance(n, 0.0);
    std::vector<std::uint8_t> hits(n, 0);
    if (!ok || !inv.ok() || fd <= 0.0) return Finish(radiance, hits, cfg);

    const int bands = std::clamp(ct.bands, 1, std::max(1, h));
    std::int64_t lo = std::numeric_limits<std::int64_t>::max();
    std::int64_t hi = std::numeric_limits<std::int64_t>::min();
    for (int b = 0; b < bands; ++b) {
        const int y0 = b * h / bands;
        const int y1 = (b + 1) * h / bands;
        // This band's exposure window, and how much of it each display state covers.
        const double t0 = t_start + ct.readout_s * (0.5 * (y0 + y1)) / static_cast<double>(h);
        const double t1 = t0 + std::max(ct.exposure_s, 1e-9);
        const auto k0 = static_cast<std::int64_t>(std::floor(t0 * fd));
        const auto k1 = static_cast<std::int64_t>(std::floor(t1 * fd - 1e-12));
        for (std::int64_t k = k0; k <= k1; ++k) {
            const double a = std::max(t0, static_cast<double>(k) / fd);
            const double z = std::min(t1, static_cast<double>(k + 1) / fd);
            const double weight = (z - a) / (t1 - t0);
            if (weight <= 0.0) continue;
            lo = std::min(lo, k);
            hi = std::max(hi, k);
            const CellMatrix& cells = state(ctx, k);
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                          static_cast<std::size_t>(x);
                    double v = 0.0;
                    if (Shade(cells, inv.value(), cfg, x, y, &v)) hits[i] = 1;
                    radiance[i] += weight * v;
                }
            }
        }
    }
    if (states_hit != nullptr) *states_hit = hi >= lo ? static_cast<int>(hi - lo + 1) : 0;
    return Finish(radiance, hits, cfg);
}

Image8 RenderView(const CellMatrix& cells, const GridGeometry& g,
                  const OpticalRenderConfig& cfg, Homography* used_transform) {
    const auto quad = QuadFromView(cfg.view, g);
    const auto corners = GridCorners(g);
    auto h = Homography::FromCorrespondences(std::span<const Point2, 4>(corners),
                                             std::span<const Point2, 4>(quad));
    if (!h.ok()) {
        if (used_transform != nullptr) *used_transform = Homography::Identity();
        return Image8(cfg.view.image_width, cfg.view.image_height, cfg.background);
    }
    if (used_transform != nullptr) *used_transform = h.value();
    return RenderOptical(cells, h.value(), cfg);
}

}  // namespace fileflow::sim
