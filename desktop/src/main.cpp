// fileflow-send: FileFlow's sending screen on a computer.
//
//   fileflow-send [--speed steady|fast|fastest] [--compression off|auto|max] [--windowed] FILE...
//
// or start it with no files and drop some on the window. Any phone running FileFlow receives from
// it exactly as from another phone: the frames come from the same core TransmitSession, the file
// travels in the same envelope (core/envelope.h), and each cell is drawn by the same rule as the
// app's shader -- a whole number of screen pixels a side, its low three bits lighting red, green
// and blue. A bigger screen than a phone's means bigger cells, so receivers can stand further back
// and further off to the side: one screen, a room of phones.
//
// On a landscape screen the grid is drawn TURNED a quarter: the same frames, rotated, with cells
// nearly twice the size of the upright grid's on a 16:10 screen (9 px against 5 at 2560x1600). The
// receiver needs nothing for it -- a phone's camera sees the grid sideways anyway, and the detector
// resolves all four rotations from the corner markers.
//
// Keys: 1 2 3 speed (Steady, Fast, Fastest) · F full screen · Esc quit.
//
// --snapshot OUT.bmp saves the screen exactly as drawn once a few frames have been shown, then
// quits: what the receiver's camera would be pointed at, for checking against the decoder.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <fileflow/envelope.h>
#include <fileflow/grid.h>
#include <fileflow/session.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace fileflow;

// The app's Speed.kt, setting for setting: a receiver cannot tell this screen from a phone's.
struct Speed {
    const char* name;
    std::uint32_t cols;
    std::uint32_t rows;
    double states_per_second;
    ModulationProfile profile;
};
constexpr std::array<Speed, 3> kSpeeds = {{
    {"Steady", 120, 260, 20.0, ModulationProfile::kM0BinaryLuminance},
    {"Fast", 120, 260, 30.0, ModulationProfile::kM3FourColor},
    {"Fastest", 120, 260, 30.0, ModulationProfile::kM5EightColor},
}};
constexpr std::size_t kFecNsym = 32;
// What one transfer may carry, as on the phone: a receiver holds the whole file in memory.
constexpr std::uint64_t kMaxFileBytes = 64ULL * 1024 * 1024;
// Present intervals measured before the refresh rate (and so the divisor) is re-derived.
constexpr std::size_t kRemeasureFrames = 60;
// How long the status line stays after sending starts, or after a key or the mouse moves. While
// sending the screen must show NOTHING but the code: the receiver finds the grid by its outer
// ring, and bright text between that ring and the edge of the camera's view is mistaken for it --
// a snapshot with the line showing does not decode at all; without it, every cell is right.
constexpr auto kTextShowsFor = std::chrono::seconds(3);

std::string FormatBytes(std::uint64_t b) {
    char s[32];
    if (b < 1024) {
        std::snprintf(s, sizeof s, "%llu B", static_cast<unsigned long long>(b));
    } else if (b < 1024 * 1024) {
        std::snprintf(s, sizeof s, "%.1f KB", static_cast<double>(b) / 1024.0);
    } else {
        std::snprintf(s, sizeof s, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    }
    return s;
}

// Paths arrive as UTF-8 (SDL_main converts Windows' arguments); the name a receiver saves under is
// the last component.
std::filesystem::path PathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string NameOf(const std::filesystem::path& p) {
    const std::u8string n = p.filename().u8string();
    return {n.begin(), n.end()};
}

// A file packed for sending, or why not.
struct Prepared {
    PackedEnvelope envelope;
    std::string name;     // what the receiver shows: the file's, or "3 files"
    std::string problem;  // non-empty: nothing to send
};

Prepared Prepare(const std::vector<std::string>& paths, CompressionMode mode) {
    Prepared out;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    std::uint64_t total = 0;
    for (const auto& utf8 : paths) {
        const auto path = PathOf(utf8);
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            out.problem = NameOf(path) + " is a folder. Drop the files in it instead.";
            return out;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            out.problem = "Can't read " + NameOf(path) + ".";
            return out;
        }
        total += size;
        if (total > kMaxFileBytes) {
            out.problem = "More than " + FormatBytes(kMaxFileBytes) + " in all: send fewer files at once.";
            return out;
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
            out.problem = "Can't read " + NameOf(path) + ".";
            return out;
        }
        files.emplace_back(NameOf(path), std::move(bytes));
    }
    if (files.empty()) {
        out.problem = "Nothing to send.";
        return out;
    }
    if (files.size() > kEnvelopeMaxBatchFiles) {
        out.problem = "The most one transfer can carry is " + std::to_string(kEnvelopeMaxBatchFiles) + " files.";
        return out;
    }
    if (files.size() == 1 && files[0].second.empty()) {
        out.problem = files[0].first + " is empty.";
        return out;
    }
    out.name = files.size() == 1 ? files[0].first : std::to_string(files.size()) + " files";
    auto packed = files.size() == 1 ? PackEnvelope(files[0].second, out.name, mode)
                                    : PackBatchEnvelope(files, out.name, mode);
    if (!packed.ok()) {
        out.problem = "Can't pack the files: " + std::string(ErrorName(packed.error())) + ".";
        return out;
    }
    out.envelope = std::move(packed).value();
    return out;
}

