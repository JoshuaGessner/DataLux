// Receive and Received: a phone's sending screen held up to this computer's camera, the file it
// carries saved to Downloads/FileFlow -- and the list of what has come, as on the phone.
#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace fileflow::desktop {

namespace th = theme;

namespace {

constexpr auto kTrackingFreshFor = std::chrono::milliseconds(300);
constexpr Rgba kPatternFound = Hex(0xE8B86A);

void DrawOutline(Painter& paint, const std::array<SDL_FPoint, 4>& points, Rgba color) {
    std::vector<SDL_FPoint> edge(4);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto a = points[i];
        const auto b = points[(i + 1) % points.size()];
        const float length = std::hypot(b.x - a.x, b.y - a.y);
        if (length < 1.0F) continue;
        const float nx = (b.y - a.y) / length;
        const float ny = (a.x - b.x) / length;
        const auto stroke = [&](float radius, Rgba tint) {
            const float x = nx * radius;
            const float y = ny * radius;
            edge = {{a.x - x, a.y - y}, {b.x - x, b.y - y}, {b.x + x, b.y + y}, {a.x + x, a.y + y}};
            paint.Polygon(edge, tint);
        };
        stroke(2.8F, Hex(0x000000, 0.7F));
        stroke(1.4F, color);
    }
}

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

std::string Decimal(double value) {
    char text[48];
    std::snprintf(text, sizeof text, "%.1f", value);
    return text;
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
    ui_.Anim("rx-progress").value = 0;
    receiver_ = std::make_unique<Receiver>(Keys(), settings_.encrypted);
    if (!options_.no_camera) rx_camera_.Open(rx_camera_choice_);
}

void App::StopReceiving() {
    receiver_.reset();  // joins its thread
    rx_camera_.Close();
    rx_camera_picker_open_ = false;
}

void App::ReceiveCameraPicker(float x, float w) {
    constexpr std::size_t kPageSize = 5;
    const std::size_t count = rx_camera_devices_.size() + 1;
    const std::size_t last_page = (count - 1) / kPageSize;
    rx_camera_page_ = std::min(rx_camera_page_, last_page);
    paint_.Text("Choose a camera", x, 88, 20, Weight::kSemiBold, th::kText);
    const char* help = rx_camera_devices_.empty() ? "Connect a camera and it will appear here."
        : receiver_ ? "Switching cameras restarts receiving."
                    : "Choose a camera, then select Start receiving.";
    paint_.Text(help, x, 120, 14, Weight::kRegular, th::kTextDim);
    const auto start = rx_camera_page_ * kPageSize;
    float y = 156;
    for (std::size_t i = start; i < std::min(count, start + kPageSize); ++i) {
        const auto id = i == 0 ? 0 : rx_camera_devices_[i - 1].id;
        const auto name = i == 0 ? std::string("Automatic") : rx_camera_devices_[i - 1].name;
        const bool selected = id == rx_camera_choice_;
        const bool clicked = ui_.Row("rx-device-" + std::to_string(id), {x, y, w, 42});
        paint_.Text(paint_.Fit(name, w - 120, 14, Weight::kSemiBold), x + 16, y + 11, 14, Weight::kSemiBold, th::kText);
        if (selected) paint_.Text("Selected", x + w - 16, y + 11, 14, Weight::kRegular, th::kPrimary, Align::kRight);
        if (clicked) {
            rx_camera_picker_open_ = false;
            if (!selected) {
                rx_camera_choice_ = id;
                if (receiver_) {
                    StopReceiving();
                    StartReceiving();
                }
            }
            return;
        }
        y += 48;
    }
    if (rx_camera_page_ > 0 && ui_.Button("rx-device-prev", {x, y + 8, 100, 40}, "Previous", false)) --rx_camera_page_;
    if (rx_camera_page_ < last_page && ui_.Button("rx-device-next", {x + w - 100, y + 8, 100, 40}, "Next", false)) ++rx_camera_page_;
}

