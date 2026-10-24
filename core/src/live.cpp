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
    if (s < 1.0) return " · almost done";
    if (s < 90.0) {
        std::snprintf(buf, sizeof(buf), " · %.0f s left", std::ceil(s));
    } else {
        std::snprintf(buf, sizeof(buf), " · %.0f min left", std::ceil(s / 60.0));
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
    if (cfg.lanes == 0 || cfg.lanes > kMaxLanes) return Error::kValueOutOfRange;
    if (cfg.switch_after_foreign_frames < 1) return Error::kValueOutOfRange;
    if (!(cfg.eta_window_s > 0.5) || !(cfg.rate_smoothing_s > 0.0) ||
        !(cfg.eta_smoothing_s > 0.0)) {
        return Error::kValueOutOfRange;
    }
    FF_ASSIGN_OR_RETURN(auto layout, FrameLayout::Create(grid, LayoutConfig{}));
    auto owned = std::make_unique<FrameLayout>(std::move(layout));
    std::vector<std::unique_ptr<FramePipeline>> pipelines;
    for (std::size_t i = 0; i < cfg.lanes; ++i) {
        FF_ASSIGN_OR_RETURN(auto pipeline, FramePipeline::Create(*owned, cfg.pipeline));
        pipelines.push_back(std::make_unique<FramePipeline>(std::move(pipeline)));
    }
    FF_ASSIGN_OR_RETURN(auto session, ReceiveSession::Create(*owned, cfg.receive));
    return LiveReceiver(std::move(owned), std::move(pipelines),
                        std::make_unique<ReceiveSession>(std::move(session)), cfg);
}

LiveStatus LiveReceiver::Ingest(const ImageView8& img, std::int64_t timestamp_ns,
                                std::size_t lane, const ChromaPlanes* chroma) {
    std::uint64_t frame_no = 0;
    {
        const std::lock_guard<std::mutex> lock(*mu_);
        frame_no = ++status_.frames;
        // Finished either way: keep reporting the outcome, spend nothing more on frames.
        if (status_.phase == LivePhase::kVerified || status_.phase == LivePhase::kFailed) {
            return status_;
        }
    }

    // The expensive part -- finding, aligning and sampling the screen -- runs in this lane alone,
    // with nothing held. A lane is only ever driven by one thread at a time (the caller's promise).
    FramePipeline& p = *pipelines_[lane % pipelines_.size()];
    const std::uint64_t geo_fail_before = p.diagnostics().geometry_failures;
    CapturedFrame cf = p.Process(img, frame_no, timestamp_ns, chroma);
    const bool in_view = p.diagnostics().geometry_failures == geo_fail_before;

    const std::lock_guard<std::mutex> lock(*mu_);
    if (status_.phase == LivePhase::kVerified || status_.phase == LivePhase::kFailed) {
        return status_;  // another lane finished it while this one was sampling
    }
    status_.screen_in_view = in_view;
    frames_without_screen_ = in_view ? 0 : frames_without_screen_ + 1;

    bool read = false;
    if (p.last_ok()) {
        const FrameOutcome o = session_->Ingest(cf.cell_samples, cf.cell_rgb, timestamp_ns);
        status_.distinct_states = session_->stats().distinct_sequences;
        status_.color_spread = session_->last_color().centroid_spread;
        status_.color_noise = session_->last_color().noise_sigma;
        status_.bits_per_cell = static_cast<std::uint32_t>(session_->last_bits_per_cell());
        status_.combined_frames = session_->stats().fec_ok_combined;
        read = o != FrameOutcome::kUnreadable && o != FrameOutcome::kHeaderFailed;
        switch (o) {
            case FrameOutcome::kSymbol:
            case FrameOutcome::kBuffered:
            case FrameOutcome::kBufferFull:
            case FrameOutcome::kManifest:
            case FrameOutcome::kManifestRejected:
                ++status_.payload_frames;  // passed intra-frame FEC and its CRC
                if (first_payload_ns_ == 0) first_payload_ns_ = timestamp_ns;
                last_payload_ns_ = std::max(last_payload_ns_, timestamp_ns);
                foreign_run_ = 0;
                break;
            case FrameOutcome::kForeignSession:
                ++foreign_run_;
                break;
            default:
                break;
        }
        // The sender has moved on to a DIFFERENT file. Every frame now belongs to another session
        // and is refused, so without this the receiver would sit "reading" forever while nothing
        // arrived. A run of foreign frames with none of ours is unambiguous -- the session id is a
        // hash of the file (jni_transmitter.cpp), so resending the SAME file never triggers it --
        // and the honest thing is to start over on the new one and say so.
        if (foreign_run_ >= cfg_.switch_after_foreign_frames) {
            auto fresh = ReceiveSession::Create(*layout_, cfg_.receive);
            if (fresh.ok()) {
                session_ = std::make_unique<ReceiveSession>(std::move(fresh).value());
                status_.progress = 0.0;
                status_.seconds_remaining = -1.0;
                status_.payload_frames = 0;
                status_.file_name.clear();
                status_.file_size = 0;
                status_.bytes_per_second = 0.0;
                ++status_.sender_switches;
                rate_.Reset();
                first_payload_ns_ = 0;
                last_payload_ns_ = 0;
            }
            foreign_run_ = 0;
        }
    }
    read_window_.push_back(read);
    while (read_window_.size() > cfg_.window_frames) read_window_.pop_front();

    if (session_->complete() && !result_) result_ = session_->Finish();
    Decide(timestamp_ns, p);
    return status_;
}

