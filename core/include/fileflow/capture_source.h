// CaptureSource — the interface that makes offline development possible (ADR-0010).
//
// THREE implementations, one decoder:
//   - Android camera (platform/android/capture)   live
//   - Channel simulator (sim/)                    synthetic, exact ground truth
//   - Replay harness (harness/)                   recorded real captures
//
// The decode chain cannot tell which one it is running against. That substitutability is
// exactly why simulator and replay results transfer to live behaviour: if a bug reproduces
// in replay it is a real bug, and if a change improves simulated goodput it is a real
// change. Design this interface before the live receiver, never retrofit it.
#pragma once

#include <fileflow/grid.h>
#include <fileflow/result.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace fileflow {

// How a captured frame relates to display-state boundaries (component C07).
enum class FramePhase : std::uint8_t {
    kClean = 0,   // exactly one display state
    kMixed,       // rolling-shutter mixture of consecutive states
    kDuplicate,   // a state already decoded
    kUnknown,
};

// One captured frame, already reduced to per-cell samples.
//
// NOTE ON SCOPE: at this stage the geometric pipeline (marker detection, homography,
// rectification, subpixel sampling) is NOT yet implemented -- sources hand back cell
// samples directly. Those stages slot in between the raw image and this struct without
// changing the interface. See docs/architecture/COMPONENT-REGISTRY.md C06/C08.
struct CapturedFrame {
    std::uint64_t index = 0;         // monotonic capture counter
    std::int64_t timestamp_ns = 0;   // sensor timestamp; base depends on the device
    FramePhase phase = FramePhase::kUnknown;

    // One sample per grid cell, flat row-major, photometrically raw.
    // NaN means "no usable sample" -- occluded, out of frame, or in a rolling-shutter
    // transition band. The demodulator turns NaN into an ERASURE, never a guess.
    std::vector<double> cell_samples;

    // Three per cell, interleaved r,g,b, each channel scaled so this frame's white pilots read 255
    // and its black ones 0 -- the input M5 demodulates. Empty when the source had no colour.
    std::vector<double> cell_rgb;

    // Ground truth, populated ONLY by the simulator. Live and replay sources leave this
    // empty. Its presence is what lets us measure component accuracy (geometric error,
    // phase-classification accuracy, per-cell error maps) rather than just end-to-end
    // success -- the thing a camera can never provide.
    std::optional<CellMatrix> ground_truth;
};

class CaptureSource {
  public:
    virtual ~CaptureSource() = default;

    [[nodiscard]] virtual GridGeometry geometry() const = 0;

    // Returns std::nullopt when the source is exhausted (simulator/replay) or the session
    // has ended (live). A dropped frame is reported as a gap in `index`, never silently --
    // drops are a Pc term and must reach telemetry.
    [[nodiscard]] virtual std::optional<CapturedFrame> Next() = 0;

    [[nodiscard]] virtual std::uint64_t frames_emitted() const = 0;
    [[nodiscard]] virtual std::uint64_t frames_dropped() const = 0;
};

}  // namespace fileflow
