#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fileflow::desktop {

namespace {

// Present intervals measured before the refresh rate is re-derived.
constexpr std::size_t kRemeasureFrames = 30;
// How long the controls stay over the code after sending starts, or after a key or the mouse
// moves. While sending the screen must show NOTHING but the code once they've gone: the receiver
// finds the grid by its outer ring, and bright text between that ring and the edge of the camera's
// view is mistaken for it -- a snapshot with a status line showing did not decode at all; without
// it, every cell was right.
constexpr auto kControlsShowFor = std::chrono::seconds(3);
// Past the first frames, which carry the manifest: a snapshot shows file data.
constexpr std::uint64_t kSnapshotState = 5;

CompressionMode ModeOf(std::size_t i) {
    return i == 0 ? CompressionMode::kOff : i == 2 ? CompressionMode::kMaximum : CompressionMode::kAuto;
}

std::uint32_t Hash(int a, int b) {
    auto h = static_cast<std::uint32_t>(a) * 374761393U + static_cast<std::uint32_t>(b) * 668265263U;
    h = (h ^ (h >> 13)) * 1274126177U;
    return h ^ (h >> 16);
}

}  // namespace

std::string PrefPath(const char* file) {
    char* dir = SDL_GetPrefPath("FileFlow", "Desktop");
    if (dir == nullptr) return {};
    std::string p = std::string(dir) + file;
    SDL_free(dir);
    return p;
}

Settings Settings::Load() {
    Settings s;
    std::ifstream in(PrefPath("settings.txt"));
    std::string line;
    while (std::getline(in, line)) {
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp);
        const std::string value = line.substr(sp + 1);
        const auto n = std::strtoul(value.c_str(), nullptr, 10);
        if (key == "speed" && n < kSpeeds.size()) s.speed = n;
        if (key == "compression" && n < kCompressionNames.size()) s.compression = n;
        if (key == "name") s.name = value;
        if (key == "encrypted") s.encrypted = n != 0;
        if (key == "seal_to") s.seal_to = value;
    }
    return s;
}

void Settings::Save() const {
    std::ofstream out(PrefPath("settings.txt"));
    out << "speed " << speed << "\ncompression " << compression << "\n";
    if (!name.empty()) out << "name " << name << "\n";
    out << "encrypted " << (encrypted ? 1 : 0) << "\n";
    if (!seal_to.empty()) out << "seal_to " << seal_to << "\n";
}

// One entry per block: "name<TAB>bytes<TAB>when", then a line per path, then a blank line.
std::vector<Recent> Recent::Load() {
    std::vector<Recent> out;
    std::ifstream in(PrefPath("recent.txt"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Recent r;
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        r.name = line.substr(0, a);
        r.bytes = std::strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
        r.when = std::strtoll(line.substr(b + 1).c_str(), nullptr, 10);
        while (std::getline(in, line) && !line.empty()) r.paths.push_back(line);
        if (!r.paths.empty()) out.push_back(std::move(r));
    }
    if (out.size() > kKeep) out.resize(kKeep);
    return out;
}

void Recent::Save(const std::vector<Recent>& list) {
    std::ofstream out(PrefPath("recent.txt"));
    for (const auto& r : list) {
        out << r.name << '\t' << r.bytes << '\t' << r.when << '\n';
        for (const auto& p : r.paths) out << p << '\n';
        out << '\n';
    }
}

std::string Ago(std::int64_t when) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto s = std::max<std::int64_t>(0, now - when);
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + " h ago";
    const auto d = s / 86400;
    return d == 1 ? "yesterday" : std::to_string(d) + " days ago";
}

// "⌘V" on a Mac, "Ctrl+V" elsewhere: shortcuts said the way the keyboard says them.
std::string Shortcut(const char* key) {
    return std::string(SDL_strcmp(SDL_GetPlatform(), "macOS") == 0 ? "⌘" : "Ctrl+") + key;
}

// The light grid: FileFlow's mark, as on the phone's Send screen -- a tile of cells each easing
// between the theme's colours on its own slow beat, with the real frame's three corner markers.
void LightGrid(Painter& p, float cx, float cy, float side, float t, float lift) {
    namespace th = theme;
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

App::CodeView::~CodeView() {
    if (texture != nullptr) SDL_DestroyTexture(texture);
    if (window != nullptr) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
    }
}

