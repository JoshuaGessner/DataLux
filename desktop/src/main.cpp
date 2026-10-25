// FileFlow for the desktop: a laptop, monitor, TV or projector as the sending screen.
//
//   fileflow-send [--speed steady|fast|fastest] [--compression off|auto|max] [--windowed]
//                 [--window WxH] [--seconds N] [--snapshot OUT.bmp [--snapshot-after S]] [FILE...]
//
// Started with files, it sends them at once, full screen. Started without, it opens its home
// screen: drop files on it, or choose them, and pick the speed and compression (remembered). Any
// phone running FileFlow receives from it exactly as from another phone: the frames come from the
// same core TransmitSession, the file travels in the same envelope (core/envelope.h), and each cell
// is drawn by the same rule as the app's shader -- a whole number of screen pixels a side, its low
// three bits lighting red, green and blue. A bigger screen than a phone's means bigger cells, so
// receivers can stand further back and further off to the side: one screen, a room of phones.
//
// On a landscape screen the grid is drawn TURNED a quarter: the same frames, rotated, with cells
// nearly twice the size of the upright grid's on a 16:10 screen (9 px against 5 at 2560x1600). The
// receiver needs nothing for it -- a phone's camera sees the grid sideways anyway, and the detector
// resolves all four rotations from the corner markers.
//
// Keys: Ctrl/Cmd+O choose files · 1 2 3 speed · F full screen · Esc stop sending.
//
// --snapshot OUT.bmp saves the screen exactly as drawn -- once the controls have faded, while
// sending -- then quits: what a receiver's camera would be pointed at, for checking against the
// decoder. --snapshot-after S takes it S seconds in instead, whatever is showing.
#include "paint.h"
#include "transfer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace fileflow;
using namespace fileflow::desktop;
using Clock = std::chrono::steady_clock;
namespace th = fileflow::desktop::theme;

// Present intervals measured before the refresh rate is re-derived.
constexpr std::size_t kRemeasureFrames = 30;
// How long the controls stay over the code after sending starts, or after a key or the mouse
// moves. While sending the screen must show NOTHING but the code once they've gone: the receiver
// finds the grid by its outer ring, and bright text between that ring and the edge of the camera's
// view is mistaken for it -- a snapshot with a status line showing did not decode at all; without
// it, every cell was right.
constexpr auto kControlsShowFor = std::chrono::seconds(3);

constexpr std::array<const char*, 3> kCompressionNames = {"Off", "Auto", "Maximum"};
constexpr std::array<const char*, 3> kCompressionHints = {
    "Files go exactly as they are.",
    "Documents and text shrink first; photos go as they are.",
    "Smallest transfer, longer to prepare.",
};

CompressionMode ModeOf(std::size_t i) {
    return i == 0 ? CompressionMode::kOff : i == 2 ? CompressionMode::kMaximum : CompressionMode::kAuto;
}

// The two settings, remembered between runs in the app's own preferences folder.
struct Settings {
    std::size_t speed = 1;        // Fast: the setting that works on the most receivers
    std::size_t compression = 1;  // Auto

    static std::string Path() {
        char* dir = SDL_GetPrefPath("FileFlow", "Desktop");
        if (dir == nullptr) return {};
        std::string p = std::string(dir) + "settings.txt";
        SDL_free(dir);
        return p;
    }
    static Settings Load() {
        Settings s;
        std::ifstream in(Path());
        std::string key;
        std::size_t v = 0;
        while (in >> key >> v) {
            if (key == "speed" && v < kSpeeds.size()) s.speed = v;
            if (key == "compression" && v < kCompressionNames.size()) s.compression = v;
        }
        return s;
    }
    void Save() const {
        std::ofstream out(Path());
        out << "speed " << speed << "\ncompression " << compression << "\n";
    }
};

// What was sent lately, newest first, so it can be sent again with one click: a file that went to one
// phone often needs to go to the next. Kept beside the settings; a path that has since gone is said
// so when it's clicked, not hidden.
struct Recent {
    std::string name;
    std::uint64_t bytes = 0;
    std::int64_t when = 0;  // seconds since the epoch
    std::vector<std::string> paths;

    static constexpr std::size_t kKeep = 3;

    static std::string Path() {
        char* dir = SDL_GetPrefPath("FileFlow", "Desktop");
        if (dir == nullptr) return {};
        std::string p = std::string(dir) + "recent.txt";
        SDL_free(dir);
        return p;
    }
    // One entry per block: "name<TAB>bytes<TAB>when", then a line per path, then a blank line.
    static std::vector<Recent> Load() {
        std::vector<Recent> out;
        std::ifstream in(Path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            Recent r;
            const auto a = line.find('\t');
            const auto b = line.find('\t', a == std::string::npos ? a : a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            r.name = line.substr(0, a);
            r.bytes = std::strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
            r.when = std::strtoll(line.substr(b + 1).c_str(), nullptr, 10);
            while (std::getline(in, line) && !line.empty()) r.paths.push_back(line);
            if (!r.paths.empty()) out.push_back(std::move(r));
        }
        if (out.size() > kKeep) out.resize(kKeep);
        return out;
    }
    static void Save(const std::vector<Recent>& list) {
        std::ofstream out(Path());
        for (const auto& r : list) {
            out << r.name << '\t' << r.bytes << '\t' << r.when << '\n';
            for (const auto& p : r.paths) out << p << '\n';
            out << '\n';
        }
    }
};

std::string Ago(std::int64_t when) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto s = std::max<std::int64_t>(0, now - when);
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + " h ago";
    const auto d = s / 86400;
    return d == 1 ? "yesterday" : std::to_string(d) + " days ago";
}

