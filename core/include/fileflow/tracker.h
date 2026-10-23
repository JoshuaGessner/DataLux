// Persistent screen tracking (component C06, second half).
//
// WHY THIS EXISTS AT ALL: ADR-0006's central claim is that "a persistent tracked grid should
// outperform repeated independent QR detection". Until this file existed the receiver did
// full-image acquisition on every frame -- i.e. we were doing the repeated-detection thing
// ourselves, and the project's founding hypothesis was untested in our own code.
//
// The saving is structural, not incremental. Full acquisition scans every pixel of the frame
// to find the screen. Tracking already knows roughly where the screen is, so it only has to
// confirm and refine within a small window around the previous quad. That turns an O(image)
// search into O(search window), and the window does not grow with sensor resolution -- which
// matters because sensor resolution is exactly what is growing.
//
// The state machine is deliberately conservative. Any doubt drops to reacquisition rather
// than propagating a drifting homography, because a slightly wrong homography is the failure
// mode that silently poisons every downstream stage (see ScreenDetector's verification note).
#pragma once

#include <fileflow/detect.h>
#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace fileflow {

enum class TrackState : std::uint8_t {
    kSearching = 0,  // no lock; full-image acquisition each frame
    kTracking,       // locked; refining locally
    kDegraded,       // lock held but quality falling; still refining, watching closely
    kLost,           // was locked, now not; next frame reacquires
};

[[nodiscard]] std::string_view TrackStateName(TrackState s) noexcept;

struct TrackerConfig {
    DetectionConfig detection;

    // Search window around the predicted quad, as a fraction of the quad's size. Sets how
    // much inter-frame motion can be absorbed without a full reacquisition.
    double search_margin = 0.25;

    // Marker score below which a tracked frame is considered degraded.
    double degrade_score = 0.80;

    // Consecutive degraded/failed frames tolerated before declaring loss. Small on purpose:
    // holding a bad lock is worse than paying for reacquisition.
    int max_degraded_frames = 3;

    // Maximum corner movement between frames, as a fraction of quad size, before the result
    // is rejected as implausible. Guards against snapping onto a different bright object.
    // Measured from the PREDICTED position when the motion model is on, so it bounds the
    // surprise, not the speed.
    double max_corner_jump = 0.35;

    // Constant-velocity prediction (F53). OFF, because it was measured and did not help.
    //
    // The hypothesis was that a hand moves smoothly, so searching around where the screen is
    // GOING beats searching around where it WAS. Simulated handheld motion (shake to 4, sweeping
    // pans to 1 image width/s, at 30, 10 and 6 processed frames/s) gave identical results with and
    // without it -- the 25% annulus already covers real hand motion -- and at 6 fps with a fast pan
    // it lost lock once per seed where the plain search lost it never. Frames lost to motion are
    // lost to BLUR, not to the tracker. Kept switchable so the result can be rechecked on real
    // rolling-shutter captures; the velocity estimate itself is always maintained, because speed()
    // drives the receiver's "hold steadier" hint.
    bool predict_motion = false;
    // Velocity smoothing time constant, in seconds. A TIME constant rather than a per-frame weight,
    // because the receiver processes frames at whatever rate it can keep up with: a fixed weight
    // averaged 3x more frames at 30 fps than at 10, and since corner positions are quantised to a
    // pixel, the speed estimate came out ~15% noisier at the higher rate (measured; see
    // ScreenTracker.SpeedIsPerSecondWhateverTheFrameRate). 0.1 s rides through tremor and still
    // follows a change of direction within a few tenths of a second.
    double velocity_tau_s = 0.1;
    // The annulus widens with observed speed: margin = search_margin + this x (speed / scale),
    // capped at max_search_margin. A fast-moving screen gets a wider window rather than a lost
    // lock and a full-image reacquisition.
    double speed_margin_gain = 1.5;
    double max_search_margin = 0.45;
};

struct TrackResult {
    bool ok = false;
    Homography grid_to_image;
    std::array<Point2, 4> quad{};
    TrackState state = TrackState::kSearching;
    double marker_score = 0.0;
    bool reacquired = false;  // this frame required a full-image search
};

class ScreenTracker {
  public:
    static Result<ScreenTracker> Create(const FrameLayout& layout, TrackerConfig cfg = {});

    // `timestamp_ns` is the capture time. With it the motion model works in real time, so a frame
    // the receiver skipped (a busy decoder, a dropped capture) widens the prediction instead of
    // breaking it. Without it (0, or not increasing) frames are assumed 1/30 s apart.
    [[nodiscard]] TrackResult Track(const ImageView8& img, std::int64_t timestamp_ns = 0);

    // Drop the lock, e.g. after a session restart or a display-mode change.
    void Reset() noexcept;

    [[nodiscard]] TrackState state() const noexcept { return state_; }

    // --- Telemetry. These numbers are the ADR-0006 evidence, so the names have to be exact. ---

    // Frames that yielded a usable homography, BY EITHER PATH. Not "frames tracked" in the
    // sense of "handled without reacquisition" -- see refined_frames() for that. The
    // distinction matters: successful_frames / (successful_frames + acquisitions) would
    // double-count acquisitions and overstate the tracker.
    [[nodiscard]] std::uint64_t successful_frames() const noexcept { return successful_; }

