#include "receiver.h"

#include <SDL3/SDL.h>

#include <fileflow/transfer.h>

#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace fileflow::desktop {

namespace {

// The grids a sender might be using (the app's Grids.ALL), most likely first.
constexpr GridGeometry kGrids[] = {{120, 260}, {144, 240}, {180, 390}, {84, 182}};

std::string SafeReceivedName(const std::string& name) {
    std::string safe = SanitiseFileName(name);
    // Also safe on Windows: no drive-qualified path, alternate data stream, or
    // device name. Apply on all hosts so transferred names behave consistently.
    for (char& c : safe) {
        if (std::string_view("<>:\"|?*").find(c) != std::string_view::npos) c = '_';
    }
    while (!safe.empty() && (safe.back() == '.' || safe.back() == ' ')) safe.pop_back();
    if (safe.empty()) return "received.bin";
    auto stem = safe.substr(0, safe.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9')) {
        safe.insert(safe.begin(), '_');
    }
    return safe;
}

// Claim the name atomically: an existence check followed by an ordinary open can overwrite
// another receiver's file, or follow a dangling symlink outside the downloads folder.
std::optional<SavedFile> Save(const std::filesystem::path& dir, const std::string& name, const secure::Bytes& data) {
    const std::string safe = SafeReceivedName(name);
    std::filesystem::path p = dir / std::filesystem::path(std::u8string(safe.begin(), safe.end()));
    const auto stem = p.stem().u8string();
    const auto ext = p.extension().u8string();
    for (int i = 0; i < 10000; ++i) {
        if (i != 0) {
            const auto suffix = std::to_string(i);
            p = dir / (stem + u8" (" + std::u8string(suffix.begin(), suffix.end()) + u8")" + ext);
        }
#if defined(_WIN32)
        std::FILE* out = _wfopen(p.c_str(), L"wbx");
#else
        std::FILE* out = std::fopen(p.c_str(), "wbx");
#endif
        if (out == nullptr) {
            if (errno == EEXIST) continue;
            return std::nullopt;
        }
        const bool written = data.empty() || std::fwrite(data.data(), 1, data.size(), out) == data.size();
        const bool closed = std::fclose(out) == 0;  // buffered writes can fail only on close
        if (!written || !closed) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
            return std::nullopt;
        }
        const auto u = p.filename().u8string();
        return SavedFile{std::string(u.begin(), u.end()), p, data.size()};
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path ReceivedFolder(bool create) {
    const char* downloads = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    std::filesystem::path base = downloads != nullptr ? std::filesystem::path(downloads) : std::filesystem::path(".");
    auto dir = base / "FileFlow";
    std::error_code ec;
    if (create) std::filesystem::create_directories(dir, ec);
    return dir;
}

Receiver::Receiver(std::vector<secure::SealKey> keys, bool require_encrypted, std::filesystem::path dir)
    : keys_(std::move(keys)), require_encrypted_(require_encrypted), dir_(std::move(dir)) {
    for (const auto& g : kGrids) {
        LiveConfig cfg;
        cfg.exposure_s = 1.0 / 60;  // a webcam's, about; only the blur warning reads it
        if (auto r = LiveReceiver::Create(g, cfg); r.ok()) candidates_.push_back(std::make_unique<LiveReceiver>(std::move(r).value()));
    }
    worker_ = std::thread([this] {
        try {
            Run();
        } catch (const std::exception&) {
            frames_.Close();
            std::lock_guard g(mu_);
            ReceiveResult failed;
            failed.problem = "Receiving failed. Check available memory and disk space, then try again.";
            result_ = std::move(failed);
        }
    });
}

Receiver::~Receiver() {
    frames_.Close();
    if (worker_.joinable()) worker_.join();
}

void Receiver::Offer(const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& u, const std::vector<std::uint8_t>& v,
                     int w, int h, std::uint64_t timestamp_ns, bool full_resolution) {
    frames_.Offer(luma, u, v, w, h, timestamp_ns, full_resolution);
}

LiveStatus Receiver::Status() const {
    std::lock_guard g(mu_);
    return status_;
}

std::optional<ReceiveResult> Receiver::Result() const {
    std::lock_guard g(mu_);
    return result_;
}

ReceiveDiagnostics Receiver::Diagnostics() const {
    ReceiveDiagnostics d;
    {
        std::lock_guard g(mu_);
        d = diagnostics_;
        if (last_progress_at_) d.useful_progress_age_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *last_progress_at_).count();
    }
    d.delivery = frames_.Stats();
    return d;
}

void Receiver::Run() {
    if (candidates_.empty()) throw std::runtime_error("No receiver configuration");
    detail::CameraFrame frame;
    LiveReceiver* tracking_candidate = nullptr;
    while (frames_.Take(&frame)) {
        const auto started = std::chrono::steady_clock::now();
        const double queue_ms = std::chrono::duration<double, std::milli>(started - frame.offered_at).count();
        LiveReceiver* rx = committed_ != nullptr ? committed_ : candidates_[next_++ % candidates_.size()].get();
        const ImageView8 img(frame.luma.data(), frame.width, frame.height, frame.width);
        const int cw = frame.full_resolution ? frame.width : frame.width / 2;
        const int ch = frame.full_resolution ? frame.height : frame.height / 2;
        ChromaPlanes chroma{ImageView8(frame.u.data(), cw, ch, cw), ImageView8(frame.v.data(), cw, ch, cw), frame.full_resolution};
        LiveStatus s = rx->Ingest(img, static_cast<std::int64_t>(frame.timestamp_ns), 0, &chroma);
        const auto finished = std::chrono::steady_clock::now();
        const double decode_ms = std::chrono::duration<double, std::milli>(finished - started).count();
        ++decoded_;
        if (committed_ == nullptr && s.payload_frames > 0) committed_ = rx;  // the grid, proven
        const bool show = committed_ == nullptr || rx == committed_;
        std::optional<ReceiveDiagnostics::Tracking> tracking;
        if (show && s.screen_in_view) {
            const auto& pipeline = rx->pipeline();  // this worker owns lane zero
            const auto grid = pipeline.layout().geometry();
            const double cols = grid.cols;
            const double rows = grid.rows;
            std::array<Point2, 4> corners{{{0, 0}, {cols, 0}, {cols, rows}, {0, rows}}};
            for (auto& corner : corners) {
                const auto point = pipeline.last_lens().Apply(pipeline.last_homography().Apply(corner));
                corner = {point.x / frame.width, point.y / frame.height};
            }
            // Edge estimates can straddle the image by a pixel. Reject larger excursions.
            if (std::all_of(corners.begin(), corners.end(), [](const Point2& p) {
                    return p.x >= -0.02 && p.x <= 1.02 && p.y >= -0.02 && p.y <= 1.02;
                })) {
                tracking = ReceiveDiagnostics::Tracking{corners, frame.width, frame.height, frame.offered_at};
            }
        }
        {
            std::lock_guard g(mu_);
            // A failed search in an alternate grid doesn't invalidate the last good candidate.
            // The view still expires its geometry by frame age, even if no frames arrive.
            if (tracking || committed_ != nullptr || tracking_candidate == rx) {
                diagnostics_.tracking = tracking;
                tracking_candidate = tracking ? rx : nullptr;
            }
            // While searching, the furthest-along candidate speaks, so the status doesn't flicker.
            if (show && (committed_ != nullptr || static_cast<int>(s.phase) >= static_cast<int>(status_.phase))) {
                const bool new_session = s.session_id != status_.session_id || s.sender_switches != status_.sender_switches;
                if (new_session) {
                    last_progress_at_.reset();
                    diagnostics_.useful_progress_updates = 0;
                }
                if (s.useful_symbols > (new_session ? 0 : status_.useful_symbols)) {
                    last_progress_at_ = finished;
                    ++diagnostics_.useful_progress_updates;
                }
                status_ = s;
            }
            ++diagnostics_.decoded;
            diagnostics_.decode_ms_sum += decode_ms;
            diagnostics_.decode_ms_max = std::max(diagnostics_.decode_ms_max, decode_ms);
            diagnostics_.queue_ms_sum += queue_ms;
            diagnostics_.queue_ms_max = std::max(diagnostics_.queue_ms_max, queue_ms);
        }
        if (s.phase == LivePhase::kVerified || s.phase == LivePhase::kFailed) {
            frames_.Close();
            Finish(*rx);
            return;
        }
    }
}

// The verified bytes are an envelope: opened with whichever key it's sealed to and saved -- or,
// sealed to a key this computer doesn't hold, or plain while encrypted mode is on, refused with
// nothing written.
void Receiver::Finish(const LiveReceiver& rx) {
    ReceiveResult r;
    const LiveStatus& s = rx.status();
    r.seconds = s.transfer_seconds;
    auto file = rx.File();
    if (s.phase == LivePhase::kFailed || !file.ok()) {
        r.problem = "The file didn't verify, so nothing was saved. Receive it again.";
    } else {
        const auto& bytes = file.value();
        r.link_bytes = bytes.size();
        secure::Opened o;
        const bool envelope = bytes.size() >= 4 && (std::equal(bytes.begin(), bytes.begin() + 4, "FFE2") ||
                                                    std::equal(bytes.begin(), bytes.begin() + 4, "FFE1"));
        secure::OpenFailure f = secure::OpenFailure::kNone;
        if (envelope || require_encrypted_) {
            f = secure::Open(bytes, keys_, require_encrypted_, &o);
        } else {
            // A sender from before envelopes sent the bare file.
            o.name = s.file_name;
            o.data = bytes;
        }
        if (f != secure::OpenFailure::kNone) {
            r.problem = secure::Describe(f);
        } else {
            const auto dir = dir_.empty() ? ReceivedFolder(true) : dir_;
            r.name = o.name;
            r.sealed_to = o.sealed_to;
            if (o.files) {
                for (const auto& [name, data] : *o.files) {
                    if (auto saved = Save(dir, name, data)) {
                        r.bytes += saved->bytes;
                        r.files.push_back(*saved);
                    }
                }
            } else if (auto saved = Save(dir, o.name, o.data)) {
                r.bytes = saved->bytes;
                r.files.push_back(*saved);
            }
            const auto expected = o.files ? o.files->size() : 1U;
            r.ok = r.files.size() == expected;
            if (!r.ok) r.problem = "Saved " + std::to_string(r.files.size()) + " of " + std::to_string(expected) +
                                  " files to " + dir.string() + ". Check the folder and available space, then receive again.";
        }
    }
    std::lock_guard g(mu_);
    status_ = s;
    result_ = std::move(r);
}

}  // namespace fileflow::desktop
