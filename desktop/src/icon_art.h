// FileFlow's canonical icon artwork. Keep exports in assets/ and Android in sync with ff-icon.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace fileflow::desktop::icon {

struct Px {
    float r, g, b, a;
};

inline Px Hex(std::uint32_t c, float a = 1) {
    return {static_cast<float>((c >> 16) & 0xFF) / 255.0F, static_cast<float>((c >> 8) & 0xFF) / 255.0F,
            static_cast<float>(c & 0xFF) / 255.0F, a};
}

// Signed distance from (x, y) to a rounded rectangle centred at (cx, cy): negative inside.
inline float RoundRectDistance(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - hw + r;
    const float qy = std::abs(y - cy) - hh + r;
    const float outside = std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F));
    return outside + std::min(std::max(qx, qy), 0.0F) - r;
}

struct Shape {
    float cx, cy, hw, hh, r;
    Px colour;
    float hole = 0;
    float hole_radius = 0;
};

// One source for the desktop raster icons, Android vectors and web-ready SVG.
inline std::vector<Shape> Shapes(float s = 1) {
    std::vector<Shape> shapes;
    // The 6 x 6 frame: three corner markers (ring, dot), the rest coloured cells.
    constexpr int kCells = 6;
    const float pad = 196 * s;
    const float pitch = (1024 * s - 2 * pad) / kCells;
    const float gap = pitch * 0.12F;
    const Px colours[] = {Hex(0x7DD3F0), Hex(0xA8E6A1), Hex(0xE1E6E8), Hex(0x3A5560), Hex(0x7DD3F0, 0.45F), Hex(0xB6C9CF)};
    const int pattern[kCells][kCells] = {
        {9, 9, 0, 2, 9, 9}, {9, 9, 3, 1, 9, 9}, {5, 0, 2, 4, 1, 3},
        {1, 3, 0, 5, 0, 2}, {9, 9, 1, 2, 3, 0}, {9, 9, 4, 0, 1, 5},
    };
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            if (pattern[y][x] == 9) continue;
            shapes.push_back({pad + (static_cast<float>(x) + 0.5F) * pitch, pad + (static_cast<float>(y) + 0.5F) * pitch,
                              pitch / 2 - gap, pitch / 2 - gap, pitch * 0.22F, colours[pattern[y][x]]});
        }
    }
    const float m = pitch - gap;  // a marker spans two cells
    for (const auto& [mx, my] : std::array<std::pair<int, int>, 3>{{{0, 0}, {4, 0}, {0, 4}}}) {
        const float cx = pad + (static_cast<float>(mx) + 1) * pitch;
        const float cy = pad + (static_cast<float>(my) + 1) * pitch;
        shapes.push_back({cx, cy, m, m, m * 0.42F, Hex(0xE1E6E8), m * 0.70F, m * 0.30F});
        shapes.push_back({cx, cy, m * 0.36F, m * 0.36F, m * 0.14F, Hex(0xE1E6E8)});
    }

    return shapes;
}

// The icon at [n] px, 4 x 4 samples a pixel; RGBA, row-major.
inline std::vector<std::uint8_t> Draw(int n) {
    const float s = static_cast<float>(n) / 1024.0F;  // drawn on a 1024 canvas
    // macOS's grid: the body is 824 of 1024, corners about a fifth of that.
    const float body = 824 * s;
    const float c = 512 * s;
    const auto shapes = Shapes(s);

    std::vector<std::uint8_t> out(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * 4);
    constexpr int kSamples = 4;
    for (int py = 0; py < n; ++py) {
        for (int px = 0; px < n; ++px) {
            Px acc{0, 0, 0, 0};
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float x = static_cast<float>(px) + (static_cast<float>(sx) + 0.5F) / kSamples;
                    const float y = static_cast<float>(py) + (static_cast<float>(sy) + 0.5F) / kSamples;
                    if (RoundRectDistance(x, y, c, c, body / 2, body / 2, body * 0.225F) > 0) continue;
                    // The tile: a deep teal, lighter at the top -- lit from above.
                    const float t = (y - (c - body / 2)) / body;
                    Px p{0.05F + (0.04F - 0.05F) * t, 0.18F + (0.10F - 0.18F) * t, 0.22F + (0.12F - 0.22F) * t, 1};
                    for (const auto& sh : shapes) {
                        if (RoundRectDistance(x, y, sh.cx, sh.cy, sh.hw, sh.hh, sh.r) <= 0 &&
                            (sh.hole == 0 || RoundRectDistance(x, y, sh.cx, sh.cy,
                                                               sh.hole, sh.hole, sh.hole_radius) > 0)) {
                            p = {p.r + (sh.colour.r - p.r) * sh.colour.a, p.g + (sh.colour.g - p.g) * sh.colour.a,
                                 p.b + (sh.colour.b - p.b) * sh.colour.a, 1};
                        }
                    }
                    acc = {acc.r + p.r, acc.g + p.g, acc.b + p.b, acc.a + 1};
                }
            }
            const auto at = (static_cast<std::size_t>(py) * static_cast<std::size_t>(n) + static_cast<std::size_t>(px)) * 4;
            const float a = acc.a / (kSamples * kSamples);
            // Straight (not premultiplied) alpha, as PNG wants.
            const auto chan = [&](float v) {
                return static_cast<std::uint8_t>(std::lround(std::min(1.0F, acc.a > 0 ? v / acc.a : 0.0F) * 255));
            };
            out[at] = chan(acc.r);
            out[at + 1] = chan(acc.g);
            out[at + 2] = chan(acc.b);
            out[at + 3] = static_cast<std::uint8_t>(std::lround(a * 255));
        }
    }
    return out;
}

}  // namespace fileflow::desktop::icon
