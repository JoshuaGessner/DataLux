#include "paint.h"

#include "fonts.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fileflow::desktop {

namespace {

constexpr int kAtlasSide = 1024;

// The characters baked: Latin-1 and the punctuation the app's text uses.
const std::vector<int>& Codepoints() {
    static const std::vector<int> cps = [] {
        std::vector<int> v;
        for (int c = 32; c < 127; ++c) v.push_back(c);
        for (int c = 160; c < 256; ++c) v.push_back(c);
        for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x203A, 0x2192, 0x2248,
                      0x2318, 0x21E7, 0x2325}) {
            v.push_back(c);
        }
        return v;
    }();
    return cps;
}

// One UTF-8 code point from [s] at [i], advancing [i]; 0xFFFD for a malformed byte.
int NextCodepoint(std::string_view s, std::size_t* i) {
    const auto b0 = static_cast<unsigned char>(s[(*i)++]);
    if (b0 < 0x80) return b0;
    int n = b0 >= 0xF0 ? 3 : b0 >= 0xE0 ? 2 : b0 >= 0xC0 ? 1 : -1;
    if (n < 0) return 0xFFFD;
    int cp = b0 & (0x3F >> n);
    while (n-- > 0) {
        if (*i >= s.size()) return 0xFFFD;
        const auto b = static_cast<unsigned char>(s[*i]);
        if ((b & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (b & 0x3F);
        ++*i;
    }
    return cp;
}

}  // namespace

Rgba Mix(Rgba a, Rgba b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

struct Painter::Face {
    stbtt_fontinfo info{};
    const unsigned char* data;
    explicit Face(const unsigned char* d) : data(d) { stbtt_InitFont(&info, d, stbtt_GetFontOffsetForIndex(d, 0)); }
};

struct Painter::Atlas {
    SDL_Texture* texture = nullptr;
    std::vector<stbtt_packedchar> chars;
    std::map<int, int> index;  // code point -> chars[]
    float ascent = 0;          // pixels
    ~Atlas() {
        if (texture != nullptr) SDL_DestroyTexture(texture);
    }
};

Painter::Painter(SDL_Renderer* r)
    : r_(r), regular_(std::make_unique<Face>(kInterRegular)), semibold_(std::make_unique<Face>(kInterSemiBold)) {
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
    // The glow: a white disc whose alpha falls off smoothly to nothing at the edge, tinted per use.
    constexpr int kSide = 128;
    std::vector<std::uint32_t> px(kSide * kSide);
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const float dx = (static_cast<float>(x) + 0.5F) / kSide * 2 - 1;
            const float dy = (static_cast<float>(y) + 0.5F) / kSide * 2 - 1;
            const float d = std::min(1.0F, std::sqrt(dx * dx + dy * dy));
            const float a = (1 - d) * (1 - d);
            px[static_cast<std::size_t>(y * kSide + x)] =
                (static_cast<std::uint32_t>(a * 255.0F + 0.5F) << 24) | 0xFFFFFFU;
        }
    }
    glow_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kSide, kSide);
    SDL_UpdateTexture(glow_, nullptr, px.data(), kSide * 4);
    SDL_SetTextureBlendMode(glow_, SDL_BLENDMODE_BLEND);
}

Painter::~Painter() {
    atlases_.clear();
    if (glow_ != nullptr) SDL_DestroyTexture(glow_);
}

void Painter::SetScale(float pixels_per_point) {
    if (pixels_per_point <= 0 || pixels_per_point == scale_) return;
    scale_ = pixels_per_point;
    atlases_.clear();  // baked for the old density
}

