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
    none_ = false;
    problem_.clear();
    diagnostics_ = {};
    ts_ = 0;
    last_arrival_ns_ = SDL_GetTicksNS();
    checked_devices_ns_ = 0;
    if (!SDL_WasInit(SDL_INIT_CAMERA) && !SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        Fail("The camera could not start. Close other camera apps, then retry.");
        return;
    }
    int n = 0;
    SDL_CameraID* ids = SDL_GetCameras(&n);
    if (ids == nullptr || n == 0) {
        none_ = ids != nullptr && n == 0;
        if (!none_) Fail("The camera list could not be read. Reconnect the camera, then retry.");
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
    // Fit the long optical code, rather than maximizing image area. A square 1552x1552 mode
    // has more pixels than 1920x1080 but gives a 260-cell edge fewer useful samples.
    int nf = 0;
    SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(pick, &nf);
    const SDL_CameraSpec* best = nullptr;
    for (int i = 0; specs != nullptr && i < nf; ++i) {
        const SDL_CameraSpec* s = specs[i];
        if (detail::PreferReceiveFormat(*s, best)) best = s;
    }
    camera_ = SDL_OpenCamera(pick, best);
    camera_id_ = pick;
    SDL_free(specs);
    if (camera_ == nullptr) Fail("The camera could not open. Close other camera apps and check camera access, then retry.");
}

void Camera::Close() {
    if (camera_ != nullptr) SDL_CloseCamera(camera_);
    camera_ = nullptr;
    camera_id_ = 0;
    if (preview_ != nullptr) SDL_DestroyTexture(preview_);
    preview_ = nullptr;
    w_ = h_ = 0;
    ts_ = 0;
    none_ = false;
    problem_.clear();
}

void Camera::Fail(const char* problem) {
    problem_ = problem;
    if (camera_ != nullptr) SDL_CloseCamera(camera_);
    camera_ = nullptr;
}

Camera::State Camera::state() const {
    if (!problem_.empty()) return State::kError;
    if (none_) return State::kNone;
    if (camera_ == nullptr) return State::kOff;
    const int p = SDL_GetCameraPermissionState(camera_);
    if (p < 0) return State::kDenied;
    if (p == 0) return State::kWaiting;
    return w_ > 0 ? State::kOn : State::kWaiting;
}