App::App(SDL_Window* w, SDL_Renderer* r, Settings settings, Options options)
    : window_(w), renderer_(r), paint_(r), ui_(paint_), settings_(std::move(settings)), options_(std::move(options)),
      teams_(PrefPath("teams.txt")) {
    ui_.window = w;
    // The display's stated rate until one is measured.
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w));
    if (m != nullptr && m->refresh_rate > 0.0F) hz_ = static_cast<double>(m->refresh_rate);
    began_ = last_frame_ = Clock::now();
    recent_ = Recent::Load();
    if (options_.open == "teams") tab_ = Tab::kTeams;
    if (options_.open == "pair") StartPairing(secure::PairingFlow::Mode::kPair);
    if (options_.open == "join") StartPairing(secure::PairingFlow::Mode::kJoin);
    if (options_.open == "encrypted") settings_.encrypted = true;
}

App::~App() {
    code_.reset();
    if (preview_ != nullptr) SDL_DestroyTexture(preview_);
}

void App::Send(std::vector<std::string> paths) {
    if (paths.empty() || phase_ == Phase::kPreparing) return;
    preparing_name_ = paths.size() == 1 ? NameOf(PathOf(paths[0])) : std::to_string(paths.size()) + " files";
    transfer_.reset();
    phase_ = Phase::kPreparing;
    tab_ = Tab::kSend;
    problem_.clear();
    sending_paths_ = paths;
    // Reading and compressing can take seconds for a big file; the window stays live meanwhile.
    pending_ = std::async(std::launch::async, Prepare, std::move(paths), ModeOf(settings_.compression));
}

// The clipboard's text, sent as a note: the quickest way to get a link or an address from a
// computer onto a phone with no network between them.
void App::SendClipboard() {
    if (phase_ != Phase::kIdle) return;
    char* text = SDL_GetClipboardText();
    std::string s = text != nullptr ? text : "";
    SDL_free(text);
    if (s.empty()) {
        problem_ = "The clipboard has no text to send. Copy some first.";
        return;
    }
    preparing_name_ = "the clipboard";
    transfer_.reset();
    phase_ = Phase::kPreparing;
    problem_.clear();
    sending_paths_.clear();  // nothing to send again from: the clipboard will have moved on
    pending_ = std::async(std::launch::async, PrepareText, std::move(s), std::string("Clipboard.txt"),
                          ModeOf(settings_.compression));
}

