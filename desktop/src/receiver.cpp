#include "receiver.h"

#include <SDL3/SDL.h>

#include <fileflow/transfer.h>

#include <fstream>

namespace fileflow::desktop {

namespace {

// The grids a sender might be using (the app's Grids.ALL), most likely first.
constexpr GridGeometry kGrids[] = {{120, 260}, {144, 240}, {180, 390}};

// [name] in [dir], made unique: "photo.jpg", then "photo (1).jpg", ... -- never over a file.
std::filesystem::path Unique(const std::filesystem::path& dir, const std::string& name) {
    const std::string safe = SanitiseFileName(name).empty() ? "received.bin" : SanitiseFileName(name);
    std::filesystem::path p = dir / std::filesystem::path(std::u8string(safe.begin(), safe.end()));
    const auto stem = p.stem().u8string();
    const auto ext = p.extension().u8string();
    std::error_code ec;
    for (int i = 1; std::filesystem::exists(p, ec) && i < 10000; ++i) {
        p = dir / (stem + u8" (" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(i).c_str())) + u8")" + ext);
    }
    return p;
}

std::optional<SavedFile> Save(const std::filesystem::path& dir, const std::string& name, const secure::Bytes& data) {
    const auto path = Unique(dir, name);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) return std::nullopt;
    const auto u = path.filename().u8string();
    return SavedFile{std::string(u.begin(), u.end()), path, data.size()};
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
    worker_ = std::thread([this] { Run(); });
}

Receiver::~Receiver() {
    {
        std::lock_guard g(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Receiver::Offer(const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& u, const std::vector<std::uint8_t>& v,
                     int w, int h, std::uint64_t timestamp_ns) {
    {
        std::lock_guard g(mu_);
        if (have_frame_ || result_) return;  // busy: this frame is dropped, the next will do
        luma_ = luma;
        u_ = u;
        v_ = v;
        w_ = w;
        h_ = h;
        ts_ = timestamp_ns;
        have_frame_ = true;
    }
    cv_.notify_one();
}

LiveStatus Receiver::Status() const {
    std::lock_guard g(mu_);
    return status_;
}

std::optional<ReceiveResult> Receiver::Result() const {
    std::lock_guard g(mu_);
    return result_;
}

void Receiver::Run() {
    std::vector<std::uint8_t> luma, u, v;
    for (;;) {
        int w = 0;
        int h = 0;
        std::uint64_t ts = 0;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || have_frame_; });
            if (stop_) return;
            luma.swap(luma_);
            u.swap(u_);
            v.swap(v_);
            w = w_;
            h = h_;
            ts = ts_;
        }
        LiveReceiver* rx = committed_ != nullptr ? committed_ : candidates_[next_++ % candidates_.size()].get();
        const ImageView8 img(luma.data(), w, h, w);
        ChromaPlanes chroma{ImageView8(u.data(), w / 2, h / 2, w / 2), ImageView8(v.data(), w / 2, h / 2, w / 2)};
        LiveStatus s = rx->Ingest(img, static_cast<std::int64_t>(ts), 0, &chroma);
        ++decoded_;
        if (committed_ == nullptr && s.payload_frames > 0) committed_ = rx;  // the grid, proven
        const bool show = committed_ == nullptr || rx == committed_;
        {
            std::lock_guard g(mu_);
            // While searching, the furthest-along candidate speaks, so the status doesn't flicker.
            if (show && (committed_ != nullptr || static_cast<int>(s.phase) >= static_cast<int>(status_.phase))) status_ = s;
            have_frame_ = false;
        }
        if (s.phase == LivePhase::kVerified || s.phase == LivePhase::kFailed) {
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
            r.ok = !r.files.empty();
            if (!r.ok) r.problem = "The file verified, but couldn't be saved to " + dir.string() + ".";
        }
    }
    std::lock_guard g(mu_);
    status_ = s;
    result_ = std::move(r);
}

}  // namespace fileflow::desktop
