#include "camera.h"

#include <qrcodegen.hpp>

extern "C" {
#include <quirc.h>
}

#include <algorithm>
#include <cmath>

namespace fileflow::desktop {

bool DrawQr(SDL_Renderer* r, const std::string& text, float x, float y, float side) {
    std::optional<qrcodegen::QrCode> qr;
    try {
        // Error correction L: the code goes screen to camera, nothing smudges it, and the lowest
        // level gives the biggest modules -- as on the phone.
        qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
    } catch (const qrcodegen::data_too_long&) {
        return false;
    }
    const int n = qr->getSize();
    constexpr int kMargin = 3;  // modules of white round it, so a camera finds its edge
    const int module = std::max(1, static_cast<int>(side) / (n + 2 * kMargin));
    const int total = module * (n + 2 * kMargin);
    const float ox = std::round(x + (side - static_cast<float>(total)) / 2);
    const float oy = std::round(y + (side - static_cast<float>(total)) / 2);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    const SDL_FRect bg{ox, oy, static_cast<float>(total), static_cast<float>(total)};
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    std::vector<SDL_FRect> cells;
    for (int my = 0; my < n; ++my) {
        for (int mx = 0; mx < n; ++mx) {
            if (!qr->getModule(mx, my)) continue;
            cells.push_back({ox + static_cast<float>((mx + kMargin) * module), oy + static_cast<float>((my + kMargin) * module),
                             static_cast<float>(module), static_cast<float>(module)});
        }
    }
    SDL_RenderFillRects(r, cells.data(), static_cast<int>(cells.size()));
    return true;
}

std::optional<std::string> ReadQr(const std::uint8_t* luma, int w, int h) {
    static quirc* q = quirc_new();
    if (q == nullptr || luma == nullptr || w <= 0 || h <= 0 || w > 4096 || h > 4096) return std::nullopt;
    if (quirc_resize(q, w, h) < 0) return std::nullopt;
    int qw = 0;
    int qh = 0;
    std::uint8_t* buf = quirc_begin(q, &qw, &qh);
    std::copy(luma, luma + static_cast<std::ptrdiff_t>(w) * h, buf);
    quirc_end(q);
    for (int i = 0; i < quirc_count(q); ++i) {
        quirc_code code;
        quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) {
            // Seen in a mirror -- or through a camera that flips -- the code reads the other way.
            quirc_flip(&code);
            err = quirc_decode(&code, &data);
        }
        if (err == QUIRC_SUCCESS) {
            return std::string(reinterpret_cast<const char*>(data.payload), static_cast<std::size_t>(data.payload_len));
        }
    }
    return std::nullopt;
}

Camera::~Camera() { Close(); }

void Camera::Open() {
    if (camera_ != nullptr) return;
    if (!SDL_WasInit(SDL_INIT_CAMERA)) SDL_InitSubSystem(SDL_INIT_CAMERA);
    int n = 0;
    SDL_CameraID* ids = SDL_GetCameras(&n);
    if (ids == nullptr || n == 0) {
        none_ = true;
        SDL_free(ids);
        return;
    }
    SDL_CameraID pick = ids[0];
    for (int i = 0; i < n; ++i) {
        if (SDL_GetCameraPosition(ids[i]) == SDL_CAMERA_POSITION_FRONT_FACING) {
            pick = ids[i];
            break;
        }
    }
    SDL_free(ids);
    // The best-resolution format offered: resolution is how far away a phone can be read from.
    int nf = 0;
    SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(pick, &nf);
    const SDL_CameraSpec* best = nullptr;
    for (int i = 0; i < nf; ++i) {
        const SDL_CameraSpec* s = specs[i];
        const float fps = s->framerate_denominator > 0
                              ? static_cast<float>(s->framerate_numerator) / static_cast<float>(s->framerate_denominator)
                              : 0;
        if (s->width > 1920 || fps < 24) continue;
        if (best == nullptr || s->width * s->height > best->width * best->height) best = s;
    }
    camera_ = SDL_OpenCamera(pick, best);
    SDL_free(specs);
    none_ = camera_ == nullptr;
}

void Camera::Close() {
    if (camera_ != nullptr) SDL_CloseCamera(camera_);
    camera_ = nullptr;
    if (preview_ != nullptr) SDL_DestroyTexture(preview_);
    preview_ = nullptr;
    w_ = h_ = 0;
}

Camera::State Camera::state() const {
    if (none_) return State::kNone;
    if (camera_ == nullptr) return State::kOff;
    const int p = SDL_GetCameraPermissionState(camera_);
    if (p < 0) return State::kDenied;
    if (p == 0) return State::kWaiting;
    return w_ > 0 ? State::kOn : State::kWaiting;
}

bool Camera::Poll(SDL_Renderer* r) {
    if (camera_ == nullptr || SDL_GetCameraPermissionState(camera_) <= 0) return false;
    Uint64 ts = 0;
    SDL_Surface* frame = SDL_AcquireCameraFrame(camera_, &ts);
    if (frame == nullptr) return false;
    // One format to read, whatever the camera gives: RGBA, from which luma and chroma follow.
    SDL_Surface* rgba = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
    SDL_ReleaseCameraFrame(camera_, frame);
    if (rgba == nullptr) return false;
    if (rgba->w != w_ || rgba->h != h_ || preview_ == nullptr) {
        w_ = rgba->w & ~1;
        h_ = rgba->h & ~1;
        if (preview_ != nullptr) SDL_DestroyTexture(preview_);
        preview_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, rgba->w, rgba->h);
        SDL_SetTextureScaleMode(preview_, SDL_SCALEMODE_LINEAR);
    }
    SDL_UpdateTexture(preview_, nullptr, rgba->pixels, rgba->pitch);
    luma_.resize(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_));
    u_.resize(static_cast<std::size_t>(w_ / 2) * static_cast<std::size_t>(h_ / 2));
    v_.resize(u_.size());
    const auto* px = static_cast<const std::uint8_t*>(rgba->pixels);
    for (int y = 0; y < h_; ++y) {
        const std::uint8_t* row = px + static_cast<std::ptrdiff_t>(y) * rgba->pitch;
        for (int x = 0; x < w_; ++x) {
            const int R = row[x * 4];
            const int G = row[x * 4 + 1];
            const int B = row[x * 4 + 2];
            // BT.601, the camera's own luma: what the phone's receiver is tuned on.
            luma_[static_cast<std::size_t>(y * w_ + x)] = static_cast<std::uint8_t>((77 * R + 150 * G + 29 * B) >> 8);
            if ((x & 1) == 0 && (y & 1) == 0) {
                const int U = ((-43 * R - 85 * G + 128 * B) >> 8) + 128;
                const int V = ((128 * R - 107 * G - 21 * B) >> 8) + 128;
                const std::size_t at = static_cast<std::size_t>((y / 2) * (w_ / 2) + x / 2);
                u_[at] = static_cast<std::uint8_t>(std::clamp(U, 0, 255));
                v_[at] = static_cast<std::uint8_t>(std::clamp(V, 0, 255));
            }
        }
    }
    SDL_DestroySurface(rgba);
    ts_ = ts;
    return true;
}

}  // namespace fileflow::desktop
