// ff-icon: draws FileFlow's icon -- the light grid on a dark tile -- at every size the desktop
// platforms ask for, as PNGs, plus a Windows .ico of them. Run by the build; no image library,
// no checked-in binaries.
//
//   ff-icon ICONSET_DIR ICO_PATH PNG_PATH
//
// writes ICONSET_DIR/icon_16x16.png ... icon_512x512@2x.png (for macOS's iconutil), a .ico holding
// 16, 32, 48 and 256 px images, and one 512 px PNG (for Linux).
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Px {
    float r, g, b, a;
};

Px Hex(std::uint32_t c, float a = 1) {
    return {static_cast<float>((c >> 16) & 0xFF) / 255.0F, static_cast<float>((c >> 8) & 0xFF) / 255.0F,
            static_cast<float>(c & 0xFF) / 255.0F, a};
}

// Signed distance from (x, y) to a rounded rectangle centred at (cx, cy): negative inside.
float RoundRectDistance(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - hw + r;
    const float qy = std::abs(y - cy) - hh + r;
    const float outside = std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F));
    return outside + std::min(std::max(qx, qy), 0.0F) - r;
}

// The icon at [n] px, 4 x 4 samples a pixel; RGBA, row-major.
std::vector<std::uint8_t> Draw(int n) {
    const float s = static_cast<float>(n) / 1024.0F;  // drawn on a 1024 canvas
    // macOS's grid: the body is 824 of 1024, corners about a fifth of that.
    const float body = 824 * s;
    const float c = 512 * s;
    struct Shape {
        float cx, cy, hw, hh, r;
        Px colour;
    };
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
        shapes.push_back({cx, cy, m, m, m * 0.42F, Hex(0xE1E6E8)});
        shapes.push_back({cx, cy, m * 0.70F, m * 0.70F, m * 0.30F, Hex(0x0E2730)});
        shapes.push_back({cx, cy, m * 0.36F, m * 0.36F, m * 0.14F, Hex(0xE1E6E8)});
    }

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
                        if (RoundRectDistance(x, y, sh.cx, sh.cy, sh.hw, sh.hh, sh.r) <= 0) {
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

std::uint32_t Crc(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}

void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// A PNG of RGBA pixels, its image data a zlib stream of stored (uncompressed) blocks: no deflate
// needed, and an icon's few kilobytes don't care.
std::vector<std::uint8_t> Png(const std::vector<std::uint8_t>& rgba, int n) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < n; ++y) {
        raw.push_back(0);  // filter: none
        const auto row = rgba.begin() + static_cast<std::ptrdiff_t>(y) * n * 4;
        raw.insert(raw.end(), row, row + n * 4);
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    for (std::size_t i = 0; i < raw.size(); i += 65535) {
        const auto len = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - i));
        z.push_back(i + len >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(len));
        z.push_back(static_cast<std::uint8_t>(len >> 8));
        z.push_back(static_cast<std::uint8_t>(~len));
        z.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(~len) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(i), raw.begin() + static_cast<std::ptrdiff_t>(i + len));
    }
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    Put32(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        Put32(png, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        png.insert(png.end(), body.begin(), body.end());
        Put32(png, Crc(body.data(), body.size()));
    };
    std::vector<std::uint8_t> ihdr;
    Put32(ihdr, static_cast<std::uint32_t>(n));
    Put32(ihdr, static_cast<std::uint32_t>(n));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

bool Write(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: ff-icon ICONSET_DIR ICO_PATH PNG_PATH\n");
        return 2;
    }
    const std::filesystem::path iconset = argv[1];
    std::filesystem::create_directories(iconset);
    bool ok = true;
    for (int size : {16, 32, 128, 256, 512}) {
        ok &= Write(iconset / ("icon_" + std::to_string(size) + "x" + std::to_string(size) + ".png"), Png(Draw(size), size));
        ok &= Write(iconset / ("icon_" + std::to_string(size) + "x" + std::to_string(size) + "@2x.png"),
                    Png(Draw(size * 2), size * 2));
    }
    // .ico: a directory of PNG images (Windows Vista and later read PNG entries).
    std::vector<std::vector<std::uint8_t>> images;
    const int sizes[] = {16, 32, 48, 256};
    for (int s : sizes) images.push_back(Png(Draw(s), s));
    std::vector<std::uint8_t> ico = {0, 0, 1, 0, static_cast<std::uint8_t>(images.size()), 0};
    std::uint32_t offset = 6 + 16 * static_cast<std::uint32_t>(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        const auto side = static_cast<std::uint8_t>(sizes[i] >= 256 ? 0 : sizes[i]);
        ico.insert(ico.end(), {side, side, 0, 0, 1, 0, 32, 0});
        const auto size = static_cast<std::uint32_t>(images[i].size());
        for (int k = 0; k < 4; ++k) ico.push_back(static_cast<std::uint8_t>(size >> (8 * k)));
        for (int k = 0; k < 4; ++k) ico.push_back(static_cast<std::uint8_t>(offset >> (8 * k)));
        offset += size;
    }
    for (const auto& img : images) ico.insert(ico.end(), img.begin(), img.end());
    ok &= Write(argv[2], ico);
    ok &= Write(argv[3], Png(Draw(512), 512));
    if (!ok) std::fprintf(stderr, "ff-icon: could not write the icons\n");
    return ok ? 0 : 1;
}
