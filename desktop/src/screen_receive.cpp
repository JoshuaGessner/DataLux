// Receive and Received: a phone's sending screen held up to this computer's camera, the file it
// carries saved to Downloads/FileFlow -- and the list of what has come, as on the phone.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace fileflow::desktop {

namespace th = theme;

namespace {

// A path as a file:// URL, for the system to open: everything but the safe characters escaped.
std::string FileUrl(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    std::string url = "file://";
    for (char8_t c8 : u8) {
        const auto c = static_cast<unsigned char>(c8);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            url += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            url += hex;
        }
    }
    return url;
}

std::string Rate(double bytes_per_second) {
    if (bytes_per_second <= 0) return "";
    return FormatBytes(static_cast<std::uint64_t>(bytes_per_second)) + "/s";
}

// A tick in a ring: received.
void Tick(Painter& p, float cx, float cy, float r, Rgba c) {
    p.Glow(cx, cy, r * 2.2F, WithAlpha(c, 0.25F));
    p.Circle(cx, cy, r, WithAlpha(c, 0.18F));
    p.Arc(cx, cy, r - 2, 3, 0, 2 * std::numbers::pi_v<float>, c);
    p.Polygon({{cx - r * 0.42F, cy + r * 0.02F}, {cx - r * 0.30F, cy - r * 0.12F}, {cx - r * 0.08F, cy + r * 0.12F},
               {cx - r * 0.12F, cy + r * 0.30F}},
              c);
    p.Polygon({{cx - r * 0.12F, cy + r * 0.30F}, {cx - r * 0.08F, cy + r * 0.12F}, {cx + r * 0.32F, cy - r * 0.30F},
               {cx + r * 0.44F, cy - r * 0.18F}},
              c);
}

}  // namespace

void App::StartReceiving() {
    rx_result_.reset();
    receiver_ = std::make_unique<Receiver>(Keys(), settings_.encrypted);
    if (!options_.no_camera) rx_camera_.Open();
}

void App::StopReceiving() {
    receiver_.reset();  // joins its thread
    rx_camera_.Close();
}