    // Frames resolved by local refinement alone -- the tracker actually doing its job.
    [[nodiscard]] std::uint64_t refined_frames() const noexcept { return refined_; }

    // WHY refinement was rejected, per gate.
    //
    // Four separate conditions send `RefineLocally` back to a full acquisition, and a bare
    // "refined_frames 0" cannot distinguish them -- which is exactly the situation on real captures,
    // where refinement failed 60 times out of 60 while the simulator refines happily (F13). The four
    // causes call for completely different fixes: no extremes means the window missed the boundary,
    // a corner jump means the plausibility bound is too tight for real inter-frame motion, and a low
    // marker score means the refined geometry is genuinely wrong.
    [[nodiscard]] std::uint64_t refine_rejects_no_extremes() const noexcept { return rj_extremes_; }
    [[nodiscard]] std::uint64_t refine_rejects_corner_jump() const noexcept { return rj_jump_; }
    [[nodiscard]] std::uint64_t refine_rejects_homography() const noexcept { return rj_homography_; }
    [[nodiscard]] std::uint64_t refine_rejects_marker_score() const noexcept { return rj_score_; }
    /** Worst marker score seen from a refinement that was rejected for it. */
    [[nodiscard]] double worst_rejected_score() const noexcept { return worst_rj_score_; }

    [[nodiscard]] std::uint64_t full_acquisitions() const noexcept { return acquisitions_; }
    [[nodiscard]] std::uint64_t losses() const noexcept { return losses_; }

    // Screen speed from the motion model, in screen DIAGONALS PER SECOND, worst corner: 0.5 means
    // a corner is moving half the screen's diagonal each second. Size-normalised so the same
    // number means the same thing near and far, and per second so it does not depend on how many
    // frames the receiver had time to process. The receiver UI turns this into "hold steadier";
    // 0 when there is no lock.
    [[nodiscard]] double speed() const noexcept;

    // Pixels examined on the last frame, SUMMED across every attempt made for that frame.
    // A frame whose refinement failed and then reacquired is charged for both, because it
    // spent both.
    [[nodiscard]] std::uint64_t last_pixels_examined() const noexcept { return last_px_; }

  private:
    ScreenTracker(const FrameLayout& layout, TrackerConfig cfg, ScreenDetector detector);

    // Refine within a window around the predicted quad. Returns false if the window yields
    // nothing plausible, which sends the caller to full reacquisition.
    [[nodiscard]] bool RefineLocally(const ImageView8& img, TrackResult* out);

    const FrameLayout* layout_;
    TrackerConfig cfg_;
    ScreenDetector detector_;

    // Corners where the motion model expects the screen `dt_s` seconds after the last lock.
    [[nodiscard]] std::array<Point2, 4> Predicted(double dt_s) const noexcept;
    // Fold a newly locked quad into the velocity estimate. `fresh` = no previous lock to compare.
    void UpdateMotion(const std::array<Point2, 4>& quad, double dt_s, bool fresh) noexcept;

    TrackState state_ = TrackState::kSearching;
    std::array<Point2, 4> last_quad_{};
    std::array<Point2, 4> velocity_{};  // per-corner, pixels per SECOND, smoothed
    std::int64_t last_ts_ = 0;           // capture time of last_quad_, 0 = unknown
    double dt_s_ = 0.0;                  // seconds since the last lock, for this frame

    // Quarter-turns the ACQUISITION applied to align the geometric quad with the grid.
    //
    // This has to be carried into refinement, and its absence was a real bug (F37). `Detect`
    // resolves the four-fold ambiguity by scoring all four rotations and stores the WINNING one --
    // so `last_quad_` is in grid order, not geometric order. Refinement rebuilds a quad from raw
    // image extremes, which is geometric order. Comparing the two without re-applying this rotation
    // compares mismatched corners, and on a screen rotated in the sensor frame that made every
    // corner appear to have jumped most of the screen's width.
    int rotation_ = 0;
    int degraded_run_ = 0;

    // Binarisation level carried forward from the last acquisition.
    //
    // Recomputing Otsu per frame would cost a full-image pass and defeat the whole point of
    // tracking. Reusing it is consistent with the system design rather than a shortcut:
    // exposure, ISO and white balance are all LOCKED for the session precisely so the
    // photometric channel stays stable (docs/research/android-camera-pipeline.md). If it does
    // drift, marker verification fails, the tracker reacquires, and the threshold is
    // refreshed -- the mechanism is self-correcting.
    std::uint32_t threshold_ = 128;

    std::uint64_t successful_ = 0;
    std::uint64_t refined_ = 0;
    std::uint64_t rj_extremes_ = 0;
    std::uint64_t rj_jump_ = 0;
    std::uint64_t rj_homography_ = 0;
    std::uint64_t rj_score_ = 0;
    double worst_rj_score_ = -1.0;
    std::uint64_t acquisitions_ = 0;
    std::uint64_t losses_ = 0;
    std::uint64_t last_px_ = 0;
};

}  // namespace fileflow
