#include <fileflow/live.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fileflow {
namespace {

std::string Percent(double p) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::floor(p * 100.0)));
    return buf;
}

std::string Size(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::string Remaining(double s) {
    if (!(s >= 0.0)) return {};
    char buf[48];
    if (s < 1.0) return " — almost done";
    if (s < 90.0) {
        std::snprintf(buf, sizeof(buf), " — about %.0f s left", std::ceil(s));
    } else {
        std::snprintf(buf, sizeof(buf), " — about %.0f min left", std::ceil(s / 60.0));
    }
    return buf;
}

}  // namespace

std::string_view LivePhaseName(LivePhase p) noexcept {
    switch (p) {
        case LivePhase::kLooking: return "looking";
        case LivePhase::kLocked: return "locked";
        case LivePhase::kReading: return "reading";
        case LivePhase::kReceiving: return "receiving";
        case LivePhase::kVerified: return "verified";
        case LivePhase::kFailed: return "failed";
    }
    return "unknown";
}

std::string_view LiveHintName(LiveHint h) noexcept {
    switch (h) {
        case LiveHint::kNone: return "none";
        case LiveHint::kPointAtScreen: return "point_at_screen";
        case LiveHint::kHoldSteadier: return "hold_steadier";
        case LiveHint::kNotReadable: return "not_readable";
    }
    return "unknown";
}

Result<LiveReceiver> LiveReceiver::Create(GridGeometry grid, LiveConfig cfg) {
    if (cfg.window_frames == 0 || cfg.lost_after_frames < 1) return Error::kValueOutOfRange;
    if (!(cfg.exposure_s > 0.0) || !(cfg.max_blur_cells > 0.0) ||
        !(cfg.severe_blur_cells >= cfg.max_blur_cells)) {
        return Error::kValueOutOfRange;
    }
    FF_ASSIGN_OR_RETURN(auto layout, FrameLayout::Create(grid, LayoutConfig{}));
    auto owned = std::make_unique<FrameLayout>(std::move(layout));
    FF_ASSIGN_OR_RETURN(auto pipeline, FramePipeline::Create(*owned, cfg.pipeline));
    FF_ASSIGN_OR_RETURN(auto session, ReceiveSession::Create(*owned, cfg.receive));
    return LiveReceiver(std::move(owned), std::make_unique<FramePipeline>(std::move(pipeline)),
                        std::make_unique<ReceiveSession>(std::move(session)), cfg);
}

const LiveStatus& LiveReceiver::Ingest(const ImageView8& img, std::int64_t timestamp_ns) {
    ++status_.frames;
    // Finished either way: keep reporting the outcome, spend nothing more on frames.
    if (status_.phase == LivePhase::kVerified || status_.phase == LivePhase::kFailed) {
        return status_;
    }

    const std::uint64_t geo_fail_before = pipeline_->diagnostics().geometry_failures;
    CapturedFrame cf = pipeline_->Process(img, status_.frames, timestamp_ns);
    status_.screen_in_view = pipeline_->diagnostics().geometry_failures == geo_fail_before;
    frames_without_screen_ = status_.screen_in_view ? 0 : frames_without_screen_ + 1;

    bool read = false;
    if (pipeline_->last_ok()) {
        const FrameOutcome o = session_->Ingest(cf.cell_samples, timestamp_ns);
        read = o != FrameOutcome::kUnreadable && o != FrameOutcome::kHeaderFailed;
    }
    read_window_.push_back(read);
    while (read_window_.size() > cfg_.window_frames) read_window_.pop_front();

    if (session_->complete() && !result_) result_ = session_->Finish();
    Decide(timestamp_ns);
    return status_;
}