void App::ReceiveScreen(float x0, float W, float H) {
    paint_.Clear(th::kBackground);
    const float L = x0 + 36;
    const float cw = std::min(W - x0 - 72, 760.0F);
    paint_.Text("Receive", L, 24, 28, Weight::kSemiBold, th::kText);

    if (receiver_) {
        if (rx_camera_.Poll(renderer_)) {
            receiver_->Offer(rx_camera_.luma(), rx_camera_.u(), rx_camera_.v(), rx_camera_.width(), rx_camera_.height(),
                             rx_camera_.timestamp_ns(), true);
        }
        if (auto r = receiver_->Result()) {
            rx_result_ = std::move(r);
            StopReceiving();
            if (!rx_result_->files.empty()) received_dirty_ = true;
            return;
        }
    }
    const LiveStatus s = receiver_ ? receiver_->Status() : LiveStatus{};
    const bool finishing = s.phase == LivePhase::kVerified || s.phase == LivePhase::kFailed;
    if (!options_.no_camera && !finishing && !rx_result_ &&
        ui_.Button("rx-camera-picker", {L + cw - 106, 20, 106, 38}, rx_camera_picker_open_ ? "Back" : "Camera", false)) {
        rx_camera_picker_open_ = !rx_camera_picker_open_;
        if (rx_camera_picker_open_) {
            rx_camera_devices_ = Camera::Devices();
            rx_camera_page_ = 0;
        }
    }
    if (rx_camera_picker_open_ && !finishing) {
        ReceiveCameraPicker(L, cw);
        return;
    }

    // Receiving: the camera view and current tracking outline use the same fitted rectangle.
    if (receiver_) {
        const auto diagnostics = receiver_->Diagnostics();
        const float vh = std::max(120.0F, std::min(H - (rx_diagnostics_open_ ? 430 : 210), cw * 0.62F));
        const SDL_FRect view{L, 76, cw, vh};
        paint_.RoundRect(view, 22, Hex(0x000000));
        const auto state = rx_camera_.state();
        const auto& tracking = diagnostics.tracking;
        const bool found = state == Camera::State::kOn && tracking && Clock::now() - tracking->offered_at <= kTrackingFreshFor &&
            tracking->width == rx_camera_.width() && tracking->height == rx_camera_.height();
        const bool productive = state == Camera::State::kOn && diagnostics.useful_progress_age_ms >= 0 &&
            diagnostics.useful_progress_age_ms <= 3000;
        if (rx_camera_.preview() != nullptr) {
            const float aspect = static_cast<float>(rx_camera_.width()) / static_cast<float>(std::max(1, rx_camera_.height()));
            float w = cw;
            float h = w / aspect;
            if (h > vh) {
                h = vh;
                w = h * aspect;
            }
            const float k = paint_.scale();
            // Mirrored, as a camera facing you is: moving the phone left moves it left here.
            const SDL_FRect preview{view.x + (view.w - w) / 2, view.y + (view.h - h) / 2, w, h};
            const SDL_FRect dst{preview.x * k, preview.y * k, preview.w * k, preview.h * k};
            SDL_RenderTextureRotated(renderer_, rx_camera_.preview(), nullptr, &dst, 0, nullptr, SDL_FLIP_HORIZONTAL);
            if (found) DrawOutline(paint_, detail::MirrorCameraOutline(tracking->corners, preview),
                                   productive ? th::kTertiary : kPatternFound);
        }
        const bool camera_problem = state == Camera::State::kNone || state == Camera::State::kDenied || state == Camera::State::kError;
        if (state != Camera::State::kOn) {
            const std::string msg = state == Camera::State::kNone ? "Connect a camera, then retry."
                                    : state == Camera::State::kDenied ? "Allow DataLux in your system's camera privacy settings, then retry."
                                    : state == Camera::State::kError ? rx_camera_.problem()
                                    : options_.no_camera ? "The camera is disabled for this run."
                                                         : "Starting the camera… allow access if asked.";
            paint_.RoundRect({L + 12, 76 + vh / 2 - 28, cw - 24, 56}, 14, WithAlpha(th::kSurface, 0.94F));
            paint_.Text(paint_.Fit(msg, cw - 48, 14, Weight::kRegular), L + cw / 2, 76 + vh / 2 - 8, 14,
                        Weight::kRegular, th::kText, Align::kCenter);
        }
        // The status: what to do, or what's coming.
        const std::string said = camera_problem ? "Camera needs attention"
            : s.phase == LivePhase::kVerified ? "File verified · saving…"
            : s.phase == LivePhase::kFailed ? "The file did not verify"
            : s.sender_switches > 0 && s.session_id == 0 ? "Sender changed the transfer · restarting"
            : s.phase == LivePhase::kReceiving && productive ? "Receiving data"
            : found ? (s.hint == LiveHint::kHoldSteadier ? "Pattern found · hold steadier"
                        : s.hint == LiveHint::kNotReadable ? "Pattern found · adjust distance or reduce glare"
                        : "Pattern found · keep the whole code in view")
            : "Align the sending screen";
        const float pw = std::min(cw - 40, paint_.Measure(said, 15, Weight::kSemiBold) + 60);
        const SDL_FRect pill{L + (cw - pw) / 2, 76 + vh - 64, pw, 44};
        paint_.RoundRect(pill, 22, WithAlpha(th::kSurface, 0.92F));
        paint_.Circle(pill.x + 22, pill.y + 22, 4.5F, camera_problem ? th::kError : productive ? th::kTertiary
                                                                    : found ? kPatternFound : th::kTextDim);
        paint_.Text(paint_.Fit(said, pw - 50, 15, Weight::kSemiBold), pill.x + 36, pill.y + 12, 15, Weight::kSemiBold, th::kText);

        // Progress, speed and time left.
        float y = 76 + vh + 22;
        const SDL_FRect track{L, y, cw, 8};
        paint_.RoundRect(track, 4, th::kSurfaceHigh);
        Ease& bar = ui_.Anim("rx-progress");
        const auto progress = static_cast<float>(std::clamp(s.progress, 0.0, s.phase == LivePhase::kVerified ? 1.0 : 0.99));
        bar.value = std::min(bar.value, progress);
        bar.To(progress, dt_, 0.25F);
        if (bar.value > 0.002F) paint_.RoundRect({L, y, std::max(8.0F, cw * bar.value), 8}, 4, th::kPrimary);
        y += 20;
        std::string line = std::to_string(static_cast<int>(std::lround(progress * 100))) + "%";
        if (productive && s.bytes_per_second > 0) line += "  ·  " + Rate(s.bytes_per_second);
        if (productive && s.seconds_remaining >= 0) line += "  ·  about " + std::to_string(static_cast<int>(std::ceil(s.seconds_remaining))) + " s left";
        paint_.Text(paint_.Fit(line, std::max(80.0F, cw - 260), 14, Weight::kRegular), L, y, 14, Weight::kRegular, th::kTextDim);
        if (finishing) return;
        if (camera_problem && ui_.Button("rx-camera-retry", {L + cw - 244, y - 6, 116, 42}, "Retry camera", true)) {
            StopReceiving();
            StartReceiving();
            return;
        }
        if (!camera_problem && ui_.Button("rx-details", {L + cw - 244, y - 6, 116, 42}, rx_diagnostics_open_ ? "Hide details" : "Details", false)) {
            rx_diagnostics_open_ = !rx_diagnostics_open_;
        }
        if (ui_.Button("rx-stop", {L + cw - 120, y - 6, 120, 42}, "Stop", false)) StopReceiving();
        if (rx_diagnostics_open_ && receiver_) {
            const auto& camera = rx_camera_.diagnostics();
            const auto& mode = camera.negotiated;
            const auto& delivery = diagnostics.delivery;
            const double fps = mode.framerate_denominator > 0 ? static_cast<double>(mode.framerate_numerator) / mode.framerate_denominator : 0;
            const double decoded = static_cast<double>(diagnostics.decoded);
            const double captured = static_cast<double>(camera.frames);
            const double accepted = static_cast<double>(delivery.accepted);
            std::vector<std::string> details;
            details.push_back(camera.have_negotiated ? "SDL delivered: " + std::to_string(mode.width) + " × " + std::to_string(mode.height) +
                " · " + Decimal(fps) + " fps · " + SDL_GetPixelFormatName(mode.format) + " · colorspace " + std::to_string(mode.colorspace)
                : "SDL delivered mode: waiting for camera access");
            details.push_back("Capture: " + std::to_string(camera.frames) + " · timestamp interval avg/min/max: " +
                Decimal(camera.timestamp_intervals > 0 ? camera.interval_ms_sum / static_cast<double>(camera.timestamp_intervals) : 0) + "/" +
                Decimal(camera.interval_ms_min) + "/" + Decimal(camera.interval_ms_max) + " ms · stale " + std::to_string(camera.nonmonotonic_timestamps));
            details.push_back("Convert + preview avg/max: " + Decimal(captured > 0 ? camera.conversion_ms_sum / captured : 0) + "/" +
                Decimal(camera.conversion_ms_max) + " ms · pending copy " + Decimal(accepted > 0 ? delivery.copy_ms_sum / accepted : 0) + "/" + Decimal(delivery.copy_ms_max) + " ms");
            details.push_back("Offered/accepted/replaced: " + std::to_string(delivery.offered) + "/" + std::to_string(delivery.accepted) + "/" +
                std::to_string(delivery.replaced) + " · stale/invalid " + std::to_string(delivery.stale) + "/" + std::to_string(delivery.invalid));
            details.push_back("Decoded/offered: " + std::to_string(diagnostics.decoded) + "/" + std::to_string(delivery.offered) + " (" +
                Decimal(delivery.offered > 0 ? 100 * decoded / static_cast<double>(delivery.offered) : 0) + "%) · one worker, one latest pending");
            details.push_back("Decode wall avg/max: " + Decimal(decoded > 0 ? diagnostics.decode_ms_sum / decoded : 0) + "/" +
                Decimal(diagnostics.decode_ms_max) + " ms · application queue " + Decimal(decoded > 0 ? diagnostics.queue_ms_sum / decoded : 0) + "/" +
                Decimal(diagnostics.queue_ms_max) + " ms");
            const auto& core = s.diagnostics;
            const double timed = static_cast<double>(core.timed_frames);
            const auto mean = [timed](double total) { return Decimal(timed > 0 ? total / timed : 0); };
            details.push_back("Selected grid stage means, ms: pipeline " + mean(core.pipeline_ms) + " · header " + mean(core.header_ms) +
                " · demod " + mean(core.demod_ms) + " · FEC " + mean(core.fec_ms) + " · combine " + mean(core.combine_ms));
            details.push_back("Lock wait / commit: " + mean(core.lock_wait_ms) + " / " + mean(core.commit_ms) + " ms · headers " +
                std::to_string(s.header_frames) + " · FEC / CRC failures " + std::to_string(s.fec_failed_frames) + " / " + std::to_string(s.crc_failed_frames));
            details.push_back("Pipeline stages, ms: track " + mean(core.tracking_ms) + " · align " + mean(core.alignment_ms) +
                " · luma " + mean(core.luma_sampling_ms) + " · photometry " + mean(core.photometry_ms) + " · chroma " + mean(core.color_sampling_ms));
            details.push_back("Useful symbols: " + std::to_string(s.useful_symbols) + " · candidate/session-scoped counters; manifests are not file progress.");
            details.push_back("Capture timestamp and application queue clocks are separate; no sensor-to-app age inferred.");
            y += 44;
            for (const auto& detail : details) {
                paint_.Text(paint_.Fit(detail, cw, 11, Weight::kRegular), L, y, 11, Weight::kRegular, th::kTextDim);
                y += 18;
            }
        }
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
    const float side = 120;
    const SDL_FRect box{cx - side / 2, top + 10, side, side};
    const float beam = 0.5F + 0.45F * std::sin(t_ * 1.8F);
    paint_.Glow(cx, box.y + side * beam, side * 0.45F, WithAlpha(th::kTertiary, 0.35F));
    paint_.RoundRect({box.x + 10, box.y + side * beam - 1, side - 20, 2.5F}, 1.2F, th::kTertiary);
    for (const auto& [sx, sy] : {std::pair{-1.0F, -1.0F}, std::pair{1.0F, -1.0F}, std::pair{-1.0F, 1.0F}, std::pair{1.0F, 1.0F}}) {
        const float x = sx < 0 ? box.x : box.x + side - 26;
        const float y = sy < 0 ? box.y : box.y + side - 26;
        paint_.RoundRect({sx < 0 ? x : x + 22, y, 4, 26}, 2, th::kText);
        paint_.RoundRect({x, sy < 0 ? y : y + 22, 26, 4}, 2, th::kText);
    }
    paint_.Text("Receive a file", cx, top + 160, 26, Weight::kSemiBold, th::kText, Align::kCenter);
    paint_.Text("Hold the sender's screen up to this camera, with the entire code in the picture.", cx, top + 200, 15,
                Weight::kRegular, th::kTextDim, Align::kCenter);
    paint_.Text("Start with Steady; try Fast once the camera reads reliably.", cx, top + 224, 15, Weight::kRegular,
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