void App::Event(const SDL_Event& e) {
    ui_.Event(e);
    switch (e.type) {
        case SDL_EVENT_QUIT:
            done_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            // The presenting display's window closed (Cmd+W on it): stop, don't quit.
            if (code_ && code_->window != nullptr && e.window.windowID == SDL_GetWindowID(code_->window)) Stop();
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            Wake();
            break;
        case SDL_EVENT_KEY_DOWN: {
            Wake();
            if (ui_.typing()) break;  // keys are letters while a text field has them
            const bool command = (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
            if (e.key.key == SDLK_ESCAPE && pair_) pair_.reset();
            else if (e.key.key == SDLK_ESCAPE && phase_ == Phase::kSending) Stop();
            else if (e.key.key == SDLK_O && command && phase_ == Phase::kIdle) Choose();
            else if (e.key.key == SDLK_V && command && phase_ == Phase::kIdle) SendClipboard();
            else if (e.key.key == SDLK_1) SetSpeed(0);
            else if (e.key.key == SDLK_2) SetSpeed(1);
            else if (e.key.key == SDLK_3) SetSpeed(2);
            else if (e.key.key == SDLK_F && !presenting()) SDL_SetWindowFullscreen(window_, !Fullscreen());
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
void App::Frame() {
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
    ui_.Begin(dt_, t_);

    if (phase_ == Phase::kSending) {
        CodeView& v = *code_;
        int cw = 0;
        int ch = 0;
        SDL_GetCurrentRenderOutputSize(v.renderer, &cw, &ch);
        const Speed& s = transfer_->speed;
        // Whichever way up gives the bigger cells, re-decided as the window changes shape.
        const bool turned = Pitch(cw, ch, s.rows, s.cols) > Pitch(cw, ch, s.cols, s.rows);
        if (v.texture == nullptr || turned != v.turned) {
            v.turned = turned;
            MakeTexture(v);
        }
        Pace(s.states_per_second);
        if (presenting()) {
            // The code on its own display, at its refresh; the presenter's view here, not waited for.
            SDL_SetRenderDrawColor(v.renderer, 0, 0, 0, 255);
            SDL_RenderClear(v.renderer);
            DrawGrid(v, cw, ch);
            Presenter(W, H);
        } else {
            paint_.Clear({0, 0, 0, 1});
            DrawGrid(v, pw, ph);
            controls_.To(now < controls_until_ ? 1.0F : 0.0F, dt_, 0.15F);
            if (controls_.value > 0.01F) SendingOverlay(W, H, controls_.value);
            if (controls_.value > 0.01F) SDL_ShowCursor(); else SDL_HideCursor();
        }
    } else if (pair_) {
        PairScreen(W, H);
    } else {
        constexpr float kRail = 92;
        if (tab_ == Tab::kTeams) TeamsScreen(kRail, W, H);
        else SendScreen(kRail, W, H);
        Rail(H);
    }
    ui_.End();
    MaybeSnapshot();
    if (presenting()) {
        SDL_RenderPresent(code_->renderer);  // waits for the code display's refresh
        SDL_RenderPresent(renderer_);
    } else {
        SDL_RenderPresent(renderer_);
    }
    Measure();
}

// ---------------------------------------------------------------------------------------- actions

bool App::Fullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

void App::Wake() { controls_until_ = Clock::now() + kControlsShowFor; }

void App::SetSpeed(std::size_t i) {
    if (i >= kSpeeds.size() || i == settings_.speed) return;
    settings_.speed = i;
    settings_.Save();
    if (phase_ == Phase::kSending) Restart();
}

void App::Stop() {
    phase_ = Phase::kIdle;
    transfer_.reset();
    code_.reset();  // closes the presenting display's window, if there was one
    if (went_fullscreen_) SDL_SetWindowFullscreen(window_, false);
    went_fullscreen_ = false;
    SDL_SetRenderVSync(renderer_, 1);
    SDL_ShowCursor();
}

void SDLCALL App::OnChosen(void* self, const char* const* list, int /*filter*/) {
    auto* app = static_cast<App*>(self);
    std::lock_guard lock(app->chosen_mutex_);
    app->chosen_.clear();
    for (; list != nullptr && *list != nullptr; ++list) app->chosen_.emplace_back(*list);
    app->chosen_ready_ = true;
}

void App::Choose() { SDL_ShowOpenFileDialog(OnChosen, this, window_, nullptr, 0, nullptr, true); }

void App::SendAgain(const Recent& r) {
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
void App::Remember() {
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

void App::TakeChosenFiles() {
    std::vector<std::string> paths;
    {
        std::lock_guard lock(chosen_mutex_);
        if (!chosen_ready_) return;
        chosen_ready_ = false;
        paths.swap(chosen_);
    }
    Send(std::move(paths));
}

void App::TakePrepared() {
    if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    prepared_ = pending_.get();
    if (!prepared_->problem.empty()) {
        problem_ = prepared_->problem;
        prepared_.reset();
        phase_ = Phase::kIdle;
        return;
    }
    if (settings_.encrypted) {
        // Sealed here, never sent in the clear by mistake: no key, no sending.
        const auto key = SealTarget();
        if (!key) {
            problem_ = "Encrypted is on, but no phone is paired and no team chosen. Pair, or choose a team.";
            prepared_.reset();
            phase_ = Phase::kIdle;
            return;
        }
        prepared_->envelope.bytes = secure::Seal(prepared_->envelope.bytes, *key);
        prepared_->sealed = true;
    }
    OpenCodeView();
    Restart();
    if (transfer_) Remember();
    if (transfer_ && !presenting() && !options_.windowed && !Fullscreen()) {
        SDL_SetWindowFullscreen(window_, true);
        went_fullscreen_ = true;
    }
}

void App::Restart() {
    if (!prepared_) return;
    std::string problem;
    transfer_ = Open(*prepared_, kSpeeds[settings_.speed], &problem);
    if (!transfer_) {
        problem_ = problem;
        Stop();
        return;
    }
    phase_ = Phase::kSending;
    if (code_ && code_->texture != nullptr) {
        SDL_DestroyTexture(code_->texture);
        code_->texture = nullptr;  // made on the next frame, the right way up for the window
    }
    next_due_.reset();
    states_ = 0;
    Wake();
    const auto& e = prepared_->envelope;
    std::printf("sending %s: %llu bytes as %zu, %s%s\n", prepared_->name.c_str(),
                static_cast<unsigned long long>(e.original_size), e.bytes.size(), transfer_->speed.name,
                presenting() ? ", presenting" : "");
    std::fflush(stdout);
}

// --------------------------------------------------------------------------- the code, pixel-exact

std::vector<SDL_DisplayID> App::Displays() const {
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    std::vector<SDL_DisplayID> out(ids, ids + std::max(0, n));
    SDL_free(ids);
    return out;
}

// The code goes where it was asked: on the app's own display, in the app's window; on another, in
// a full-screen window there of its own -- a projector or a TV -- with the app's window left as the
// presenter's view.
void App::OpenCodeView() {
    code_ = std::make_unique<CodeView>();
    const auto displays = Displays();
    const SDL_DisplayID here = SDL_GetDisplayForWindow(window_);
    const SDL_DisplayID there = display_choice_ < displays.size() ? displays[display_choice_] : here;
    if (there == here && !options_.code_window) {
        code_->renderer = renderer_;
        return;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!options_.code_window) flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "FileFlow — code");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(there));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(there));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 960);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 600);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
    code_->window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    code_->renderer = code_->window != nullptr ? SDL_CreateRenderer(code_->window, nullptr) : nullptr;
    if (code_->renderer == nullptr) {
        // No second window to be had: show the code here after all.
        std::fprintf(stderr, "presenting window: %s\n", SDL_GetError());
        if (code_->window != nullptr) SDL_DestroyWindow(code_->window);
        code_ = std::make_unique<CodeView>();
        code_->renderer = renderer_;
        return;
    }
    // The code's display sets the pace; the presenter's view follows without waiting for its own.
    SDL_SetRenderVSync(code_->renderer, 1);
    SDL_SetRenderVSync(renderer_, 0);
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(there);
    if (m != nullptr && m->refresh_rate > 0.0F) hz_ = static_cast<double>(m->refresh_rate);
    SDL_HideCursor();
}

void App::MakeTexture(CodeView& v) {
    if (v.texture != nullptr) SDL_DestroyTexture(v.texture);
    const Speed& s = transfer_->speed;
    v.tex_w = static_cast<int>(v.turned ? s.rows : s.cols);
    v.tex_h = static_cast<int>(v.turned ? s.cols : s.rows);
    v.texture = SDL_CreateTexture(v.renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, v.tex_w, v.tex_h);
    // Nearest neighbour: every cell a flat square with hard edges, as the phone's GL_NEAREST.
    SDL_SetTextureScaleMode(v.texture, SDL_SCALEMODE_NEAREST);
    if (presenting()) {
        if (preview_ != nullptr) SDL_DestroyTexture(preview_);
        preview_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, v.tex_w, v.tex_h);
        SDL_SetTextureScaleMode(preview_, SDL_SCALEMODE_NEAREST);
    }
    pixels_.assign(static_cast<std::size_t>(s.cols) * s.rows, 0);
    Upload();
}

