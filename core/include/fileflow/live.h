// Live receiver (C13b): camera frames in, a verified file out, and at every frame a status a
// person holding the phone can act on.
//
// The receive path used to be record-then-analyse: the phone wrote a fixed number of frames to disk
// and a desktop tool decoded them afterwards. That is right for experiments and useless to someone
// receiving a file -- they cannot know whether it worked, how long is left, or that the phone has
// drifted off the screen until it is too late. And a handheld phone WILL drift: the transfer has to
// survive it without the person needing to understand why.
//
// It survives by construction. Fountain coding makes a frame lost to motion a non-event -- nothing
// already received is ever discarded, and progress resumes the moment the screen is back -- so the
// only job left is to TELL the person what to do. This class owns that: one phase, one hint and one
// plain sentence per frame, decided here in portable C++ where the desktop suite can test it
// (ADR-0014), not reassembled from raw numbers in Kotlin.
#pragma once

#include <fileflow/grid.h>
#include <fileflow/image.h>
#include <fileflow/pipeline.h>
#include <fileflow/result.h>
#include <fileflow/session.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fileflow {

struct LiveConfig {
    PipelineConfig pipeline;
    ReceiveConfig receive;

    // Frames of history behind "is anything being read right now".
    std::size_t window_frames = 30;
    // Consecutive frames without the screen before the person is told to point back at it. Short
    // enough to react within a third of a second at 30 fps, long enough that one blurred frame does
    // not flash a warning.
    int lost_after_frames = 8;
    // Exposure the camera is running at, and when to say "hold steadier". Measured as the screen's
    // motion DURING one exposure, in cells -- the quantity that smears cells into neighbours --
    // rather than an arbitrary speed.
    //
    // Two conditions, because a nag that fires while everything works teaches people to ignore it.
    // From LiveReceiver.DISABLED_BlurCalibrationTable (F54), mean / peak estimated blur in cells
    // and the header success that came with it:
    //   shake 1  0.29 / 0.49  1.00        shake 4  0.89 / 1.64  0.68
    //   shake 2  0.57 / 0.95  1.00        pan 0.6  0.93 / 1.34  1.00
    //   shake 3  0.85 / 1.40  0.89        pan 1.0  0.98 / 1.92  0.02
    // Blur alone does not separate pan 0.6 (fine) from pan 1.0 (dead), so it is combined with what
    // is actually being read: blur over `max_blur_cells` AND fewer than `steady_read_fraction` of
    // recent frames read, or blur over `severe_blur_cells` regardless. [HYP] until a real
    // handheld capture repeats the table.
    double exposure_s = 0.015;
    double max_blur_cells = 0.75;
    double steady_read_fraction = 0.8;
    double severe_blur_cells = 1.5;
    // Progress-rate history behind the time-remaining estimate.
    double eta_window_s = 4.0;
};

enum class LivePhase : std::uint8_t {
    kLooking = 0,  // no screen in view
    kLocked,       // screen found, nothing readable yet
    kReading,      // frames readable, waiting for the file details (manifest)
    kReceiving,    // the transfer is running
    kVerified,     // complete, and the SHA-256 matched
    kFailed,       // complete, and it did NOT match: nothing is delivered (G3)
};

enum class LiveHint : std::uint8_t {
    kNone = 0,
    kPointAtScreen,  // the screen is not in view (or not for long enough)
    kHoldSteadier,   // in view, but moving fast enough to smear cells
    kNotReadable,    // in view and steady, but nothing decodes: distance, focus, glare
};

[[nodiscard]] std::string_view LivePhaseName(LivePhase p) noexcept;
[[nodiscard]] std::string_view LiveHintName(LiveHint h) noexcept;

struct LiveStatus {
    LivePhase phase = LivePhase::kLooking;
    LiveHint hint = LiveHint::kPointAtScreen;
    std::string message = "Point the camera at the sending screen.";

    double progress = 0.0;            // 0..1, never decreases
    double seconds_remaining = -1.0;  // < 0 = no estimate yet
    double motion_blur_cells = 0.0;   // screen motion during one exposure, in cells
    double recent_read_fraction = 0.0;  // frames in the window whose header decoded
    bool screen_in_view = false;
    std::uint64_t frames = 0;

    std::string file_name;            // a display hint (sanitised), once the manifest is in
    std::uint64_t file_size = 0;
};

class LiveReceiver {
  public:
    static Result<LiveReceiver> Create(GridGeometry grid, LiveConfig cfg = {});

    // One camera frame. Frames may be skipped freely -- a receiver too slow to process every frame
    // just reads fewer of them -- because nothing depends on seeing consecutive ones.
    const LiveStatus& Ingest(const ImageView8& img, std::int64_t timestamp_ns);

    [[nodiscard]] const LiveStatus& status() const noexcept { return status_; }

    // The verified file, once `status().phase == kVerified`. kFountainIncomplete before then, and
    // kHashMismatch after a failed verification -- never partial or unverified bytes.
    [[nodiscard]] Result<std::vector<std::uint8_t>> File() const;

    [[nodiscard]] const FramePipeline& pipeline() const noexcept { return *pipeline_; }
    [[nodiscard]] const ReceiveSession& session() const noexcept { return *session_; }

  private:
    LiveReceiver(std::unique_ptr<FrameLayout> layout, std::unique_ptr<FramePipeline> pipeline,
                 std::unique_ptr<ReceiveSession> session, LiveConfig cfg)
        : layout_(std::move(layout)),
          pipeline_(std::move(pipeline)),
          session_(std::move(session)),
          cfg_(cfg) {}

    void Decide(std::int64_t timestamp_ns);

    // Heap-held so their addresses survive moves: the pipeline and the session keep pointers to
    // the layout.
    std::unique_ptr<FrameLayout> layout_;
    std::unique_ptr<FramePipeline> pipeline_;
    std::unique_ptr<ReceiveSession> session_;
    LiveConfig cfg_;

    LiveStatus status_;
    std::deque<bool> read_window_;
    int frames_without_screen_ = 0;
    std::deque<std::pair<std::int64_t, double>> progress_history_;
    std::optional<Result<std::vector<std::uint8_t>>> result_;
};

}  // namespace fileflow
