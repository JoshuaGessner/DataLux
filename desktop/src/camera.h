// The computer's camera, for pairing and receiving: frames as luma (and colour, for receiving),
// a mirrored preview, and QR codes read from it.
//
// SDL3's camera API on all three systems. On macOS the first use asks the person (FileFlow.app's
// Info.plist says why); until they answer, and if they say no, the camera is "waiting" or "denied"
// and the screen says so.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fileflow::desktop {

// QR codes: drawn crisp (whole pixels a module, white margin) and read from a grey image.
// [side] is in pixels; the code is centred in it. Returns false when [text] won't fit a code.
bool DrawQr(SDL_Renderer* r, const std::string& text, float x, float y, float side);
std::optional<std::string> ReadQr(const std::uint8_t* luma, int w, int h);

class Camera {
  public:
    enum class State { kOff, kNone, kWaiting, kDenied, kOn };

    Camera() = default;
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // Opens the camera facing the person (a laptop's built-in one) or else the first.
    void Open();
    void Close();
    [[nodiscard]] State state() const;

    // Takes the newest frame, if a new one has come: true then. Its luma, colour planes and the
    // preview texture are updated from it.
    bool Poll(SDL_Renderer* r);

    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }
    [[nodiscard]] const std::vector<std::uint8_t>& luma() const { return luma_; }
    // Chroma, half resolution each way, a plane each -- as the decoder takes it.
    [[nodiscard]] const std::vector<std::uint8_t>& u() const { return u_; }
    [[nodiscard]] const std::vector<std::uint8_t>& v() const { return v_; }
    [[nodiscard]] std::uint64_t timestamp_ns() const { return ts_; }
    [[nodiscard]] SDL_Texture* preview() const { return preview_; }

  private:
    SDL_Camera* camera_ = nullptr;
    SDL_Texture* preview_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    std::uint64_t ts_ = 0;
    std::vector<std::uint8_t> luma_;
    std::vector<std::uint8_t> u_;
    std::vector<std::uint8_t> v_;
    bool none_ = false;
};

}  // namespace fileflow::desktop