void App::Advance() {
    auto header = transfer_->session->Next(&transfer_->cells);
    if (!header.ok()) return;
    Upload();
    ++states_;
}

// The current cells into the texture, turned a quarter clockwise when the grid is drawn turned:
// cell (x, y) of a cols x rows grid lands at (rows - 1 - y, x).
void App::Upload() {
    CodeView& v = *code_;
    if (v.texture == nullptr) return;
    const auto& cells = transfer_->cells.data();
    const std::size_t cols = transfer_->speed.cols;
    const std::size_t rows = transfer_->speed.rows;
    for (std::size_t y = 0; y < rows; ++y) {
        for (std::size_t x = 0; x < cols; ++x) {
            const std::size_t to = v.turned ? x * rows + (rows - 1 - y) : y * cols + x;
            pixels_[to] = CellColour(cells[y * cols + x]);
        }
    }
    const int pitch = v.tex_w * static_cast<int>(sizeof(std::uint32_t));
    SDL_UpdateTexture(v.texture, nullptr, pixels_.data(), pitch);
    if (presenting() && preview_ != nullptr) SDL_UpdateTexture(preview_, nullptr, pixels_.data(), pitch);
}

// Largest whole-pixel cell for a gw x gh grid in w x h with a margin: the phone's rule, so no
// cell edge ever falls between pixels.
int App::Pitch(int w, int h, std::uint32_t gw, std::uint32_t gh) {
    const int margin = std::max(24, std::min(w, h) / 30);
    return std::max(1, std::min((w - 2 * margin) / static_cast<int>(gw), (h - 2 * margin) / static_cast<int>(gh)));
}