// One transmit session over a prepared file at one speed.
struct Transfer {
    std::unique_ptr<FrameLayout> layout;
    std::optional<TransmitSession> session;
    CellMatrix cells;
    Speed speed;

    Transfer(std::unique_ptr<FrameLayout> l, const Speed& s)
        : layout(std::move(l)), cells(s.cols, s.rows), speed(s) {}
};

std::unique_ptr<Transfer> Open(const Prepared& p, const Speed& speed, std::string* problem) {
    auto layout = FrameLayout::Create({speed.cols, speed.rows}, LayoutConfig{});
    if (!layout.ok()) {
        *problem = "Layout: " + std::string(ErrorName(layout.error()));
        return nullptr;
    }
    auto t = std::make_unique<Transfer>(std::make_unique<FrameLayout>(std::move(layout).value()), speed);
    TransmitConfig tc;
    tc.fec_nsym = kFecNsym;
    tc.profile = speed.profile;
    tc.content = {.original_size = p.envelope.original_size, .codec = p.envelope.codec,
                  .compress_ms = p.envelope.compress_ms};
    auto s = TransmitSession::Create(*t->layout, p.envelope.bytes, p.name,
                                     SessionIdFor(p.envelope.bytes, p.name), tc);
    if (!s.ok()) {
        *problem = "Transmitter: " + std::string(ErrorName(s.error()));
        return nullptr;
    }
    t->session.emplace(std::move(s).value());
    return t;
}

// The app's fragment shader: a cell's low three bits light red, green and blue. M0's 0 and 255 come
// out black and white by the same rule.
std::uint32_t CellColour(std::uint8_t v) {
    return 0xFF000000U | ((v & 4U) ? 0xFF0000U : 0U) | ((v & 2U) ? 0x00FF00U : 0U) | ((v & 1U) ? 0x0000FFU : 0U);
}

