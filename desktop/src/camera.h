// The computer's camera, for pairing and receiving: frames as luma (and colour, for receiving),
// a mirrored preview, and QR codes read from it.
//
// SDL3's camera API on all three systems. On macOS the first use asks the person (FileFlow.app's
// Info.plist says why); until they answer, and if they say no, the camera is "waiting" or "denied"
// and the screen says so.
#pragma once

#include <SDL3/SDL.h>

#include <fileflow/geometry.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fileflow::desktop {

// QR codes: drawn crisp (whole pixels a module, white margin) and read from a grey image.
// [side] is in pixels; the code is centred in it. Returns false when [text] won't fit a code.
bool DrawQr(SDL_Renderer* r, const std::string& text, float x, float y, float side);
std::optional<std::string> ReadQr(const std::uint8_t* luma, int w, int h);

namespace detail {
// Usable pixels per cell for the stock 120x260 code, in either orientation; -1 rejects a mode.
double ReceiveCellPitch(const SDL_CameraSpec& mode);
bool PreferReceiveFormat(const SDL_CameraSpec& candidate, const SDL_CameraSpec* current);
// Extract co-sited planes from a valid RGBA32 surface, reusing the output vectors' storage.
void RgbaToYuv444(const std::uint8_t* pixels, int width, int height, int pitch,
                  std::vector<std::uint8_t>* luma, std::vector<std::uint8_t>* u, std::vector<std::uint8_t>* v);
std::array<SDL_FPoint, 4> MirrorCameraOutline(const std::array<Point2, 4>& corners, SDL_FRect preview);
}

class Camera {
  public:
    enum class State { kOff, kNone, kWaiting, kDenied, kOn, kError };
    struct Device {
        SDL_CameraID id;
        std::string name;
    };

    struct Diagnostics {
        SDL_CameraSpec negotiated{};  // SDL's delivered format; not necessarily native sensor data
        bool have_negotiated = false;
        std::uint64_t frames = 0;
        std::uint64_t timestamp_intervals = 0;
        std::uint64_t nonmonotonic_timestamps = 0;
        double interval_ms_sum = 0;
        double interval_ms_min = 0;
        double interval_ms_max = 0;
        double conversion_ms_sum = 0;  // conversion, plane extraction and preview upload
        double conversion_ms_max = 0;
    };

    Camera() = default;
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // An ID is valid only while its camera is connected. Zero chooses the built-in or first camera.
    static std::vector<Device> Devices();
    void Open(SDL_CameraID preferred = 0);
    void Close();
    [[nodiscard]] State state() const;

    // Takes the newest frame, if a new one has come: true then. Its luma, colour planes and the
    // preview texture are updated from it.
    bool Poll(SDL_Renderer* r);

    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }
    [[nodiscard]] const std::vector<std::uint8_t>& luma() const { return luma_; }
    // Co-sited chroma from the RGB surface, the same size as luma. This retains RGB detail;
    // it does not recover detail a native camera YUV conversion has already lost.
    [[nodiscard]] const std::vector<std::uint8_t>& u() const { return u_; }
    [[nodiscard]] const std::vector<std::uint8_t>& v() const { return v_; }
    [[nodiscard]] std::uint64_t timestamp_ns() const { return ts_; }
    [[nodiscard]] SDL_Texture* preview() const { return preview_; }
    [[nodiscard]] const Diagnostics& diagnostics() const { return diagnostics_; }
    [[nodiscard]] const std::string& problem() const { return problem_; }

  private:
    SDL_Camera* camera_ = nullptr;
    SDL_CameraID camera_id_ = 0;
    SDL_Texture* preview_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    std::uint64_t ts_ = 0;
    std::vector<std::uint8_t> luma_;
    std::vector<std::uint8_t> u_;
    std::vector<std::uint8_t> v_;
    bool none_ = false;
    std::string problem_;
    Diagnostics diagnostics_;
    std::uint64_t last_arrival_ns_ = 0;
    std::uint64_t checked_devices_ns_ = 0;
    void Fail(const char* problem);
};

}  // namespace fileflow::desktop
