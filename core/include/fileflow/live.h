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
#include <mutex>
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
    // The speed and time-left estimates (TransferRate): the window of progress history behind
    // them, and the time constants of the smoothing on top.
    double eta_window_s = 4.0;
    double rate_smoothing_s = 2.0;
    double eta_smoothing_s = 2.0;
    // No payload for this long counts as a pause for the speed and time left, even while headers
    // still read: a screen whose headers read but whose data does not is not transferring.
    double stalled_after_s = 3.0;

    // Decode LANES: independent pipelines (tracker, alignment, sampler) that may run on separate
    // threads at once, feeding one session. One phone core decodes 20-50 ms of a 4080-wide frame
    // (F57), so a single lane cannot keep up with a 60 fps camera; three can (F60). Each lane sees
    // only the frames given to it, so its tracker searches over a longer gap -- which it handles,
    // because a frame it cannot track is simply reacquired.
    std::size_t lanes = 1;

    // Consecutive readable frames from a DIFFERENT session before this receiver abandons its
    // transfer and starts on the new one: about a second at 30 fps. Longer than any stray frame
    // from a second sender in view; short enough that a person who switched files on the sender
    // is not left watching a frozen bar.
    int switch_after_foreign_frames = 30;
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
    // Verified-file bytes per second right now, smoothed: goodput's rate over the last few seconds
    // (README). 0 until the manifest is in, because before it nothing knows the file's size.
    double bytes_per_second = 0.0;
    // Seconds from this file's first decoded data to the frame that verified it, once verified.
    //
    // The transfer itself, without the time spent pointing the camera: the 2.2 MB photo that
    // read 178 KB/s on the card for 13 s was reported as 94.3 KB/s because the clock had started
    // 11 s earlier, at "Receive", before the sender was even in view. Both are true; only one is
    // the speed of the link, and a result that shows only the other contradicts the card.
    double transfer_seconds = 0.0;
    double motion_blur_cells = 0.0;   // screen motion during one exposure, in cells
    double recent_read_fraction = 0.0;  // frames in the window whose header decoded
    bool screen_in_view = false;
    std::uint64_t frames = 0;
    // Frames whose PAYLOAD decoded: intra-frame FEC succeeded and the CRC over it matched. The
    // proof that this receiver has the sender's grid. A decoded header is not: the header band is a
    // few rows at the top, and a near-miss grid (120x208 against 120x200) puts those rows in nearly
    // the same place and reads them (F58). The payload spans every cell, so a wrong grid cannot.
    std::uint64_t payload_frames = 0;
    // Times the sender switched to a different file and this receiver started over on it.
    std::uint32_t sender_switches = 0;
    // Distinct display states whose header read: over time, the state rate this receiver actually
    // sees (EXP-006), against the rate the sender drew.
    std::uint64_t distinct_states = 0;
    // The last colour frame's evidence (ColorDiagnostics): how far apart its eight colours sat and
    // how noisy, 0..255 units. Both 0 until an M5 frame arrives.
    double color_spread = 0.0;
    double color_noise = 0.0;
    // Bits per cell the sender is using (1, 2 or 3), from its headers; 0 before any has read.
    std::uint32_t bits_per_cell = 0;
    // Frames whose payload decoded only combined with another capture of the same state.
    std::uint64_t combined_frames = 0;
    // The lens distortion the last frame's lane has learned (FramePipeline::lens_strength).
    double lens_strength = 0.0;

    std::string file_name;            // a display hint (sanitised), once the manifest is in
    std::uint64_t file_size = 0;
    // What the sender said the file is before compression (ContentHint), when it said.
    ContentHint content;
};

// The speed and the time left of a transfer in progress, from its verified progress over time.
//
// The rate over the last few seconds is ACCURATE but not steady: payloads arrive in bursts as the
// hand moves, and on the first 2.2 MB transfer the estimate made from it went
// 2 min -> 4 -> 6 -> 12 -> 9 -> 2 min -> 81 s in under ten seconds. Here the speed follows that
// windowed rate through a first-order filter, and the time left counts down by the clock and glides
// to each new estimate -- so a real change of speed shows within a few seconds, without a jump.
class TransferRate {
  public:
    struct Config {
        double window_s = 4.0;
        double max_window_s = 20.0;  // the window stretches to this to hold...
        double min_progress = 0.02;  // ...at least this much of the file
        double rate_smoothing_s = 2.0;
        double eta_smoothing_s = 2.0;
    };

    TransferRate() noexcept = default;
    explicit TransferRate(Config cfg) noexcept : cfg_(cfg) {}