void TransferRate::Update(std::int64_t timestamp_ns, double progress, bool paused,
                          std::int64_t started_ns) {
    if (timestamp_ns <= last_ns_) return;
    const double dt = last_ns_ > 0 ? static_cast<double>(timestamp_ns - last_ns_) * 1e-9 : 0.0;
    last_ns_ = timestamp_ns;

    paused_ = paused;
    if (paused) {
        // Nothing is arriving, so there is no honest time left. The rate is kept for when the
        // person aims again, and the window restarts then: left in it, the pause would read as a
        // slow transfer for seconds after it ended.
        history_.clear();
        eta_s_ = -1.0;
        return;
    }
    if (!seeded_) {
        // Progress already made arrived over the time since the first data, not in this instant
        // -- 98% of the file when the manifest came last on the first hand-held run (F59) -- so it
        // seeds the rate as an average instead of entering the window as a spike.
        const double since =
            started_ns > 0 ? static_cast<double>(timestamp_ns - started_ns) * 1e-9 : 0.0;
        rate_ = since > 0.5 ? progress / since : 0.0;
        valid_ = rate_ > 0.0;
        seeded_ = true;
    }
    history_.emplace_back(timestamp_ns, progress);
    // The window is `window_s`, stretched until it holds at least `min_progress` of the file. At a
    // slow start data comes in steps seconds apart, and a fixed window that happened to fall
    // between two of them read as no progress at all -- "hours left" on a transfer that was
    // merely slow.
    const auto window = static_cast<std::int64_t>(cfg_.window_s * 1e9);
    const auto longest = static_cast<std::int64_t>(cfg_.max_window_s * 1e9);
    while (history_.size() > 2) {
        const auto& [t1, p1] = history_[1];
        const bool stale = timestamp_ns - history_.front().first > longest;
        const bool enough = timestamp_ns - t1 >= window && progress - p1 >= cfg_.min_progress;
        if (!stale && !enough) break;
        history_.pop_front();
    }
    const auto& [t0, p0] = history_.front();
    const double span = static_cast<double>(timestamp_ns - t0) * 1e-9;
    if (span > 0.5 && dt > 0.0) {
        const double windowed = (progress - p0) / span;
        // The first measurement IS the rate. Smoothed up from zero instead, the first estimate
        // read as eight minutes for a twenty-second transfer and took seconds to come down.
        rate_ = valid_ ? rate_ + (1.0 - std::exp(-dt / cfg_.rate_smoothing_s)) * (windowed - rate_)
                       : windowed;
        valid_ = valid_ || windowed > 0.0;
    }

    // The time left counts down by the clock and glides to each new estimate: a steady transfer
    // reads 30, 29, 28... and a faster one closes the gap over a couple of seconds. It rises three
    // times more slowly than it falls, because a time left that goes UP reads as something going
    // wrong, and most dips in the rate are a moment of shake that is over by the time it would show.
    const double estimate = valid_ && rate_ > 1e-9 ? (1.0 - progress) / rate_ : -1.0;
    if (estimate < 0.0) {
        eta_s_ = -1.0;
    } else if (eta_s_ < 0.0) {
        eta_s_ = estimate;
    } else {
        eta_s_ = std::max(0.0, eta_s_ - dt);
        const double tau = cfg_.eta_smoothing_s * (estimate > eta_s_ ? 3.0 : 1.0);
        eta_s_ += (1.0 - std::exp(-dt / tau)) * (estimate - eta_s_);
    }
}