void App::DrawGrid(CodeView& v, int w, int h) {
    const int pitch = Pitch(w, h, static_cast<std::uint32_t>(v.tex_w), static_cast<std::uint32_t>(v.tex_h));
    const int gw = pitch * v.tex_w;
    const int gh = pitch * v.tex_h;
    const SDL_FRect dst{static_cast<float>((w - gw) / 2), static_cast<float>((h - gh) / 2), static_cast<float>(gw),
                        static_cast<float>(gh)};
    SDL_RenderTexture(v.renderer, v.texture, nullptr, &dst);
    v.pitch = pitch;
}

// Frames paced by the CLOCK, each due 1/rate after the last and shown on the refresh nearest its
// due time -- so each lasts a whole number of refreshes, and none is skipped or doubled.
//
// It counted refreshes instead (a new frame every Nth, N from the measured refresh rate), which is
// right only while the rate holds still. A Mac's adaptive-refresh panel does not: entering full
// screen it ran at 60 Hz, then 120, and every change sent frames at the wrong rate until the next
// measurement -- 34 a second on Fast, where 30 is meant, and the receiving phones counted 40-48
// through a whole transfer. A 50 fps camera misses frames shown that fast.
void App::Pace(double states_per_second) {
    const auto now = Clock::now();
    const auto slack = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(0.5 / hz_));
    if (next_due_.has_value() && now + slack < *next_due_) return;
    Advance();
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / states_per_second));
    next_due_ = next_due_.value_or(now) + period;
    // Far behind (the window was hidden, the machine slept): start the clock again from now rather
    // than racing through the backlog.
    if (*next_due_ + period < now) next_due_ = now + period;
}

// The MEASURED refresh rate (median present interval): a display's stated rate is often wrong, and
// variable-rate panels change theirs. Used only for pacing's half-refresh slack.
void App::Measure() {
    const auto now = Clock::now();
    ++presents_;
    // Every few seconds, what was actually shown: the receiver's "states/s seen" should match.
    if (phase_ == Phase::kSending && now - report_at_ >= std::chrono::seconds(5)) {
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

// Saves what is drawn: the app's window, and -- presenting -- the code's window beside it, as
// OUT-code.bmp.
void App::MaybeSnapshot() {
    if (options_.snapshot.empty()) return;
    const bool now = options_.snapshot_after > 0
                         ? static_cast<double>(t_) >= options_.snapshot_after
                         : (phase_ == Phase::kSending && states_ >= kSnapshotState && (presenting() || controls_.value <= 0.0F)) ||
                               (phase_ == Phase::kIdle && t_ > 1.5F && pending_files == 0);
    if (!now) return;
    const auto save = [&](SDL_Renderer* r, const std::string& path) {
        SDL_Surface* s = SDL_RenderReadPixels(r, nullptr);
        if (s == nullptr || !SDL_SaveBMP(s, path.c_str())) {
            std::fprintf(stderr, "snapshot: %s\n", SDL_GetError());
        } else {
            std::printf("snapshot %dx%d, %d px cells, frame %llu: %s\n", s->w, s->h, code_ ? code_->pitch : 0,
                        static_cast<unsigned long long>(states_), path.c_str());
        }
        SDL_DestroySurface(s);
    };
    save(renderer_, options_.snapshot);
    if (presenting()) {
        std::string code = options_.snapshot;
        const auto dot = code.rfind('.');
        code.insert(dot == std::string::npos ? code.size() : dot, "-code");
        save(code_->renderer, code);
    }
    options_.snapshot.clear();
    done_ = true;
}

}  // namespace fileflow::desktop
