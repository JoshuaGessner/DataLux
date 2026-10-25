// Drawing for the desktop app: Inter text, anti-aliased rounded shapes, soft glows, arcs.
//
// SDL's renderer draws textures, rectangles and triangles, which is all a clean UI needs: text is
// glyphs from an atlas stb_truetype bakes at the display's own pixel density (so it is sharp on a
// Retina screen and a plain one alike), and every rounded shape is a triangle fan with a feathered
// edge a pixel wide, which is what anti-aliasing amounts to. Everything here is in POINTS; the
// painter scales to pixels. The optical frame itself is not drawn through this -- it must be
// pixel-exact -- but straight to the renderer.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fileflow::desktop {

struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};

constexpr Rgba Hex(std::uint32_t rgb, float a = 1.0F) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0F, static_cast<float>((rgb >> 8) & 0xFF) / 255.0F,
            static_cast<float>(rgb & 0xFF) / 255.0F, a};
}
Rgba Mix(Rgba a, Rgba b, float t);
constexpr Rgba WithAlpha(Rgba c, float a) { return {c.r, c.g, c.b, c.a * a}; }

// The app's own dark theme (app/src/main/res/values/colors.xml), so the two read as one product.
namespace theme {
inline constexpr Rgba kBackground = Hex(0x0F1416);
inline constexpr Rgba kSurface = Hex(0x1A2023);
inline constexpr Rgba kSurfaceHigh = Hex(0x242B2E);
inline constexpr Rgba kOutline = Hex(0x3A4448);
inline constexpr Rgba kText = Hex(0xE1E6E8);
inline constexpr Rgba kTextDim = Hex(0x9AA5AA);
inline constexpr Rgba kPrimary = Hex(0x7DD3F0);
inline constexpr Rgba kOnPrimary = Hex(0x003544);
inline constexpr Rgba kPrimaryContainer = Hex(0x004D61);
inline constexpr Rgba kTertiary = Hex(0xA8E6A1);
inline constexpr Rgba kError = Hex(0xFF8A80);
}  // namespace theme

enum class Weight { kRegular, kSemiBold };
enum class Align { kLeft, kCenter, kRight };

class Painter {
  public:
    explicit Painter(SDL_Renderer* r);
    ~Painter();
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    // Pixels per point, from the window's display; glyphs are re-baked when it changes.
    void SetScale(float pixels_per_point);
    [[nodiscard]] float scale() const noexcept { return scale_; }
    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return r_; }

    void Clear(Rgba c);
    void Rect(SDL_FRect r, Rgba c);
    void RoundRect(SDL_FRect r, float radius, Rgba c);
    // A rounded outline [width] wide, drawn inside [r].
    void RoundStroke(SDL_FRect r, float radius, float width, Rgba c);
    void Circle(float cx, float cy, float radius, Rgba c);
    // Soft light, brightest at the centre and gone at [radius].
    void Glow(float cx, float cy, float radius, Rgba c);
    // A convex polygon through [points] (in order, in points), its edge anti-aliased.
    void Polygon(const std::vector<SDL_FPoint>& points, Rgba c);
    // An arc [thickness] wide, from [start] through [sweep] radians, with round ends.
    void Arc(float cx, float cy, float radius, float thickness, float start, float sweep, Rgba c);

    // Draws UTF-8 [text] with its line box's top at [y]; returns its width. A character Inter
    // lacks is drawn as "?".
    float Text(std::string_view text, float x, float y, float size, Weight w, Rgba c, Align a = Align::kLeft);
    float Measure(std::string_view text, float size, Weight w);
    [[nodiscard]] float LineHeight(float size) const { return size * 1.3F; }
    // [text] cut with "…" in the middle to fit [max_width]: a file name keeps its extension.
    std::string Fit(std::string_view text, float max_width, float size, Weight w);

  private:
    struct Face;
    struct Atlas;
    Atlas& AtlasFor(Weight w, float size);
    void Fill(const std::vector<SDL_FPoint>& ring, const std::vector<SDL_FPoint>& normals, Rgba c);

    SDL_Renderer* r_;
    float scale_ = 1.0F;
    std::unique_ptr<Face> regular_;
    std::unique_ptr<Face> semibold_;
    std::map<std::pair<int, int>, std::unique_ptr<Atlas>> atlases_;  // (weight, pixel size)
    SDL_Texture* glow_ = nullptr;
};

}  // namespace fileflow::desktop