// "⌘V" on a Mac, "Ctrl+V" elsewhere: shortcuts said the way the keyboard says them.
std::string Shortcut(const char* key) {
    return std::string(SDL_strcmp(SDL_GetPlatform(), "macOS") == 0 ? "\u2318" : "Ctrl+") + key;
}

// Eases a value toward a target, frame by frame: the same feel at any frame rate.
struct Ease {
    float value = 0;
    void To(float target, float dt, float seconds = 0.12F) {
        value += (target - value) * (1 - std::exp(-dt / seconds));
        if (std::abs(target - value) < 0.001F) value = target;
    }
};

struct Pointer {
    float x = -1;
    float y = -1;
    bool down = false;
    bool pressed = false;   // went down this frame
    bool released = false;  // came up this frame
    [[nodiscard]] bool In(SDL_FRect r) const { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
};

float Smooth(float x) { return x * x * (3 - 2 * x); }

std::uint32_t Hash(int a, int b) {
    auto h = static_cast<std::uint32_t>(a) * 374761393U + static_cast<std::uint32_t>(b) * 668265263U;
    h = (h ^ (h >> 13)) * 1274126177U;
    return h ^ (h >> 16);
}

// The light grid: FileFlow's mark, as on the phone's Send screen -- a tile of cells each easing
// between the theme's colours on its own slow beat, with the real frame's three corner markers.
void LightGrid(Painter& p, float cx, float cy, float side, float t, float lift) {
    constexpr int kCells = 6;
    const Rgba palette[] = {th::kPrimary, th::kTertiary, Hex(0xB6C9CF), th::kText,
                            WithAlpha(th::kPrimary, 0.28F), WithAlpha(th::kText, 0.1F)};
    constexpr int kColours = 6;
    p.Glow(cx, cy, side * (0.95F + 0.05F * std::sin(t * 1.6F)) * (1 + 0.15F * lift), WithAlpha(th::kPrimary, 0.30F + 0.2F * lift));
    const SDL_FRect tile{cx - side / 2, cy - side / 2, side, side};
    p.RoundRect(tile, side * 0.18F, th::kSurfaceHigh);
    p.RoundStroke(tile, side * 0.18F, 1, WithAlpha(th::kPrimary, 0.35F + 0.4F * lift));
    const float pad = side * 0.13F;
    const float pitch = (side - 2 * pad) / kCells;
    const float gap = pitch * 0.14F;
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            const bool corner = (x < 2 && y < 2) || (x >= kCells - 2 && y < 2) || (x < 2 && y >= kCells - 2);
            if (corner) continue;
            const int id = y * kCells + x;
            const float period = 0.9F + 1.4F * static_cast<float>(Hash(id, -1) & 0xFFFF) / 65536.0F;
            const float phase = t * (1 + lift) / period + static_cast<float>(Hash(id, -2) & 0xFFFF) / 65536.0F * 7;
            const int step = static_cast<int>(std::floor(phase));
            const float f = phase - static_cast<float>(step);
            const Rgba from = palette[Hash(id, step) % kColours];
            const Rgba to = palette[Hash(id, step + 1) % kColours];
            const SDL_FRect cell{tile.x + pad + static_cast<float>(x) * pitch + gap, tile.y + pad + static_cast<float>(y) * pitch + gap,
                                 pitch - 2 * gap, pitch - 2 * gap};
            p.RoundRect(cell, pitch * 0.22F, Mix(from, to, Smooth(std::clamp((f - 0.7F) / 0.3F, 0.0F, 1.0F))));
        }
    }
    const float m = 2 * pitch - 2 * gap;
    for (const auto& [mx, my] : {std::pair{0, 0}, std::pair{kCells - 2, 0}, std::pair{0, kCells - 2}}) {
        const SDL_FRect ring{tile.x + pad + static_cast<float>(mx) * pitch + gap, tile.y + pad + static_cast<float>(my) * pitch + gap, m, m};
        p.RoundRect(ring, m * 0.24F, th::kText);
        const float w = m * 0.16F;
        p.RoundRect({ring.x + w, ring.y + w, m - 2 * w, m - 2 * w}, m * 0.16F, th::kSurfaceHigh);
        const float d = m * 0.34F;
        p.RoundRect({ring.x + d, ring.y + d, m - 2 * d, m - 2 * d}, m * 0.08F, th::kText);
    }
}

class App {
  public:
    struct Options {
        bool windowed = false;
        std::string snapshot;
        double snapshot_after = 0;
    };