bool Camera::Poll(SDL_Renderer* r) {
    if (camera_ == nullptr) return false;
    if (SDL_GetCameraPermissionState(camera_) <= 0) {
        last_arrival_ns_ = SDL_GetTicksNS();
        return false;
    }
    const Uint64 arrival = SDL_GetTicksNS();
    // A disconnected SDL camera may continue producing blank frames. Check its identity at a
    // bounded rate rather than treating "no new frame" as a failure or consuming app events.
    if (arrival - checked_devices_ns_ >= 1'000'000'000) {
        checked_devices_ns_ = arrival;
        int count = 0;
        SDL_CameraID* ids = SDL_GetCameras(&count);
        if (ids != nullptr) {
            bool connected = false;
            for (int i = 0; i < count; ++i) connected = connected || ids[i] == camera_id_;
            SDL_free(ids);
            if (!connected) {
                Fail("The camera disconnected. Reconnect it, then retry.");
                return false;
            }
        }
    }
    if (!diagnostics_.have_negotiated) diagnostics_.have_negotiated = SDL_GetCameraFormat(camera_, &diagnostics_.negotiated);
    Uint64 ts = 0;
    SDL_ClearError();
    SDL_Surface* frame = SDL_AcquireCameraFrame(camera_, &ts);
    if (frame == nullptr) {
        if (*SDL_GetError() != '\0') Fail("The camera could not deliver a frame. Close other camera apps, then retry.");
        else if (arrival - last_arrival_ns_ > 10'000'000'000) Fail("The camera is not delivering frames. Check its connection, then retry.");
        return false;
    }
    last_arrival_ns_ = arrival;
    if (diagnostics_.frames > 0 && ts <= ts_) {
        ++diagnostics_.nonmonotonic_timestamps;
        SDL_ReleaseCameraFrame(camera_, frame);
        return false;
    }
    const Uint64 conversion_started = SDL_GetTicksNS();
    // One format to read, whatever the camera gives: RGBA, from which luma and chroma follow.
    SDL_Surface* rgba = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
    SDL_ReleaseCameraFrame(camera_, frame);
    if (rgba == nullptr) {
        Fail("The camera image could not be converted. Check available memory, then retry.");
        return false;
    }
    if (rgba->w != w_ || rgba->h != h_ || preview_ == nullptr) {
        w_ = rgba->w;
        h_ = rgba->h;
        if (preview_ != nullptr) SDL_DestroyTexture(preview_);
        preview_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, rgba->w, rgba->h);
        if (preview_ == nullptr) {
            SDL_DestroySurface(rgba);
            Fail("The camera preview could not start. Check available memory, then retry.");
            return false;
        }
        SDL_SetTextureScaleMode(preview_, SDL_SCALEMODE_LINEAR);
    }
    if (!SDL_UpdateTexture(preview_, nullptr, rgba->pixels, rgba->pitch)) {
        SDL_DestroySurface(rgba);
        Fail("The camera preview stopped updating. Retry receiving.");
        return false;
    }
    detail::RgbaToYuv444(static_cast<const std::uint8_t*>(rgba->pixels), w_, h_, rgba->pitch, &luma_, &u_, &v_);
    SDL_DestroySurface(rgba);
    if (diagnostics_.frames > 0) {
        const double interval_ms = static_cast<double>(ts - ts_) / 1'000'000;
        if (diagnostics_.timestamp_intervals == 0) diagnostics_.interval_ms_min = interval_ms;
        diagnostics_.interval_ms_min = std::min(diagnostics_.interval_ms_min, interval_ms);
        diagnostics_.interval_ms_max = std::max(diagnostics_.interval_ms_max, interval_ms);
        diagnostics_.interval_ms_sum += interval_ms;
        ++diagnostics_.timestamp_intervals;
    }
    const double conversion_ms = static_cast<double>(SDL_GetTicksNS() - conversion_started) / 1'000'000;
    diagnostics_.conversion_ms_sum += conversion_ms;
    diagnostics_.conversion_ms_max = std::max(diagnostics_.conversion_ms_max, conversion_ms);
    ++diagnostics_.frames;
    ts_ = ts;
    return true;
}

double detail::ReceiveCellPitch(const SDL_CameraSpec& mode) {
    if (mode.width <= 0 || mode.height <= 0 || mode.width > 1920 || mode.height > 1920 ||
        mode.framerate_denominator <= 0 || mode.framerate_numerator <= 0 ||
        static_cast<double>(mode.framerate_numerator) / mode.framerate_denominator < 24.0) return -1.0;
    const double width = mode.width;
    const double height = mode.height;
    return std::max(std::min(width / 120.0, height / 260.0),
                    std::min(width / 260.0, height / 120.0));
}

bool detail::PreferReceiveFormat(const SDL_CameraSpec& candidate, const SDL_CameraSpec* current) {
    const double pitch = ReceiveCellPitch(candidate);
    if (pitch < 0) return false;
    if (current == nullptr) return true;
    const double old_pitch = ReceiveCellPitch(*current);
    if (pitch != old_pitch) return pitch > old_pitch;
    return static_cast<double>(candidate.framerate_numerator) / candidate.framerate_denominator >
           static_cast<double>(current->framerate_numerator) / current->framerate_denominator;
}

void detail::RgbaToYuv444(const std::uint8_t* pixels, int width, int height, int pitch,
                         std::vector<std::uint8_t>* luma, std::vector<std::uint8_t>* u, std::vector<std::uint8_t>* v) {
    luma->resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    u->resize(luma->size());
    v->resize(luma->size());
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = pixels + static_cast<std::ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const int R = row[x * 4];
            const int G = row[x * 4 + 1];
            const int B = row[x * 4 + 2];
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            // BT.601 with the existing full-range RGB coefficients.
            (*luma)[at] = static_cast<std::uint8_t>((77 * R + 150 * G + 29 * B) >> 8);
            (*u)[at] = static_cast<std::uint8_t>(std::clamp(((-43 * R - 85 * G + 128 * B) >> 8) + 128, 0, 255));
            (*v)[at] = static_cast<std::uint8_t>(std::clamp(((128 * R - 107 * G - 21 * B) >> 8) + 128, 0, 255));
        }
    }
}

}  // namespace fileflow::desktop