void App::ReceiveScreen(float x0, float W, float H) {
    paint_.Clear(th::kBackground);
    const float L = x0 + 36;
    const float cw = std::min(W - x0 - 72, 760.0F);
    paint_.Text("Receive", L, 24, 28, Weight::kSemiBold, th::kText);

    // Receiving: the camera's view, with where it's got over it.
    if (receiver_) {
        if (rx_camera_.Poll(renderer_)) {
            receiver_->Offer(rx_camera_.luma(), rx_camera_.u(), rx_camera_.v(), rx_camera_.width(), rx_camera_.height(),
                             rx_camera_.timestamp_ns());
        }
        if (auto r = receiver_->Result()) {
            rx_result_ = std::move(r);
            StopReceiving();
            if (!rx_result_->files.empty()) received_dirty_ = true;
            return;
        }
        const LiveStatus s = receiver_->Status();
        const float vh = std::min(H - 210, cw * 0.62F);
        const SDL_FRect view{L, 76, cw, vh};
        paint_.RoundRect(view, 22, Hex(0x000000));
        const auto state = rx_camera_.state();
        if (state == Camera::State::kOn && rx_camera_.preview() != nullptr) {
            const float aspect = static_cast<float>(rx_camera_.width()) / static_cast<float>(std::max(1, rx_camera_.height()));
            float w = cw;
            float h = w / aspect;
            if (h > vh) {
                h = vh;
                w = h * aspect;
            }
            const float k = paint_.scale();
            // Mirrored, as a camera facing you is: moving the phone left moves it left here.
            const SDL_FRect dst{(L + (cw - w) / 2) * k, (76 + (vh - h) / 2) * k, w * k, h * k};
            SDL_RenderTextureRotated(renderer_, rx_camera_.preview(), nullptr, &dst, 0, nullptr, SDL_FLIP_HORIZONTAL);
        } else {
            const char* msg = state == Camera::State::kNone     ? "This computer has no camera to receive with."
                              : state == Camera::State::kDenied ? "FileFlow can't use the camera. Allow it in your system's privacy settings."
                                                                : "Starting the camera… (allow it if you're asked)";
            paint_.Text(msg, L + cw / 2, 76 + vh / 2 - 10, 14, Weight::kRegular, th::kTextDim, Align::kCenter);
        }
        // The status: what to do, or what's coming.
        const std::string said = s.phase == LivePhase::kReceiving && !s.file_name.empty() ? "Receiving " + s.file_name : s.message;
        const float pw = std::min(cw - 40, paint_.Measure(said, 15, Weight::kSemiBold) + 60);
        const SDL_FRect pill{L + (cw - pw) / 2, 76 + vh - 64, pw, 44};
        paint_.RoundRect(pill, 22, WithAlpha(th::kSurface, 0.92F));
        paint_.Circle(pill.x + 22, pill.y + 22, 4.5F, s.screen_in_view ? th::kTertiary : th::kTextDim);
        paint_.Text(paint_.Fit(said, pw - 50, 15, Weight::kSemiBold), pill.x + 36, pill.y + 12, 15, Weight::kSemiBold, th::kText);

        // Progress, speed and time left.
        float y = 76 + vh + 22;
        const SDL_FRect track{L, y, cw, 8};
        paint_.RoundRect(track, 4, th::kSurfaceHigh);
        Ease& bar = ui_.Anim("rx-progress");
        bar.To(static_cast<float>(s.progress), dt_, 0.25F);
        if (bar.value > 0.002F) paint_.RoundRect({L, y, std::max(8.0F, cw * bar.value), 8}, 4, th::kPrimary);
        y += 20;
        std::string line = std::to_string(static_cast<int>(std::lround(s.progress * 100))) + "%";
        if (s.bytes_per_second > 0) line += "  ·  " + Rate(s.bytes_per_second);
        if (s.seconds_remaining >= 0) line += "  ·  about " + std::to_string(static_cast<int>(std::ceil(s.seconds_remaining))) + " s left";
        paint_.Text(line, L, y, 14, Weight::kRegular, th::kTextDim);
        if (ui_.Button("rx-stop", {L + cw - 120, y - 6, 120, 42}, "Stop", false)) StopReceiving();
        return;
    }

    // Done: what came, and what to do with it.
    if (rx_result_) {
        const ReceiveResult& r = *rx_result_;
        const float cx = L + cw / 2;
        if (r.ok) {
            Tick(paint_, cx, 160, 44, th::kTertiary);
            paint_.Text("Received", cx, 222, 28, Weight::kSemiBold, th::kText, Align::kCenter);
            paint_.Text(paint_.Fit(r.name, cw - 40, 18, Weight::kSemiBold), cx, 262, 18, Weight::kSemiBold, th::kText, Align::kCenter);
            std::string facts = FormatBytes(r.bytes);
            if (r.seconds > 0) facts += "  ·  " + Rate(static_cast<double>(r.bytes) / r.seconds) + " in " +
                                        std::to_string(static_cast<int>(std::lround(r.seconds))) + " s";
            if (!r.sealed_to.empty()) facts += "  ·  encrypted to " + r.sealed_to;
            paint_.Text(facts, cx, 292, 14, Weight::kRegular, th::kTextDim, Align::kCenter);
            const std::string where = "Saved to " + ReceivedFolder().string();
            paint_.Text(paint_.Fit(where, cw - 40, 13, Weight::kRegular), cx, 316, 13, Weight::kRegular, th::kTextDim, Align::kCenter);
            const float by = 360;
            if (r.files.size() == 1 && ui_.Button("rx-open", {cx - 206, by, 132, 46}, "Open", true)) SDL_OpenURL(FileUrl(r.files[0].path).c_str());
            if (ui_.Button("rx-folder", {r.files.size() == 1 ? cx - 66 : cx - 140, by, 132, 46}, "Show folder", false)) {
                SDL_OpenURL(FileUrl(ReceivedFolder()).c_str());
            }
            if (ui_.Button("rx-again", {r.files.size() == 1 ? cx + 74 : cx + 8, by, 132, 46}, "Receive another", false)) StartReceiving();
        } else {
            paint_.Circle(cx, 160, 44, WithAlpha(th::kError, 0.18F));
            paint_.Text("!", cx, 136, 40, Weight::kSemiBold, th::kError, Align::kCenter);
            paint_.Text("Not received", cx, 222, 28, Weight::kSemiBold, th::kText, Align::kCenter);
            paint_.Text(paint_.Fit(r.problem, cw - 40, 15, Weight::kRegular), cx, 262, 15, Weight::kRegular, th::kTextDim, Align::kCenter);
            if (ui_.Button("rx-retry", {cx - 80, 310, 160, 46}, "Try again", true)) StartReceiving();
        }
        return;
    }

    // Before: what to do.
    const float cx = L + cw / 2;
    const float top = std::max(110.0F, H / 2 - 190);
    paint_.Glow(cx, top + 70, 110, WithAlpha(th::kTertiary, 0.22F));
    // A viewfinder, its corners bracketing a beam.
    const float s = 120;
    const SDL_FRect box{cx - s / 2, top + 10, s, s};
    const float beam = 0.5F + 0.45F * std::sin(t_ * 1.8F);
    paint_.Glow(cx, box.y + s * beam, s * 0.45F, WithAlpha(th::kTertiary, 0.35F));
    paint_.RoundRect({box.x + 10, box.y + s * beam - 1, s - 20, 2.5F}, 1.2F, th::kTertiary);
    for (const auto& [sx, sy] : {std::pair{-1.0F, -1.0F}, std::pair{1.0F, -1.0F}, std::pair{-1.0F, 1.0F}, std::pair{1.0F, 1.0F}}) {
        const float x = sx < 0 ? box.x : box.x + s - 26;
        const float y = sy < 0 ? box.y : box.y + s - 26;
        paint_.RoundRect({sx < 0 ? x : x + 22, y, 4, 26}, 2, th::kText);
        paint_.RoundRect({x, sy < 0 ? y : y + 22, 26, 4}, 2, th::kText);
    }
    paint_.Text("Receive a file", cx, top + 160, 26, Weight::kSemiBold, th::kText, Align::kCenter);
    paint_.Text("Hold the sending phone up to this computer's camera, its screen filling the picture.", cx, top + 200, 15,
                Weight::kRegular, th::kTextDim, Align::kCenter);
    paint_.Text("Set the phone to Steady: a computer's camera is slower than a phone's.", cx, top + 224, 15, Weight::kRegular,
                th::kTextDim, Align::kCenter);
    if (settings_.encrypted) {
        paint_.Text("Encrypted is on: only files for your pairing or your teams are accepted.", cx, top + 252, 13,
                    Weight::kRegular, th::kPrimary, Align::kCenter);
    }
    if (ui_.Button("rx-start", {cx - 110, top + 290, 220, 50}, "Start receiving", true)) StartReceiving();
}