    App(SDL_Window* w, SDL_Renderer* r, Settings settings, Options options)
        : window_(w), renderer_(r), paint_(r), settings_(settings), options_(std::move(options)) {
        // The display's stated rate until one is measured.
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w));
        if (m != nullptr && m->refresh_rate > 0.0F) hz_ = static_cast<double>(m->refresh_rate);
        began_ = last_frame_ = Clock::now();
        recent_ = Recent::Load();
    }

    // The clipboard's text, sent as a note: the quickest way to get a link or an address from a
    // computer onto a phone with no network between them.
    void SendClipboard() {
        if (screen_ != Screen::kHome) return;
        char* text = SDL_GetClipboardText();
        std::string s = text != nullptr ? text : "";
        SDL_free(text);
        if (s.empty()) {
            problem_ = "The clipboard has no text to send. Copy some first.";
            return;
        }
        preparing_name_ = "the clipboard";
        transfer_.reset();
        screen_ = Screen::kPreparing;
        problem_.clear();
        sending_paths_.clear();  // nothing to send again from: the clipboard will have moved on
        pending_ = std::async(std::launch::async, PrepareText, std::move(s), std::string("Clipboard.txt"),
                              ModeOf(settings_.compression));
    }

    ~App() {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    }

    [[nodiscard]] bool done() const noexcept { return done_; }

    void Send(std::vector<std::string> paths) {
        if (paths.empty() || screen_ == Screen::kPreparing) return;
        preparing_name_ = paths.size() == 1 ? NameOf(PathOf(paths[0])) : std::to_string(paths.size()) + " files";
        transfer_.reset();
        screen_ = Screen::kPreparing;
        problem_.clear();
        sending_paths_ = paths;
        // Reading and compressing can take seconds for a big file; the window stays live meanwhile.
        pending_ = std::async(std::launch::async, Prepare, std::move(paths), ModeOf(settings_.compression));
    }

    void Event(const SDL_Event& e) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                done_ = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                pointer_.x = e.motion.x;
                pointer_.y = e.motion.y;
                Wake();
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    pointer_.down = true;
                    pointer_.pressed = true;
                }
                Wake();
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    pointer_.down = false;
                    pointer_.released = true;
                }
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                pointer_.x = pointer_.y = -1;
                break;
            case SDL_EVENT_KEY_DOWN: {
                Wake();
                const bool command = (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
                if (e.key.key == SDLK_ESCAPE && screen_ == Screen::kSending) Stop();
                else if (e.key.key == SDLK_O && command && screen_ == Screen::kHome) Choose();
                else if (e.key.key == SDLK_V && command && screen_ == Screen::kHome) SendClipboard();
                else if (e.key.key == SDLK_1) SetSpeed(0);
                else if (e.key.key == SDLK_2) SetSpeed(1);
                else if (e.key.key == SDLK_3) SetSpeed(2);
                else if (e.key.key == SDLK_F) SDL_SetWindowFullscreen(window_, !Fullscreen());
                break;
            }
            case SDL_EVENT_DROP_BEGIN:
                dropped_.clear();
                break;
            case SDL_EVENT_DROP_POSITION:
                drag_seen_ = Clock::now();
                break;
            case SDL_EVENT_DROP_FILE:
                if (e.drop.data != nullptr) dropped_.emplace_back(e.drop.data);
                break;
            case SDL_EVENT_DROP_COMPLETE:
                drag_seen_.reset();
                if (!dropped_.empty()) Send(std::move(dropped_));
                dropped_.clear();
                break;
            default:
                break;
        }
    }

    // One vsync: take in what happened, advance the code when it's due, draw, present.
    void Frame() {
        const auto now = Clock::now();
        dt_ = std::min(0.1F, std::chrono::duration<float>(now - last_frame_).count());
        last_frame_ = now;
        t_ = std::chrono::duration<float>(now - began_).count();
        TakeChosenFiles();
        TakePrepared();

        int pw = 0;
        int ph = 0;
        int ww = 0;
        int wh = 0;
        SDL_GetCurrentRenderOutputSize(renderer_, &pw, &ph);
        SDL_GetWindowSize(window_, &ww, &wh);
        if (ww > 0) paint_.SetScale(static_cast<float>(pw) / static_cast<float>(ww));
        const float W = static_cast<float>(ww);
        const float H = static_cast<float>(wh);

        if (screen_ == Screen::kSending) {
            const Speed& s = transfer_->speed;
            // Whichever way up gives the bigger cells, re-decided as the window changes shape.
            const bool turned = Pitch(pw, ph, s.rows, s.cols) > Pitch(pw, ph, s.cols, s.rows);
            if (texture_ == nullptr || turned != turned_) {
                turned_ = turned;
                MakeTexture();
            }
            Pace(s.states_per_second);
            paint_.Clear({0, 0, 0, 1});
            DrawGrid(pw, ph);
            controls_.To(now < controls_until_ ? 1.0F : 0.0F, dt_, 0.15F);
            if (controls_.value > 0.01F) SendingControls(W, H, controls_.value);
            if (controls_.value > 0.01F) SDL_ShowCursor(); else SDL_HideCursor();
        } else {
            Home(W, H);
        }
        MaybeSnapshot();
        pointer_.pressed = pointer_.released = false;
        SDL_RenderPresent(renderer_);
        Measure();
    }

  private:
    enum class Screen { kHome, kPreparing, kSending };

    // ------------------------------------------------------------------------------- the home screen

    void Home(float W, float H) {
        paint_.Clear(th::kBackground);
        hot_ = false;
        const float margin = W < 600 ? 20 : 32;

        // Header: the mark, the name, what it does.
        LightGrid(paint_, margin + 14, 40, 28, t_ * 0.6F, 0);
        paint_.Text("FileFlow", margin + 38, 27, 20, Weight::kSemiBold, th::kText);
        if (W > 560) paint_.Text("Files, sent with light", W - margin, 30, 14, Weight::kRegular, th::kTextDim, Align::kRight);

        // The drop zone.
        const float cw = std::min(W - 2 * margin, 760.0F);
        const float recent_h = recent_.empty() ? 0.0F : 36.0F + 52.0F * static_cast<float>(recent_.size());
        const float ch = std::clamp(H - 360 - recent_h, 260.0F, 400.0F);
        const SDL_FRect card{(W - cw) / 2, 84, cw, ch};
        const bool dragging = drag_seen_.has_value() && Clock::now() - *drag_seen_ < std::chrono::milliseconds(250);
        drag_.To(dragging ? 1.0F : 0.0F, dt_);
        const float lift = drag_.value;
        if (lift > 0.01F) paint_.Glow(card.x + cw / 2, card.y + ch / 2, cw * 0.7F, WithAlpha(th::kPrimary, 0.18F * lift));
        paint_.RoundRect(card, 28, Mix(th::kSurface, th::kSurfaceHigh, lift * 0.6F));
        paint_.RoundStroke(card, 28, 1.5F + lift, Mix(th::kOutline, th::kPrimary, lift));

        const bool preparing = screen_ == Screen::kPreparing;
        // The card's contents as one block, centred: the mark takes what the words and buttons
        // leave, so a short card shrinks the mark rather than piling the rest on top of each other.
        constexpr float kBelowMark = 22 + 36 + 20 + 16 + 46 + 12 + 20;  // headline, line, button, link
        const float side = std::clamp(ch - kBelowMark - 48, 64.0F, 150.0F);
        const float top = card.y + (ch - side - kBelowMark) / 2;
        if (preparing) {
            // Reading and compressing: a turning arc where the mark was.
            const float cx = card.x + cw / 2;
            const float cy = top + side / 2;
            paint_.Glow(cx, cy, side * 0.9F, WithAlpha(th::kPrimary, 0.25F));
            paint_.Arc(cx, cy, side * 0.32F, 6, 0, 2 * std::numbers::pi_v<float>, WithAlpha(th::kText, 0.08F));
            const float spin = t_ * 4.2F;
            paint_.Arc(cx, cy, side * 0.32F, 6, spin, 1.6F + 0.8F * std::sin(t_ * 2.1F), th::kPrimary);
            const float ty = top + side + 22;
            paint_.Text(paint_.Fit("Preparing " + preparing_name_ + "…", cw - 64, 22, Weight::kSemiBold),
                        card.x + cw / 2, ty, 22, Weight::kSemiBold, th::kText, Align::kCenter);
            paint_.Text(settings_.compression == 0 ? "Reading the files" : "Reading and compressing",
                        card.x + cw / 2, ty + 34, 14, Weight::kRegular, th::kTextDim, Align::kCenter);
        } else {
            LightGrid(paint_, card.x + cw / 2, top + side / 2, side, t_, lift);
            const float ty = top + side + 22;
            paint_.Text(dragging ? "Let go to send" : "Drop files here to send", card.x + cw / 2, ty, 24,
                        Weight::kSemiBold, th::kText, Align::kCenter);
            paint_.Text("Several go as one transfer · up to " + FormatBytes(kMaxFileBytes) + " in all",
                        card.x + cw / 2, ty + 36, 14, Weight::kRegular, th::kTextDim, Align::kCenter);
            const float bw = 200;
            const SDL_FRect choose{card.x + (cw - bw) / 2, ty + 72, bw, 46};
            if (Button("choose", choose, "Choose files…", true)) Choose();
            if (Link("paste", card.x + cw / 2, choose.y + 58, "or send the clipboard's text  " + Shortcut("V"))) {
                SendClipboard();
            }
        }

        // The settings, under the drop zone: side by side when there's room, stacked when not.
        const float sy = card.y + ch + 28;
        const bool stacked = cw < 620;
        const float colw = stacked ? cw : (cw - 24) / 2;
        Setting("speed", card.x, sy, colw, "Speed", {kSpeeds[0].name, kSpeeds[1].name, kSpeeds[2].name},
                &settings_.speed, kSpeeds[settings_.speed].hint);
        Setting("compression", stacked ? card.x : card.x + colw + 24, stacked ? sy + 104 : sy, colw, "Compression",
                {kCompressionNames[0], kCompressionNames[1], kCompressionNames[2]}, &settings_.compression,
                kCompressionHints[settings_.compression]);

        // What was sent lately, to send again: as many as fit above the bottom line.
        float bottom = sy + (stacked ? 196 : 98);
        const float footer_y = H - 40;
        const float limit = H - 16;
        if (!recent_.empty() && bottom + 22 + 26 + 46 <= limit) {
            float ry = bottom + 22;
            paint_.Text("Recent", card.x + 4, ry, 13, Weight::kSemiBold, th::kTextDim);
            ry += 26;
            for (std::size_t i = 0; i < recent_.size() && ry + 46 <= limit; ++i) {
                const Recent& r = recent_[i];
                const SDL_FRect row{card.x, ry, cw, 46};
                const std::string id = "recent/" + std::to_string(i);
                const bool over = pointer_.In(row);
                hot_ |= over;
                Ease& hover = anim_[id];
                hover.To(over ? 1.0F : 0.0F, dt_);
                paint_.RoundRect(row, 14, Mix(th::kSurface, th::kSurfaceHigh, hover.value));
                // A small document, folded at the corner.
                paint_.RoundRect({row.x + 16, row.y + 12, 17, 22}, 3, WithAlpha(th::kPrimary, 0.85F));
                paint_.RoundRect({row.x + 26, row.y + 12, 7, 7}, 1.5F, Mix(th::kSurface, th::kSurfaceHigh, hover.value));
                paint_.Text(paint_.Fit(r.name, cw * 0.5F, 14, Weight::kSemiBold), row.x + 46, row.y + 4, 14,
                            Weight::kSemiBold, th::kText);
                paint_.Text(FormatBytes(r.bytes) + " \u00B7 " + Ago(r.when), row.x + 46, row.y + 24, 12,
                            Weight::kRegular, th::kTextDim);
                paint_.Text("Send again", row.x + cw - 18, row.y + 14, 13, Weight::kSemiBold,
                            Mix(th::kTextDim, th::kPrimary, hover.value), Align::kRight);
                if (over && pointer_.released) SendAgain(r);
                ry += 52;
            }
            bottom = ry - 6;
        }

        // A problem with the last files, until something else happens.
        if (!problem_.empty()) {
            const float tw = paint_.Measure(problem_, 14, Weight::kRegular) + 56;
            const SDL_FRect toast{(W - tw) / 2, H - 104, tw, 40};
            paint_.RoundRect(toast, 20, th::kSurfaceHigh);
            paint_.Circle(toast.x + 22, toast.y + 20, 4, th::kError);
            paint_.Text(problem_, toast.x + 36, toast.y + 11, 14, Weight::kRegular, th::kText);
        }

        // The one line of instruction, where there's room for it.
        const char* footer = "Point a phone running FileFlow at this screen. No network, no pairing, nothing to install.";
        if (bottom + 20 <= footer_y && paint_.Measure(footer, 13, Weight::kRegular) <= W - 2 * margin) {
            paint_.Text(footer, W / 2, footer_y, 13, Weight::kRegular, th::kTextDim, Align::kCenter);
        }
        SDL_SetCursor(SDL_GetDefaultCursor());
        if (hot_) SDL_SetCursor(HandCursor());
    }

    void Setting(const char* id, float x, float y, float w, const char* label, std::array<const char*, 3> options,
                 std::size_t* value, const char* hint) {
        paint_.Text(label, x + 4, y, 13, Weight::kSemiBold, th::kTextDim);
        const std::size_t before = *value;
        *value = Segmented(id, {x, y + 24, w, 42}, options, *value);
        if (*value != before) settings_.Save();
        paint_.Text(paint_.Fit(hint, w - 8, 13, Weight::kRegular), x + 4, y + 74, 13, Weight::kRegular, th::kTextDim);
    }

    // ---------------------------------------------------------------------------------- widgets

    SDL_Cursor* HandCursor() {
        if (hand_ == nullptr) hand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        return hand_;
    }

    // A pill button: lightens under the pointer, sinks while pressed. True when clicked.
    bool Button(const char* id, SDL_FRect r, const std::string& label, bool filled) {
        const bool over = pointer_.In(r);
        hot_ |= over;
        Ease& hover = anim_[id];
        Ease& press = anim_[std::string(id) + "/press"];
        hover.To(over ? 1.0F : 0.0F, dt_);
        press.To(over && pointer_.down ? 1.0F : 0.0F, dt_, 0.05F);
        const float k = 1 - 0.03F * press.value;
        const SDL_FRect d{r.x + r.w * (1 - k) / 2, r.y + r.h * (1 - k) / 2, r.w * k, r.h * k};
        if (filled) {
            paint_.Glow(d.x + d.w / 2, d.y + d.h / 2, d.w * 0.7F, WithAlpha(th::kPrimary, 0.12F * hover.value));
            paint_.RoundRect(d, d.h / 2, Mix(th::kPrimary, th::kText, 0.25F * hover.value));
        } else {
            paint_.RoundRect(d, d.h / 2, Mix(th::kSurfaceHigh, th::kOutline, hover.value));
        }
        paint_.Text(label, d.x + d.w / 2, d.y + (d.h - paint_.LineHeight(15)) / 2, 15, Weight::kSemiBold,
                    filled ? th::kOnPrimary : th::kText, Align::kCenter);
        return over && pointer_.released;
    }

    // A line of text that works as a button: brightens under the pointer. True when clicked.
    bool Link(const char* id, float cx, float y, const std::string& label) {
        const float w = paint_.Measure(label, 13, Weight::kRegular);
        const SDL_FRect r{cx - w / 2 - 8, y - 4, w + 16, 26};
        const bool over = pointer_.In(r);
        hot_ |= over;
        Ease& hover = anim_[id];
        hover.To(over ? 1.0F : 0.0F, dt_);
        paint_.Text(label, cx, y, 13, Weight::kRegular, Mix(th::kTextDim, th::kPrimary, hover.value), Align::kCenter);
        return over && pointer_.released;
    }

    // Three choices on a track; the chosen one's pill slides to it. Returns the choice.
    std::size_t Segmented(const char* id, SDL_FRect r, std::array<const char*, 3> labels, std::size_t chosen) {
        paint_.RoundRect(r, r.h / 2, th::kSurfaceHigh);
        const float seg = r.w / 3;
        Ease& at = anim_[std::string(id) + "/at"];
        if (!seen_.contains(id)) at.value = static_cast<float>(chosen);  // no slide on first sight
        seen_[id] = true;
        at.To(static_cast<float>(chosen), dt_, 0.09F);
        const SDL_FRect pill{r.x + 3 + at.value * seg, r.y + 3, seg - 6, r.h - 6};
        paint_.RoundRect(pill, pill.h / 2, th::kPrimary);
        std::size_t result = chosen;
        for (std::size_t i = 0; i < 3; ++i) {
            const SDL_FRect cell{r.x + static_cast<float>(i) * seg, r.y, seg, r.h};
            const bool over = pointer_.In(cell);
            hot_ |= over && i != chosen;
            const float near = std::max(0.0F, 1 - std::abs(at.value - static_cast<float>(i)));
            const Rgba ink = Mix(over ? th::kText : th::kTextDim, th::kOnPrimary, near);
            paint_.Text(labels[i], cell.x + seg / 2, cell.y + (r.h - paint_.LineHeight(14)) / 2, 14,
                        Weight::kSemiBold, ink, Align::kCenter);
            if (over && pointer_.released) result = i;
        }
        return result;
    }

    // ---------------------------------------------------------------------------- sending screen

    void SendingControls(float W, float H, float alpha) {
        const auto& e = prepared_->envelope;
        const Speed& s = transfer_->speed;
        // Top: what is going, and how long a pass over it takes.
        const double pass = static_cast<double>(transfer_->PassFrames()) / s.states_per_second;
        const double shown = static_cast<double>(states_) / static_cast<double>(std::max<std::uint64_t>(1, transfer_->PassFrames()));
        char stats[96];
        std::snprintf(stats, sizeof stats, "each pass %.0f s · shown %.1f×", std::max(1.0, pass), shown);
        std::string what = FormatBytes(e.original_size);
        if (e.codec != 0) what += " → " + FormatBytes(e.bytes.size());
        const std::string name = paint_.Fit(prepared_->name, std::min(W * 0.4F, 420.0F), 15, Weight::kSemiBold);
        const float w1 = paint_.Measure(name, 15, Weight::kSemiBold);
        const float w2 = paint_.Measure(what + "  ·  " + stats, 13, Weight::kRegular);
        const float tw = std::max(w1 + 40, w2 + 40) + 28;
        const float slide = (1 - alpha) * -16;
        const SDL_FRect top{(W - tw) / 2, 20 + slide, tw, 62};
        paint_.RoundRect(top, 22, WithAlpha(th::kSurface, 0.94F * alpha));
        const float pulse = 0.5F + 0.5F * std::sin(t_ * 4);
        paint_.Glow(top.x + 26, top.y + 22, 12, WithAlpha(th::kTertiary, 0.5F * pulse * alpha));
        paint_.Circle(top.x + 26, top.y + 22, 4.5F, WithAlpha(th::kTertiary, alpha));
        paint_.Text(name, top.x + 40, top.y + 10, 15, Weight::kSemiBold, WithAlpha(th::kText, alpha));
        paint_.Text(what + "  ·  " + stats, top.x + 40, top.y + 34, 13, Weight::kRegular, WithAlpha(th::kTextDim, alpha));

        // Bottom: the speed, and Stop.
        const float bw = std::min(W - 40, 460.0F);
        const SDL_FRect bar{(W - bw) / 2, H - 82 - slide, bw, 58};
        paint_.RoundRect(bar, 29, WithAlpha(th::kSurface, 0.94F * alpha));
        const std::size_t speed = Segmented("send-speed", {bar.x + 9, bar.y + 9, bw - 140, 40},
                                            {kSpeeds[0].name, kSpeeds[1].name, kSpeeds[2].name}, settings_.speed);
        if (speed != settings_.speed) SetSpeed(speed);
        if (Button("stop", {bar.x + bw - 122, bar.y + 9, 113, 40}, "Stop", false)) Stop();
        SDL_SetCursor(hot_ ? HandCursor() : SDL_GetDefaultCursor());
        hot_ = false;
    }

    // ---------------------------------------------------------------------------------- actions

    bool Fullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

    void Wake() { controls_until_ = Clock::now() + kControlsShowFor; }

    void SetSpeed(std::size_t i) {
        if (i >= kSpeeds.size() || i == settings_.speed) return;
        settings_.speed = i;
        settings_.Save();
        if (screen_ == Screen::kSending) Restart();
    }

    void Stop() {
        screen_ = Screen::kHome;
        transfer_.reset();
        if (went_fullscreen_) SDL_SetWindowFullscreen(window_, false);
        went_fullscreen_ = false;
        SDL_ShowCursor();
    }

    static void SDLCALL OnChosen(void* self, const char* const* list, int /*filter*/) {
        auto* app = static_cast<App*>(self);
        std::lock_guard lock(app->chosen_mutex_);
        app->chosen_.clear();
        for (; list != nullptr && *list != nullptr; ++list) app->chosen_.emplace_back(*list);
        app->chosen_ready_ = true;
    }

    void Choose() { SDL_ShowOpenFileDialog(OnChosen, this, window_, nullptr, 0, nullptr, true); }

    void SendAgain(const Recent& r) {
        for (const auto& p : r.paths) {
            std::error_code ec;
            if (!std::filesystem::exists(PathOf(p), ec)) {
                problem_ = NameOf(PathOf(p)) + " has moved or gone since it was sent.";
                return;
            }
        }
        Send(r.paths);
    }

    // A send that started is remembered first in the recent list, replacing any earlier copy of it.
    void Remember() {
        if (sending_paths_.empty() || !prepared_) return;
        Recent r;
        r.name = prepared_->name;
        r.bytes = prepared_->envelope.original_size;
        r.when = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        r.paths = sending_paths_;
        std::erase_if(recent_, [&](const Recent& o) { return o.paths == r.paths; });
        recent_.insert(recent_.begin(), std::move(r));
        if (recent_.size() > Recent::kKeep) recent_.resize(Recent::kKeep);
        Recent::Save(recent_);
    }

    void TakeChosenFiles() {
        std::vector<std::string> paths;
        {
            std::lock_guard lock(chosen_mutex_);
            if (!chosen_ready_) return;
            chosen_ready_ = false;
            paths.swap(chosen_);
        }
        Send(std::move(paths));
    }

    void TakePrepared() {
        if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        prepared_ = pending_.get();
        if (!prepared_->problem.empty()) {
            problem_ = prepared_->problem;
            prepared_.reset();
            screen_ = Screen::kHome;
            return;
        }
        Restart();
        if (transfer_) Remember();
        if (transfer_ && !options_.windowed && !Fullscreen()) {
            SDL_SetWindowFullscreen(window_, true);
            went_fullscreen_ = true;
        }
    }

    void Restart() {
        if (!prepared_) return;
        std::string problem;
        transfer_ = Open(*prepared_, kSpeeds[settings_.speed], &problem);
        if (!transfer_) {
            problem_ = problem;
            screen_ = Screen::kHome;
            return;
        }
        screen_ = Screen::kSending;
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        texture_ = nullptr;  // made on the next frame, the right way up for the window
        next_due_.reset();
        states_ = 0;
        Wake();
        const auto& e = prepared_->envelope;
        std::printf("sending %s: %llu bytes as %zu, %s\n", prepared_->name.c_str(),
                    static_cast<unsigned long long>(e.original_size), e.bytes.size(), transfer_->speed.name);
        std::fflush(stdout);
    }

    // ---------------------------------------------------------------------- the code, pixel-exact

    void MakeTexture() {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        const Speed& s = transfer_->speed;
        tex_w_ = static_cast<int>(turned_ ? s.rows : s.cols);
        tex_h_ = static_cast<int>(turned_ ? s.cols : s.rows);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, tex_w_, tex_h_);
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
        return std::max(1, std::min((w - 2 * margin) / static_cast<int>(gw), (h - 2 * margin) / static_cast<int>(gh)));
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

    // Frames paced by the CLOCK, each due 1/rate after the last and shown on the refresh nearest its
    // due time -- so each lasts a whole number of refreshes, and none is skipped or doubled.
    //
    // It counted refreshes instead (a new frame every Nth, N from the measured refresh rate), which
    // is right only while the rate holds still. A Mac's adaptive-refresh panel does not: entering
    // full screen it ran at 60 Hz, then 120, and every change sent frames at the wrong rate until
    // the next measurement -- 34 a second on Fast, where 30 is meant, and the receiving phones counted
    // 40-48 through a whole transfer. A 50 fps camera misses frames shown that fast.
    void Pace(double states_per_second) {
        const auto now = Clock::now();
        const auto slack = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(0.5 / hz_));
        if (next_due_.has_value() && now + slack < *next_due_) return;
        Advance();
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / states_per_second));
        next_due_ = next_due_.value_or(now) + period;
        // Far behind (the window was hidden, the machine slept): start the clock again from now
        // rather than racing through the backlog.
        if (*next_due_ + period < now) next_due_ = now + period;
    }

    // The MEASURED refresh rate (median present interval): a display's stated rate is often wrong,
    // and variable-rate panels change theirs. Used only for pacing's half-refresh slack.
    void Measure() {
        const auto now = Clock::now();
        ++presents_;
        // Every few seconds, what was actually shown: the receiver's "states/s seen" should match.
        if (screen_ == Screen::kSending && now - report_at_ >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - report_at_).count();
            if (report_presents_ > 0) {
                std::printf("%.1f frames/s, %.1f presents/s\n", static_cast<double>(states_ - report_states_) / secs,
                            static_cast<double>(presents_ - report_presents_) / secs);
                std::fflush(stdout);
            }
            report_at_ = now;
            report_states_ = states_;
            report_presents_ = presents_;
        }
        if (last_present_.has_value()) intervals_.push_back(std::chrono::duration<double>(now - *last_present_).count());
        last_present_ = now;
        if (intervals_.size() < kRemeasureFrames) return;
        std::nth_element(intervals_.begin(), intervals_.begin() + static_cast<long>(intervals_.size() / 2), intervals_.end());
        const double median = intervals_[intervals_.size() / 2];
        intervals_.clear();
        if (median > 0.0) hz_ = 1.0 / median;
    }

    // Past the first frames, which carry the manifest: a snapshot shows file data.
    static constexpr std::uint64_t kSnapshotState = 5;

    void MaybeSnapshot() {
        if (options_.snapshot.empty()) return;
        const bool now = options_.snapshot_after > 0
                             ? static_cast<double>(t_) >= options_.snapshot_after
                             : (screen_ == Screen::kSending && states_ >= kSnapshotState && controls_.value <= 0.0F) ||
                                   (screen_ == Screen::kHome && t_ > 1.5F && pending_files_ == 0);
        if (!now) return;
        SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr);
        if (s == nullptr || !SDL_SaveBMP(s, options_.snapshot.c_str())) {
            std::fprintf(stderr, "snapshot: %s\n", SDL_GetError());
        } else {
            std::printf("snapshot %dx%d, %d px cells, frame %llu: %s\n", s->w, s->h, pitch_,
                        static_cast<unsigned long long>(states_), options_.snapshot.c_str());
        }
        SDL_DestroySurface(s);
        options_.snapshot.clear();
        done_ = true;
    }

  public:
    // Files given on the command line: the home screen is skipped, so a snapshot waits for them.
    std::size_t pending_files_ = 0;

  private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    Painter paint_;
    Settings settings_;
    Options options_;
    Screen screen_ = Screen::kHome;
    bool done_ = false;

    // Frame timing and animation.
    Clock::time_point began_;
    Clock::time_point last_frame_;
    float dt_ = 0;
    float t_ = 0;
    std::map<std::string, Ease> anim_;
    std::map<std::string, bool> seen_;
    Ease drag_;
    Ease controls_;
    Clock::time_point controls_until_;
    Pointer pointer_;
    bool hot_ = false;  // the pointer is over something clickable
    SDL_Cursor* hand_ = nullptr;

    // Files on their way in.
    std::vector<std::string> dropped_;
    std::optional<Clock::time_point> drag_seen_;
    std::mutex chosen_mutex_;
    std::vector<std::string> chosen_;
    bool chosen_ready_ = false;
    std::future<Prepared> pending_;
    std::string preparing_name_;
    std::string problem_;
    std::vector<std::string> sending_paths_;  // what is being sent, for the recent list
    std::vector<Recent> recent_;

    // Sending.
    std::optional<Prepared> prepared_;
    std::unique_ptr<Transfer> transfer_;
    SDL_Texture* texture_ = nullptr;
    std::vector<std::uint32_t> pixels_;
    bool turned_ = false;  // drawn a quarter turn round, for a landscape screen
    int tex_w_ = 0;
    int tex_h_ = 0;
    int pitch_ = 0;
    bool went_fullscreen_ = false;
    std::uint64_t states_ = 0;
    std::optional<Clock::time_point> next_due_;
    double hz_ = 60.0;  // the display's refresh rate: stated, then measured
    std::uint64_t presents_ = 0;
    Clock::time_point report_at_;
    std::uint64_t report_states_ = 0;
    std::uint64_t report_presents_ = 0;
    std::optional<Clock::time_point> last_present_;
    std::vector<double> intervals_;
};