void LiveReceiver::Decide(std::int64_t timestamp_ns, const FramePipeline& lane) {
    LiveStatus& s = status_;

    // ---- measurements ----
    const GridGeometry g = layout_->geometry();
    const double diag_cells =
        std::hypot(static_cast<double>(g.cols), static_cast<double>(g.rows));
    s.motion_blur_cells = lane.tracker().speed() * diag_cells * cfg_.exposure_s;
    s.recent_read_fraction =
        static_cast<double>(std::count(read_window_.begin(), read_window_.end(), true)) /
        static_cast<double>(read_window_.size());
    // Never backwards: the person reads a drop as a fault, and the receiver never loses data.
    s.progress = std::max(s.progress, session_->progress());
    if (const FileManifest* m = session_->manifest()) {
        s.file_name = m->file_name;
        s.file_size = m->file_size;
        s.content = m->content;
    }

    // Paused when nothing is arriving: showing "3 s left" while the camera points at the floor
    // would be a promise the receiver cannot keep.
    if (session_->has_manifest()) {
        // A pause is also data that has stopped while headers go on reading: the Pixel reading the
        // S26 showed "6845 min left" for a minute of exactly that (F66).
        const bool stalled =
            last_payload_ns_ > 0 && static_cast<double>(timestamp_ns - last_payload_ns_) * 1e-9 >
                                        cfg_.stalled_after_s;
        rate_.Update(timestamp_ns, s.progress,
                     frames_without_screen_ >= cfg_.lost_after_frames ||
                         s.recent_read_fraction == 0.0 || stalled,
                     first_payload_ns_);
        s.bytes_per_second = rate_.rate() * static_cast<double>(s.file_size);
        s.seconds_remaining = rate_.seconds_remaining();
    }

    // ---- verdict ----
    if (result_) {
        if (result_->ok()) {
            s.phase = LivePhase::kVerified;
            s.hint = LiveHint::kNone;
            s.progress = 1.0;
            s.seconds_remaining = 0.0;
            if (first_payload_ns_ > 0 && timestamp_ns > first_payload_ns_) {
                s.transfer_seconds = static_cast<double>(timestamp_ns - first_payload_ns_) * 1e-9;
            }
            s.message = "Received " + s.file_name + " (" + Size(s.file_size) + ") — verified.";
        } else {
            s.phase = LivePhase::kFailed;
            s.hint = LiveHint::kNone;
            s.message = "The file failed its integrity check, so it was not saved. Try again.";
        }
        return;
    }

    const bool lost = frames_without_screen_ >= cfg_.lost_after_frames;
    const bool started = s.progress > 0.0;
    // Messages are SHORT: they are read in a two-line card over a camera picture, by someone holding
    // a phone at arm's length, and the progress bar beside them already shows the percentage.
    const std::string where;

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
                                  ". Aim back at the screen — nothing received so far is lost."
                            : "Point the camera at the sending screen.";
    } else if ((s.motion_blur_cells > cfg_.max_blur_cells &&
                s.recent_read_fraction < cfg_.steady_read_fraction) ||
               s.motion_blur_cells > cfg_.severe_blur_cells) {
        s.hint = LiveHint::kHoldSteadier;
        s.message = "Hold steadier." + where;
    } else if (s.screen_in_view && read_window_.size() >= cfg_.window_frames / 2 &&
               s.recent_read_fraction == 0.0) {
        s.hint = LiveHint::kNotReadable;
        // Filling the picture, not "about 20 cm": the 2.2 MB transfer read fastest at 11 cm with
        // the screen filling the view, ten times the rate it managed at a third of it. F59's
        // failure at 12 cm was the exposure servo, not the distance.
        s.message = "Screen found — fill the picture with it and hold still." + where;
    } else {
        s.hint = LiveHint::kNone;
        switch (s.phase) {
            case LivePhase::kReceiving:
                s.message = "Receiving " + (s.file_name.empty() ? std::string("file")
                                                                 : s.file_name) +
                            Remaining(s.seconds_remaining);
                break;
            case LivePhase::kReading:
                // Data read before the manifest is kept, so say so: on the first hand-held run
                // this read "waiting for the file details" at 0% for 2.5 minutes while 98% of the
                // file was already held (F59).
                s.message = s.payload_frames > 0
                                ? "Receiving — " + std::to_string(s.payload_frames) +
                                      " pieces so far"
                            : s.sender_switches > 0
                                ? "The sender switched files — receiving the new one…"
                                : "Reading…" + where;
                break;
            case LivePhase::kLocked:
                s.message = "Screen found…";
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