    // Progress (0..1) at `timestamp_ns`; `paused` while nothing is arriving. `started_ns` is when
    // the first data arrived, 0 if unknown: progress already made at the first call is spread
    // over the time since then. Calls older than one already seen are ignored, because decode
    // lanes finish out of order.
    void Update(std::int64_t timestamp_ns, double progress, bool paused,
                std::int64_t started_ns = 0);
    void Reset() noexcept { *this = TransferRate(cfg_); }

    // Progress per second right now: 0 before the first measurement and while paused.
    [[nodiscard]] double rate() const noexcept { return paused_ || !valid_ ? 0.0 : rate_; }
    // Seconds left as they should be SHOWN; < 0 when there is no honest estimate.
    [[nodiscard]] double seconds_remaining() const noexcept { return eta_s_; }

  private:
    Config cfg_;
    std::deque<std::pair<std::int64_t, double>> history_;
    std::int64_t last_ns_ = 0;
    bool seeded_ = false;  // the first call, and the progress made before it, accounted for
    bool valid_ = false;   // `rate_` rests on a measurement rather than its zero start
    bool paused_ = false;
    double rate_ = 0.0;
    double eta_s_ = -1.0;
};

class LiveReceiver {
  public:
    static constexpr std::size_t kMaxLanes = 8;

    static Result<LiveReceiver> Create(GridGeometry grid, LiveConfig cfg = {});

    // One camera frame. Frames may be skipped freely -- a receiver too slow to process every frame
    // just reads fewer of them -- because nothing depends on seeing consecutive ones.
    //
    // THREAD-SAFE across lanes: up to `cfg.lanes` threads may call this at once, each with its own
    // `lane`. The image work runs in the lane, unlocked; only the session and the status are
    // serialised. Returns a COPY of the status, because another lane may be changing it.
    //
    // `chroma` carries the frame's colour, which an M5 sender needs; null reads M0 only.
    LiveStatus Ingest(const ImageView8& img, std::int64_t timestamp_ns, std::size_t lane = 0,
                      const ChromaPlanes* chroma = nullptr);

    // Not for use while another thread may be in Ingest.
    [[nodiscard]] const LiveStatus& status() const noexcept { return status_; }

    // The verified file, once `status().phase == kVerified`. kFountainIncomplete before then, and
    // kHashMismatch after a failed verification -- never partial or unverified bytes.
    [[nodiscard]] Result<std::vector<std::uint8_t>> File() const;

    [[nodiscard]] const FramePipeline& pipeline() const noexcept { return *pipelines_.front(); }
    [[nodiscard]] const ReceiveSession& session() const noexcept { return *session_; }
    [[nodiscard]] std::size_t lanes() const noexcept { return pipelines_.size(); }

  private:
    LiveReceiver(std::unique_ptr<FrameLayout> layout,
                 std::vector<std::unique_ptr<FramePipeline>> pipelines,
                 std::vector<std::unique_ptr<FrameDecoder>> decoders,
                 std::unique_ptr<ReceiveSession> session, LiveConfig cfg)
        : layout_(std::move(layout)),
          pipelines_(std::move(pipelines)),
          decoders_(std::move(decoders)),
          session_(std::move(session)),
          cfg_(cfg),
          mu_(std::make_unique<std::mutex>()),
          rate_(TransferRate::Config{.window_s = cfg.eta_window_s,
                                     .rate_smoothing_s = cfg.rate_smoothing_s,
                                     .eta_smoothing_s = cfg.eta_smoothing_s}) {}

    void Decide(std::int64_t timestamp_ns, const FramePipeline& lane);

    // Heap-held so their addresses survive moves: the pipelines and the session keep pointers to
    // the layout.
    std::unique_ptr<FrameLayout> layout_;
    std::vector<std::unique_ptr<FramePipeline>> pipelines_;
    // One per lane, beside its pipeline: header, demodulation and FEC run in the lane too, outside
    // the lock, which then covers only the session's bookkeeping (F69).
    std::vector<std::unique_ptr<FrameDecoder>> decoders_;
    std::unique_ptr<ReceiveSession> session_;
    LiveConfig cfg_;
    // Guards everything below it and the session. Heap-held so the receiver stays movable.
    std::unique_ptr<std::mutex> mu_;

    LiveStatus status_;
    std::deque<bool> read_window_;
    int frames_without_screen_ = 0;
    int foreign_run_ = 0;
    TransferRate rate_;
    std::int64_t first_payload_ns_ = 0;  // when this session's first payload frame arrived
    std::int64_t last_payload_ns_ = 0;   // and its latest
    std::optional<Result<std::vector<std::uint8_t>>> result_;
};

}  // namespace fileflow