// The window's icon: the light grid's corner markers and a few lit cells, drawn small.
SDL_Surface* Icon() {
    constexpr int kSide = 64;
    SDL_Surface* s = SDL_CreateSurface(kSide, kSide, SDL_PIXELFORMAT_ARGB8888);
    if (s == nullptr) return nullptr;
    const auto* fmt = SDL_GetPixelFormatDetails(s->format);
    auto fill = [&](int x, int y, int w, int h, std::uint32_t rgb) {
        const SDL_Rect r{x, y, w, h};
        SDL_FillSurfaceRect(s, &r, SDL_MapRGB(fmt, nullptr, static_cast<Uint8>(rgb >> 16), static_cast<Uint8>(rgb >> 8),
                                              static_cast<Uint8>(rgb)));
    };
    fill(0, 0, kSide, kSide, 0x0B2A33);
    for (const auto& [x, y] : {std::pair{8, 8}, std::pair{36, 8}, std::pair{8, 36}}) {
        fill(x, y, 20, 20, 0xE1E6E8);
        fill(x + 3, y + 3, 14, 14, 0x0B2A33);
        fill(x + 7, y + 7, 6, 6, 0xE1E6E8);
    }
    fill(38, 38, 8, 8, 0x7DD3F0);
    fill(48, 48, 8, 8, 0xA8E6A1);
    fill(48, 38, 8, 8, 0xE1E6E8);
    return s;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: fileflow-send [--speed steady|fast|fastest] [--compression off|auto|max] [--windowed]\n"
                 "                     [--window WxH] [--seconds N] [--snapshot OUT.bmp [--snapshot-after S]] [FILE...]\n"
                 "With no files, the home screen opens: drop files on it, or choose them.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Settings settings = Settings::Load();
    App::Options options;
    double seconds = 0.0;  // > 0: stop after this long (for unattended runs)
    int win_w = 1100;
    int win_h = 760;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--speed" && i + 1 < argc) {
            const std::string v = argv[++i];
            settings.speed = v == "steady" ? 0 : v == "fast" ? 1 : v == "fastest" ? 2 : kSpeeds.size();
            if (settings.speed == kSpeeds.size()) return Usage();
        } else if (a == "--compression" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "off") settings.compression = 0;
            else if (v == "auto") settings.compression = 1;
            else if (v == "max") settings.compression = 2;
            else return Usage();
        } else if (a == "--windowed") {
            options.windowed = true;
        } else if (a == "--window" && i + 1 < argc) {
            // The window's size in points, e.g. 640x600: for checking the layout at a size.
            if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2 || win_w < 320 || win_h < 320) return Usage();
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (a == "--snapshot" && i + 1 < argc) {
            options.snapshot = argv[++i];
        } else if (a == "--snapshot-after" && i + 1 < argc) {
            options.snapshot_after = std::atof(argv[++i]);
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
    SDL_SetAppMetadata("FileFlow", "0.2.0", "dev.fileflow.desktop");
    // Straight to full screen when there are files to send; otherwise a window for the home screen.
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (!files.empty() && !options.windowed) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow("FileFlow", win_w, win_h, flags);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 520, 560);
    if (SDL_Surface* icon = Icon()) {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
    // Presenting waits for the display's refresh: the clock every frame is counted against.
    SDL_SetRenderVSync(renderer, 1);

    {
        App app(window, renderer, settings, options);
        app.pending_files_ = files.size();
        app.Send(files);
        const auto began = Clock::now();
        while (!app.done()) {
            if (seconds > 0.0 && std::chrono::duration<double>(Clock::now() - began).count() > seconds) break;
            SDL_Event e;
            while (SDL_PollEvent(&e)) app.Event(e);
            app.Frame();
        }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