void App::ReceivedScreen(float x0, float W, float H) {
    paint_.Clear(th::kBackground);
    const float L = x0 + 36;
    const float cw = std::min(W - x0 - 72, 760.0F);
    paint_.Text("Received", L, 24, 28, Weight::kSemiBold, th::kText);
    // The folder, read again when something has arrived or every few seconds -- not every frame.
    if (received_dirty_ || t_ - received_read_at_ > 3) {
        received_.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(ReceivedFolder(), ec)) {
            if (!e.is_regular_file(ec)) continue;
            const auto name = e.path().filename().u8string();
            if (!name.empty() && name[0] == u8'.') continue;
            received_.push_back({std::string(name.begin(), name.end()), e.path(), e.file_size(ec)});
            // The file clock's epoch needn't be the system clock's: carried across by "now" on each.
            const auto written = std::filesystem::last_write_time(e.path(), ec);
            const auto sys = std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(written - std::filesystem::file_time_type::clock::now());
            received_times_[e.path().string()] = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
        }
        std::sort(received_.begin(), received_.end(), [&](const SavedFile& a, const SavedFile& b) {
            return received_times_[a.path.string()] > received_times_[b.path.string()];
        });
        received_dirty_ = false;
        received_read_at_ = t_;
    }
    std::uint64_t total = 0;
    for (const auto& f : received_) total += f.bytes;
    paint_.Text(received_.empty() ? std::string("Nothing here yet")
                                  : std::to_string(received_.size()) + (received_.size() == 1 ? " file" : " files") + " · " + FormatBytes(total),
                L, 64, 14, Weight::kRegular, th::kTextDim);
    if (!received_.empty() && ui_.Button("open-folder", {L + cw - 150, 22, 150, 40}, "Show folder", false)) {
        SDL_OpenURL(FileUrl(ReceivedFolder()).c_str());
    }
    if (received_.empty()) {
        const float cx = L + cw / 2;
        paint_.Glow(cx, 220, 80, WithAlpha(th::kPrimary, 0.2F));
        paint_.Text("Files you receive are kept here, and in Downloads › FileFlow.", cx, 210, 15, Weight::kRegular,
                    th::kTextDim, Align::kCenter);
        if (ui_.Button("go-receive", {cx - 100, 250, 200, 46}, "Start receiving", false)) {
            tab_ = Tab::kReceive;
            StartReceiving();
        }
        return;
    }
    float y = 100;
    for (std::size_t i = 0; i < received_.size() && y + 56 <= H - 16; ++i) {
        const auto& f = received_[i];
        const SDL_FRect row{L, y, cw, 56};
        const bool clicked = ui_.Row("rcvd/" + f.path.string(), row);
        paint_.RoundRect({row.x + 16, row.y + 15, 20, 26}, 3, WithAlpha(th::kPrimary, 0.85F));
        paint_.Text(paint_.Fit(f.name, cw - 200, 15, Weight::kSemiBold), row.x + 50, row.y + 8, 15, Weight::kSemiBold, th::kText);
        paint_.Text(FormatBytes(f.bytes) + " · " + Ago(received_times_[f.path.string()]), row.x + 50, row.y + 30, 12,
                    Weight::kRegular, th::kTextDim);
        paint_.Text("Open", row.x + cw - 20, row.y + 18, 13, Weight::kSemiBold, Mix(th::kTextDim, th::kPrimary, ui_.Anim("rcvd/" + f.path.string()).value),
                    Align::kRight);
        if (clicked) SDL_OpenURL(FileUrl(f.path).c_str());
        y += 62;
    }
}

}  // namespace fileflow::desktop