void LiveReceiver::Decide(std::int64_t timestamp_ns) {
    LiveStatus& s = status_;

    // ---- measurements ----
    const GridGeometry g = layout_->geometry();
    const double diag_cells =
        std::hypot(static_cast<double>(g.cols), static_cast<double>(g.rows));
    s.motion_blur_cells = pipeline_->tracker().speed() * diag_cells * cfg_.exposure_s;
    s.recent_read_fraction =
        static_cast<double>(std::count(read_window_.begin(), read_window_.end(), true)) /
        static_cast<double>(read_window_.size());
    // Never backwards: the person reads a drop as a fault, and the receiver never loses data.
    s.progress = std::max(s.progress, session_->progress());
    if (const FileManifest* m = session_->manifest()) {
        s.file_name = m->file_name;
        s.file_size = m->file_size;
    }

    // Time remaining from the recent progress rate. Only while data is actually arriving: a pause
    // spent off-screen would otherwise read as "hours left", which is both wrong and alarming.
    if (timestamp_ns > 0 && session_->has_manifest()) {
        progress_history_.emplace_back(timestamp_ns, s.progress);
        const auto horizon = static_cast<std::int64_t>(cfg_.eta_window_s * 1e9);
        while (progress_history_.size() > 2 &&
               timestamp_ns - progress_history_.front().first > horizon) {
            progress_history_.pop_front();
        }
        const auto& [t0, p0] = progress_history_.front();
        const double dt = static_cast<double>(timestamp_ns - t0) * 1e-9;
        const double rate = dt > 0.5 ? (s.progress - p0) / dt : 0.0;
        s.seconds_remaining = rate > 1e-6 ? (1.0 - s.progress) / rate : -1.0;
    }
    // The rate above still remembers the seconds before a pause. Showing "3 s left" while the
    // camera points at the floor would be a promise the receiver cannot keep.
    if (frames_without_screen_ >= cfg_.lost_after_frames || s.recent_read_fraction == 0.0) {
        s.seconds_remaining = -1.0;
    }

    // ---- verdict ----
    if (result_) {
        if (result_->ok()) {
            s.phase = LivePhase::kVerified;
            s.hint = LiveHint::kNone;
            s.progress = 1.0;
            s.seconds_remaining = 0.0;
            s.message = "Received \"" + s.file_name + "\" (" + Size(s.file_size) +
                        ") — verified. You can lower the phone.";
        } else {
            s.phase = LivePhase::kFailed;
            s.hint = LiveHint::kNone;
            s.message = "The file arrived but failed its integrity check, so nothing was "
                        "saved. Start the transfer again.";
        }
        return;
    }

    const bool lost = frames_without_screen_ >= cfg_.lost_after_frames;
    const bool started = s.progress > 0.0;
    const std::string where = started ? " " + Percent(s.progress) + " received." : "";

    if (session_->has_manifest()) {
        s.phase = LivePhase::kReceiving;
    } else if (s.recent_read_fraction > 0.0 || session_->stats().header_ok > 0) {
        s.phase = LivePhase::kReading;
    } else if (s.screen_in_view) {
        s.phase = LivePhase::kLocked;
    } else {
        s.phase = LivePhase::kLooking;
    }

    if (lost) {
        // The most important message the receiver ever shows, because it is the moment someone
        // assumes the transfer has failed. It has not: everything received so far is kept.
        s.hint = LiveHint::kPointAtScreen;
        s.message = started ? "Paused at " + Percent(s.progress) +
                                  " — point the camera back at the sending screen. Nothing "
                                  "received so far is lost."
                            : "Point the camera at the sending screen.";
    } else if ((s.motion_blur_cells > cfg_.max_blur_cells &&
                s.recent_read_fraction < cfg_.steady_read_fraction) ||
               s.motion_blur_cells > cfg_.severe_blur_cells) {
        s.hint = LiveHint::kHoldSteadier;
        s.message = "Hold steadier — the phone is moving too fast to read." + where;
    } else if (s.screen_in_view && read_window_.size() >= cfg_.window_frames / 2 &&
               s.recent_read_fraction == 0.0) {
        s.hint = LiveHint::kNotReadable;
        s.message = "Screen found but not readable yet — hold still, and move closer or "
                    "refocus." + where;
    } else {
        s.hint = LiveHint::kNone;
        switch (s.phase) {
            case LivePhase::kReceiving:
                s.message = "Receiving " + (s.file_name.empty() ? std::string("file")
                                                                 : "\"" + s.file_name + "\"") +
                            " — " + Percent(s.progress) + Remaining(s.seconds_remaining) + ".";
                break;
            case LivePhase::kReading:
                s.message = "Reading — waiting for the file details." + where;
                break;
            case LivePhase::kLocked:
                s.message = "Screen found — reading…";
                break;
            default:
                s.message = "Point the camera at the sending screen.";
                break;
        }
    }
}

Result<std::vector<std::uint8_t>> LiveReceiver::File() const {
    if (!result_) return Error::kFountainIncomplete;
    return *result_;
}

}  // namespace fileflow