Painter::Atlas& Painter::AtlasFor(Weight w, float size) {
    const int px = std::max(6, static_cast<int>(std::lround(size * scale_)));
    const auto key = std::make_pair(static_cast<int>(w), px);
    auto it = atlases_.find(key);
    if (it != atlases_.end()) return *it->second;

    auto atlas = std::make_unique<Atlas>();
    const Face& face = w == Weight::kSemiBold ? *semibold_ : *regular_;
    const auto& cps = Codepoints();
    atlas->chars.resize(cps.size());
    std::vector<unsigned char> alpha(kAtlasSide * kAtlasSide);
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, alpha.data(), kAtlasSide, kAtlasSide, 0, 1, nullptr);
    stbtt_PackSetOversampling(&pc, 2, 1);  // horizontal oversampling: crisper at small sizes
    stbtt_pack_range range{};
    range.font_size = static_cast<float>(px);
    range.array_of_unicode_codepoints = const_cast<int*>(cps.data());
    range.num_chars = static_cast<int>(cps.size());
    range.chardata_for_range = atlas->chars.data();
    stbtt_PackFontRanges(&pc, face.data, 0, &range, 1);
    stbtt_PackEnd(&pc);
    for (std::size_t i = 0; i < cps.size(); ++i) atlas->index[cps[i]] = static_cast<int>(i);

    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&face.info, &ascent, &descent, &gap);
    atlas->ascent = static_cast<float>(ascent) * stbtt_ScaleForPixelHeight(&face.info, static_cast<float>(px));

    std::vector<std::uint32_t> rgba(alpha.size());
    for (std::size_t i = 0; i < alpha.size(); ++i) rgba[i] = (static_cast<std::uint32_t>(alpha[i]) << 24) | 0xFFFFFFU;
    atlas->texture = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kAtlasSide, kAtlasSide);
    SDL_UpdateTexture(atlas->texture, nullptr, rgba.data(), kAtlasSide * 4);
    SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas->texture, SDL_SCALEMODE_LINEAR);
    auto& ref = *atlas;
    atlases_[key] = std::move(atlas);
    return ref;
}

float Painter::Measure(std::string_view text, float size, Weight w) {
    Atlas& a = AtlasFor(w, size);
    float x = 0;
    float y = 0;
    for (std::size_t i = 0; i < text.size();) {
        const int cp = NextCodepoint(text, &i);
        auto it = a.index.find(cp);
        if (it == a.index.end()) it = a.index.find('?');
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(a.chars.data(), kAtlasSide, kAtlasSide, it->second, &x, &y, &q, 0);
    }
    return x / scale_;
}