class Sender {
  public:
    Sender(SDL_Window* w, SDL_Renderer* r, std::size_t speed, CompressionMode mode)
        : window_(w), renderer_(r), speed_(speed), mode_(mode) {
        // The display's stated rate until one is measured. Guessing 60 sent a 120 Hz screen's
        // first half second at twice the speed's frame rate -- faster than a 50 fps camera reads.
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w));
        if (m != nullptr && m->refresh_rate > 0.0F) hz_ = static_cast<double>(m->refresh_rate);
        divisor_ = DivisorFor(hz_);
    }

    ~Sender() {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    }

    void Load(std::vector<std::string> paths) {
        if (paths.empty()) return;
        status_ = paths.size() == 1 ? "Preparing " + NameOf(PathOf(paths[0])) + "..."
                                    : "Preparing " + std::to_string(paths.size()) + " files...";
        transfer_.reset();
        // Reading and compressing can take seconds for a big file; the window stays live meanwhile.
        pending_ = std::async(std::launch::async, Prepare, std::move(paths), mode_);
    }

    void SetSpeed(std::size_t i) {
        if (i == speed_ || i >= kSpeeds.size()) return;
        speed_ = i;
        Restart();
    }

    // Brings the status line back for a while.
    void Wake() { text_until_ = std::chrono::steady_clock::now() + kTextShowsFor; }

    // Save the drawn screen to `path` once kSnapshotState frames have been shown, then stop.
    void SnapshotTo(std::string path) { snapshot_ = std::move(path); }
    [[nodiscard]] bool done() const noexcept { return done_; }

    void ToggleFullscreen() {
        const bool full = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreen(window_, !full);
        if (full) SDL_ShowCursor(); else SDL_HideCursor();
    }

    // One vsync: advance the state when it is time, draw, present (which waits for the vsync).
    void Frame() {
        if (pending_.valid() &&
            pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            prepared_ = pending_.get();
            if (prepared_->problem.empty()) {
                Restart();
            } else {
                status_ = prepared_->problem;
                prepared_.reset();
            }
        }
        int w = 0;
        int h = 0;
        SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
        const int text_scale = std::max(1, h / 540);
        const int band = 8 * text_scale * 3;
        if (transfer_) {
            // Whichever way up gives the bigger cells, re-decided as the window changes shape.
            const Speed& s = transfer_->speed;
            const bool turned = Pitch(w, h - band, s.rows, s.cols) > Pitch(w, h - band, s.cols, s.rows);
            if (texture_ == nullptr || turned != turned_) {
                turned_ = turned;
                MakeTexture();
            }
            if (vsyncs_++ % divisor_ == 0) Advance();
        }

        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        const bool text = !transfer_ || std::chrono::steady_clock::now() < text_until_;
        if (transfer_) DrawGrid(w, h - band);
        if (text) DrawText(w, h, text_scale);
        if (!snapshot_.empty() && states_ >= kSnapshotState && !text) Snapshot();
        SDL_RenderPresent(renderer_);
        Measure();
    }

  private:
    // Past the first frames, which carry the manifest: a snapshot shows file data.
    static constexpr std::uint64_t kSnapshotState = 5;

    void Snapshot() {
        SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr);
        if (s == nullptr || !SDL_SaveBMP(s, snapshot_.c_str())) {
            std::fprintf(stderr, "snapshot: %s\n", SDL_GetError());
        } else {
            std::printf("snapshot %dx%d, %d px cells, frame %llu: %s\n", s->w, s->h, pitch_,
                        static_cast<unsigned long long>(states_), snapshot_.c_str());
        }
        SDL_DestroySurface(s);
        snapshot_.clear();
        done_ = true;
    }

    void Restart() {
        if (!prepared_) return;
        std::string problem;
        transfer_ = Open(*prepared_, kSpeeds[speed_], &problem);
        if (!transfer_) {
            status_ = problem;
            return;
        }
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        texture_ = nullptr;  // made on the next frame, the right way up for the window
        const Speed& s = kSpeeds[speed_];
        divisor_ = DivisorFor(hz_);  // a new speed is a new frame rate, now, not after a measurement
        vsyncs_ = 0;
        states_ = 0;
        started_ = std::chrono::steady_clock::now();
        Wake();
        const auto& e = prepared_->envelope;
        status_ = prepared_->name + " - " + FormatBytes(e.original_size) +
                  (e.codec != 0 ? " (" + FormatBytes(e.bytes.size()) + " sent)" : "");
        std::printf("sending %s: %llu bytes as %zu, %s\n", prepared_->name.c_str(),
                    static_cast<unsigned long long>(e.original_size), e.bytes.size(), s.name);
    }

    void MakeTexture() {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        const Speed& s = transfer_->speed;
        tex_w_ = static_cast<int>(turned_ ? s.rows : s.cols);
        tex_h_ = static_cast<int>(turned_ ? s.cols : s.rows);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     tex_w_, tex_h_);
        // Nearest neighbour: every cell a flat square with hard edges, as the phone's GL_NEAREST.
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
        pixels_.assign(static_cast<std::size_t>(s.cols) * s.rows, 0);
        Upload();
    }

    void Advance() {
        auto header = transfer_->session->Next(&transfer_->cells);
        if (!header.ok()) return;
        Upload();
        ++states_;
    }

    // The current cells into the texture, turned a quarter clockwise when the grid is drawn turned:
    // cell (x, y) of a cols x rows grid lands at (rows - 1 - y, x).
    void Upload() {
        const auto& cells = transfer_->cells.data();
        const std::size_t cols = transfer_->speed.cols;
        const std::size_t rows = transfer_->speed.rows;
        for (std::size_t y = 0; y < rows; ++y) {
            for (std::size_t x = 0; x < cols; ++x) {
                const std::size_t to = turned_ ? x * rows + (rows - 1 - y) : y * cols + x;
                pixels_[to] = CellColour(cells[y * cols + x]);
            }
        }
        SDL_UpdateTexture(texture_, nullptr, pixels_.data(), tex_w_ * static_cast<int>(sizeof(std::uint32_t)));
    }

    // Largest whole-pixel cell for a gw x gh grid in w x h with a margin: the phone's rule, so no
    // cell edge ever falls between pixels.
    static int Pitch(int w, int h, std::uint32_t gw, std::uint32_t gh) {
        const int margin = std::max(24, std::min(w, h) / 30);
        return std::max(1, std::min((w - 2 * margin) / static_cast<int>(gw),
                                    (h - 2 * margin) / static_cast<int>(gh)));
    }

    void DrawGrid(int w, int h) {
        const int pitch = Pitch(w, h, static_cast<std::uint32_t>(tex_w_), static_cast<std::uint32_t>(tex_h_));
        const int gw = pitch * tex_w_;
        const int gh = pitch * tex_h_;
        const SDL_FRect dst{static_cast<float>((w - gw) / 2), static_cast<float>((h - gh) / 2),
                            static_cast<float>(gw), static_cast<float>(gh)};
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
        pitch_ = pitch;
    }

    void DrawText(int w, int h, int scale) {
        SDL_SetRenderScale(renderer_, static_cast<float>(scale), static_cast<float>(scale));
        SDL_SetRenderDrawColor(renderer_, 140, 140, 140, 255);
        const float x = 8.0F;
        const float y = static_cast<float>(h / scale - 8 - 6);
        std::string line;
        if (transfer_) {
            const Speed& s = transfer_->speed;
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            char rate[160];
            std::snprintf(rate, sizeof rate, " | %s, %.0f frames/s, %d px cells | 1 2 3 speed  F full screen  Esc quit",
                          s.name, secs > 1.0 ? static_cast<double>(states_) / secs : 0.0, pitch_);
            line = status_ + rate;
        } else {
            line = status_.empty() ? "Drop files here to send them to a phone running FileFlow.  Esc quit"
                                   : status_;
        }
        if (!transfer_) {
            // Centred and alone on an empty screen.
            const auto len = static_cast<float>(line.size() * 8);
            SDL_RenderDebugText(renderer_, std::max(x, (static_cast<float>(w / scale) - len) / 2.0F),
                                static_cast<float>(h / scale / 2), line.c_str());
        } else {
            SDL_RenderDebugText(renderer_, x, y, line.c_str());
        }
        SDL_SetRenderScale(renderer_, 1.0F, 1.0F);
    }

    // The divisor from the MEASURED refresh rate (median present interval), as the phone does:
    // a display's stated rate is often wrong, and variable-rate panels change theirs.
    void Measure() {
        const auto now = std::chrono::steady_clock::now();
        if (last_present_.has_value()) {
            intervals_.push_back(std::chrono::duration<double>(now - *last_present_).count());
        }
        last_present_ = now;
        if (intervals_.size() < kRemeasureFrames) return;
        std::nth_element(intervals_.begin(), intervals_.begin() + static_cast<long>(intervals_.size() / 2),
                         intervals_.end());
        const double median = intervals_[intervals_.size() / 2];
        intervals_.clear();
        if (median <= 0.0) return;
        hz_ = 1.0 / median;
        const auto d = DivisorFor(hz_);
        if (d != divisor_) {
            std::printf("display %.1f Hz: a new frame every %llu refreshes\n", hz_,
                        static_cast<unsigned long long>(d));
            divisor_ = d;
        }
    }

    // Refreshes per frame for the current speed at `hz`.
    [[nodiscard]] std::uint64_t DivisorFor(double hz) const {
        return static_cast<std::uint64_t>(std::max(1L, std::lround(hz / kSpeeds[speed_].states_per_second)));
    }

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    SDL_Texture* texture_ = nullptr;
    std::size_t speed_;
    CompressionMode mode_;
    std::future<Prepared> pending_;
    std::optional<Prepared> prepared_;
    std::unique_ptr<Transfer> transfer_;
    std::vector<std::uint32_t> pixels_;
    bool turned_ = false;  // drawn a quarter turn round, for a landscape screen
    int tex_w_ = 0;
    int tex_h_ = 0;
    std::string status_;
    std::string snapshot_;
    bool done_ = false;
    double hz_ = 60.0;  // the display's refresh rate: stated, then measured
    std::uint64_t divisor_ = 2;
    std::uint64_t vsyncs_ = 0;
    std::uint64_t states_ = 0;
    int pitch_ = 0;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point text_until_;
    std::optional<std::chrono::steady_clock::time_point> last_present_;
    std::vector<double> intervals_;
};