float Painter::Text(std::string_view text, float x, float y, float size, Weight w, Rgba c, Align align) {
    Atlas& a = AtlasFor(w, size);
    const float width = Measure(text, size, w);
    float left = x;
    if (align == Align::kCenter) left = x - width / 2;
    if (align == Align::kRight) left = x - width;
    // Baseline: the line box's top, half its leading, then the ascent -- on whole pixels, so the
    // glyphs aren't smeared across two rows.
    float px = std::round(left * scale_);
    float py = std::round(y * scale_ + (LineHeight(size) - size) * scale_ / 2 + a.ascent);
    SDL_SetTextureColorModFloat(a.texture, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(a.texture, c.a);
    for (std::size_t i = 0; i < text.size();) {
        const int cp = NextCodepoint(text, &i);
        auto it = a.index.find(cp);
        if (it == a.index.end()) it = a.index.find('?');
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(a.chars.data(), kAtlasSide, kAtlasSide, it->second, &px, &py, &q, 0);
        const SDL_FRect src{q.s0 * kAtlasSide, q.t0 * kAtlasSide, (q.s1 - q.s0) * kAtlasSide, (q.t1 - q.t0) * kAtlasSide};
        const SDL_FRect dst{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        if (dst.w > 0 && dst.h > 0) SDL_RenderTexture(r_, a.texture, &src, &dst);
    }
    return width;
}

std::string Painter::Fit(std::string_view text, float max_width, float size, Weight w) {
    if (Measure(text, size, w) <= max_width) return std::string(text);
    // Keep the end (the extension) and as much of the start as fits, cutting on character bounds.
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < text.size();) {
        starts.push_back(i);
        NextCodepoint(text, &i);
    }
    const std::size_t n = starts.size();
    const std::size_t keep_end = std::min<std::size_t>(8, n / 3);
    for (std::size_t keep_start = n - keep_end; keep_start-- > 1;) {
        const std::string s = std::string(text.substr(0, starts[keep_start])) + "…" +
                              std::string(text.substr(starts[n - keep_end]));
        if (Measure(s, size, w) <= max_width || keep_start == 1) return s;
    }
    return "…";
}

void Painter::Clear(Rgba c) {
    SDL_SetRenderDrawColorFloat(r_, c.r, c.g, c.b, c.a);
    SDL_RenderClear(r_);
}

void Painter::Rect(SDL_FRect r, Rgba c) {
    SDL_SetRenderDrawColorFloat(r_, c.r, c.g, c.b, c.a);
    const SDL_FRect p{r.x * scale_, r.y * scale_, r.w * scale_, r.h * scale_};
    SDL_RenderFillRect(r_, &p);
}

// A convex shape from its outline (pixels, in order) and each point's outward normal: a fan of
// triangles at full colour, and round its edge a strip fading to nothing a pixel out.
void Painter::Fill(const std::vector<SDL_FPoint>& ring, const std::vector<SDL_FPoint>& normals, Rgba c) {
    const std::size_t n = ring.size();
    if (n < 3) return;
    SDL_FPoint centre{0, 0};
    for (const auto& p : ring) {
        centre.x += p.x / static_cast<float>(n);
        centre.y += p.y / static_cast<float>(n);
    }
    const SDL_FColor solid{c.r, c.g, c.b, c.a};
    const SDL_FColor clear{c.r, c.g, c.b, 0};
    std::vector<SDL_Vertex> v;
    v.reserve(1 + 2 * n);
    v.push_back({centre, solid, {0, 0}});
    for (std::size_t i = 0; i < n; ++i) {
        // Half a pixel in at full colour, half a pixel out at none: the edge straddles the outline.
        v.push_back({{ring[i].x - normals[i].x * 0.5F, ring[i].y - normals[i].y * 0.5F}, solid, {0, 0}});
        v.push_back({{ring[i].x + normals[i].x * 0.5F, ring[i].y + normals[i].y * 0.5F}, clear, {0, 0}});
    }
    std::vector<int> idx;
    idx.reserve(n * 9);
    for (std::size_t i = 0; i < n; ++i) {
        const int a = static_cast<int>(1 + 2 * i);
        const int b = static_cast<int>(1 + 2 * ((i + 1) % n));
        idx.insert(idx.end(), {0, a, b, a, a + 1, b + 1, a, b + 1, b});
    }
    SDL_RenderGeometry(r_, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

void Painter::RoundRect(SDL_FRect rect, float radius, Rgba c) {
    const float s = scale_;
    const float x0 = rect.x * s;
    const float y0 = rect.y * s;
    const float x1 = (rect.x + rect.w) * s;
    const float y1 = (rect.y + rect.h) * s;
    const float r = std::clamp(radius * s, 0.0F, std::min(x1 - x0, y1 - y0) / 2);
    const int seg = std::clamp(static_cast<int>(std::ceil(r / 2)), 2, 24);
    std::vector<SDL_FPoint> ring;
    std::vector<SDL_FPoint> normals;
    const SDL_FPoint centres[4] = {{x1 - r, y0 + r}, {x1 - r, y1 - r}, {x0 + r, y1 - r}, {x0 + r, y0 + r}};
    constexpr float kPi = std::numbers::pi_v<float>;
    for (int corner = 0; corner < 4; ++corner) {
        const float a0 = -kPi / 2 + static_cast<float>(corner) * kPi / 2;
        for (int i = 0; i <= seg; ++i) {
            const float a = a0 + kPi / 2 * static_cast<float>(i) / static_cast<float>(seg);
            const SDL_FPoint nrm{std::cos(a), std::sin(a)};
            ring.push_back({centres[corner].x + nrm.x * r, centres[corner].y + nrm.y * r});
            normals.push_back(nrm);
        }
    }
    Fill(ring, normals, c);
}

void Painter::RoundStroke(SDL_FRect r, float radius, float width, Rgba c) {
    // An outline as a ring of quads between the shape and the shape inset by [width].
    const float s = scale_;
    std::vector<SDL_FPoint> outer;
    std::vector<SDL_FPoint> inner;
    const float x0 = r.x * s;
    const float y0 = r.y * s;
    const float x1 = (r.x + r.w) * s;
    const float y1 = (r.y + r.h) * s;
    const float rad = std::clamp(radius * s, 0.0F, std::min(x1 - x0, y1 - y0) / 2);
    const float wpx = width * s;
    const int seg = std::clamp(static_cast<int>(std::ceil(rad / 2)), 2, 24);
    const SDL_FPoint centres[4] = {{x1 - rad, y0 + rad}, {x1 - rad, y1 - rad}, {x0 + rad, y1 - rad}, {x0 + rad, y0 + rad}};
    constexpr float kPi = std::numbers::pi_v<float>;
    for (int corner = 0; corner < 4; ++corner) {
        const float a0 = -kPi / 2 + static_cast<float>(corner) * kPi / 2;
        for (int i = 0; i <= seg; ++i) {
            const float a = a0 + kPi / 2 * static_cast<float>(i) / static_cast<float>(seg);
            const float cx = std::cos(a);
            const float cy = std::sin(a);
            outer.push_back({centres[corner].x + cx * (rad + 0.5F), centres[corner].y + cy * (rad + 0.5F)});
            inner.push_back({centres[corner].x + cx * std::max(0.0F, rad - wpx), centres[corner].y + cy * std::max(0.0F, rad - wpx)});
        }
    }
    const SDL_FColor col{c.r, c.g, c.b, c.a};
    std::vector<SDL_Vertex> v;
    for (std::size_t i = 0; i < outer.size(); ++i) {
        v.push_back({outer[i], col, {0, 0}});
        v.push_back({inner[i], col, {0, 0}});
    }
    std::vector<int> idx;
    const int n = static_cast<int>(outer.size());
    for (int i = 0; i < n; ++i) {
        const int a = 2 * i;
        const int b = 2 * ((i + 1) % n);
        idx.insert(idx.end(), {a, a + 1, b, b, a + 1, b + 1});
    }
    SDL_RenderGeometry(r_, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

void Painter::Circle(float cx, float cy, float radius, Rgba c) {
    const float s = scale_;
    const float r = radius * s;
    const int seg = std::clamp(static_cast<int>(r), 12, 96);
    std::vector<SDL_FPoint> ring;
    std::vector<SDL_FPoint> normals;
    for (int i = 0; i < seg; ++i) {
        const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(seg);
        const SDL_FPoint nrm{std::cos(a), std::sin(a)};
        ring.push_back({cx * s + nrm.x * r, cy * s + nrm.y * r});
        normals.push_back(nrm);
    }
    Fill(ring, normals, c);
}

void Painter::Polygon(const std::vector<SDL_FPoint>& points, Rgba c) {
    const std::size_t n = points.size();
    if (n < 3) return;
    // Each corner's normal: the two edges' outward normals, averaged. Which way is out depends on
    // the winding, so it's read from the polygon's signed area.
    float area = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    const float out = area > 0 ? 1.0F : -1.0F;
    std::vector<SDL_FPoint> ring;
    std::vector<SDL_FPoint> normals;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& prev = points[(i + n - 1) % n];
        const auto& cur = points[i];
        const auto& next = points[(i + 1) % n];
        const auto edge = [&](SDL_FPoint a, SDL_FPoint b) {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float len = std::max(1e-6F, std::sqrt(dx * dx + dy * dy));
            return SDL_FPoint{out * dy / len, -out * dx / len};
        };
        const SDL_FPoint n1 = edge(prev, cur);
        const SDL_FPoint n2 = edge(cur, next);
        SDL_FPoint nm{n1.x + n2.x, n1.y + n2.y};
        const float len = std::max(1e-6F, std::sqrt(nm.x * nm.x + nm.y * nm.y));
        ring.push_back({cur.x * scale_, cur.y * scale_});
        normals.push_back({nm.x / len, nm.y / len});
    }
    Fill(ring, normals, c);
}

void Painter::Glow(float cx, float cy, float radius, Rgba c) {
    SDL_SetTextureColorModFloat(glow_, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(glow_, c.a);
    const SDL_FRect dst{(cx - radius) * scale_, (cy - radius) * scale_, 2 * radius * scale_, 2 * radius * scale_};
    SDL_RenderTexture(r_, glow_, nullptr, &dst);
}

void Painter::Arc(float cx, float cy, float radius, float thickness, float start, float sweep, Rgba c) {
    const float s = scale_;
    const int seg = std::max(8, static_cast<int>(std::abs(sweep) * radius * s / 4));
    const SDL_FColor col{c.r, c.g, c.b, c.a};
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const float ro = (radius + thickness / 2) * s;
    const float ri = (radius - thickness / 2) * s;
    for (int i = 0; i <= seg; ++i) {
        const float a = start + sweep * static_cast<float>(i) / static_cast<float>(seg);
        v.push_back({{cx * s + std::cos(a) * ro, cy * s + std::sin(a) * ro}, col, {0, 0}});
        v.push_back({{cx * s + std::cos(a) * ri, cy * s + std::sin(a) * ri}, col, {0, 0}});
        if (i > 0) {
            const int b = 2 * i;
            idx.insert(idx.end(), {b - 2, b - 1, b, b, b - 1, b + 1});
        }
    }
    SDL_RenderGeometry(r_, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
    Circle(cx + std::cos(start) * radius, cy + std::sin(start) * radius, thickness / 2, c);
    Circle(cx + std::cos(start + sweep) * radius, cy + std::sin(start + sweep) * radius, thickness / 2, c);
}

}  // namespace fileflow::desktop