int Usage() {
    std::fprintf(stderr,
                 "usage: fileflow-send [--speed steady|fast|fastest] [--compression off|auto|max] "
                 "[--windowed] [--snapshot OUT.bmp] [FILE...]\n"
                 "With no files, drop them on the window.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t speed = 1;  // Fast: the setting that works on the most receivers
    CompressionMode mode = CompressionMode::kAuto;
    bool windowed = false;
    std::string snapshot;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--speed" && i + 1 < argc) {
            const std::string v = argv[++i];
            speed = v == "steady" ? 0 : v == "fast" ? 1 : v == "fastest" ? 2 : kSpeeds.size();
            if (speed == kSpeeds.size()) return Usage();
        } else if (a == "--compression" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "off") mode = CompressionMode::kOff;
            else if (v == "auto") mode = CompressionMode::kAuto;
            else if (v == "max") mode = CompressionMode::kMaximum;
            else return Usage();
        } else if (a == "--windowed") {
            windowed = true;
        } else if (a == "--snapshot" && i + 1 < argc) {
            snapshot = argv[++i];
        } else if (a == "--help" || a == "-h" || (!a.empty() && a[0] == '-')) {
            return Usage();
        } else {
            files.push_back(a);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (!windowed) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow("FileFlow", 1280, 800, flags);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Presenting waits for the display's refresh: the clock every frame is counted against.
    SDL_SetRenderVSync(renderer, 1);
    if (!windowed) SDL_HideCursor();

    int code = 0;
    {
        Sender sender(window, renderer, speed, mode);
        sender.Load(files);
        if (!snapshot.empty()) sender.SnapshotTo(snapshot);
        std::vector<std::string> dropped;
        bool running = true;
        while (running && !sender.done()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                        sender.Wake();
                        break;
                    case SDL_EVENT_KEY_DOWN:
                        sender.Wake();
                        if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q) running = false;
                        else if (e.key.key == SDLK_1) sender.SetSpeed(0);
                        else if (e.key.key == SDLK_2) sender.SetSpeed(1);
                        else if (e.key.key == SDLK_3) sender.SetSpeed(2);
                        else if (e.key.key == SDLK_F) sender.ToggleFullscreen();
                        break;
                    case SDL_EVENT_DROP_BEGIN:
                        dropped.clear();
                        break;
                    case SDL_EVENT_DROP_FILE:
                        if (e.drop.data != nullptr) dropped.emplace_back(e.drop.data);
                        break;
                    case SDL_EVENT_DROP_COMPLETE:
                        sender.Load(std::move(dropped));
                        dropped.clear();
                        break;
                    default:
                        break;
                }
            }
            sender.Frame();
        }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}
