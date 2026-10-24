// Optical rendering: a CellMatrix as a camera would actually see it (component C16).
//
// This is the GEOMETRIC half of the simulator, the part channel.h deferred until the CV path
// existed. It matters because it exercises the real receiver chain -- detect, rectify, sample
// -- rather than handing cell samples straight to the demodulator. Geometric error is a real
// term in the error budget and the only honest way to include it is to make the decoder earn
// its homography from pixels.
//
// ⚠ Same warning as channel.h: NOT calibrated against real captures (RISK-024). Every output
// is [HYP] until Phase 2 compares it with recorded frames.
#pragma once

#include <fileflow/geometry.h>
#include <fileflow/grid.h>
#include <fileflow/image.h>

#include <array>
#include <cstdint>

namespace fileflow::sim {

// Where the camera is relative to the screen. Drives EXP-018 (viewing angle) and EXP-017
// (distance) directly, rather than making experimenters hand-write corner coordinates.
struct ViewConfig {
    int image_width = 960;
    int image_height = 1280;

    // 1.0 places the screen filling ~80% of image height head-on. Larger is further away.
    double distance = 1.0;

    double yaw_deg = 0.0;    // rotation about the screen's vertical axis
    double pitch_deg = 0.0;  // about the horizontal axis
    double roll_deg = 0.0;   // about the optical axis

    // Off-centre placement, in fractions of image size.
    double offset_x = 0.0;
    double offset_y = 0.0;
};

struct OpticalRenderConfig {
    ViewConfig view;

    // Multiplicative luminance at the frame corners relative to the centre, about the GRID
    // centre -- i.e. a transmitter-side effect (RISK-025), not lens vignetting. It does not
    // follow the image centre under an off-axis view, which is exactly what makes it
    // different from vignetting and why the photometric field must not assume a cause.
    double corner_falloff = 1.0;

    // Uniform additive black-level lift.
    double black_lift = 0.0;

    // Directional glare: additive lift strongest at the left edge of the GRID, zero at the
    // right. Finding F7: this, not falloff, is what defeats a global threshold, because it
    // squeezes the level ranges from the opposite side until they overlap.
    double glare_lift = 0.0;

    // Radial lens distortion, Brown k1, about the image centre with radius normalised to the half
    // diagonal: a point at undistorted radius r lands at r * (1 + k1 r^2). Negative is barrel.
    //
    // The first impairment here a homography CANNOT represent. Every other geometric effect in
    // this renderer is projective, so four correct corners made the whole grid correct and a
    // single global sub-cell shift was always enough -- which is why the simulator could never
    // show a reason to refine anything but a translation (F51). Phone main cameras are corrected
    // by the ISP but not perfectly, and 1% of residual at the frame edge is several pixels.
    double lens_k1 = 0.0;

    int blur_radius = 0;          // box blur, px. Approximates defocus.
    double noise_amplitude = 0.0; // deterministic additive noise, peak in 8-bit levels
    std::uint64_t seed = 12345;

    std::uint8_t background = 0;  // surround level; locked exposure underexposes it
};

// Screen corners in image space (TL, TR, BR, BL) for a viewing configuration.
// Returns a degenerate quad if the screen would be behind the camera.
[[nodiscard]] std::array<Point2, 4> QuadFromView(const ViewConfig& v, const GridGeometry& g);

// Grid corners in grid space, in the canonical order used everywhere: TL, TR, BR, BL.
[[nodiscard]] std::array<Point2, 4> GridCorners(const GridGeometry& g);

// Render `cells` warped by an explicit transform.
[[nodiscard]] Image8 RenderOptical(const CellMatrix& cells, const Homography& grid_to_image,
                                   const OpticalRenderConfig& cfg);

// Render using the viewing geometry, and report the transform that was used so tests and
// experiments can measure estimated-vs-true geometric error.
//
// With `lens_k1` set the transform is NOT the whole truth: a grid point's true image position is
// `DistortPoint(used_transform.Apply(p), cfg)`. Measure against that, or the distortion the
// receiver is fighting is invisible to the error figure.
[[nodiscard]] Image8 RenderView(const CellMatrix& cells, const GridGeometry& g,
                                const OpticalRenderConfig& cfg, Homography* used_transform);

// How the RECEIVING camera sees the sending panel's three primaries (M5).
//
// Display channel j lights camera channel i by `mix[i][j]`: the diagonal is each primary's own
// response, the rest is crosstalk through overlapping colour filters -- an OLED's red is not the
// sensor's red. Then a per-channel gain: white balance, wherever the locked camera left it.
// Defaults are deliberately unkind: ~15-20% crosstalk and a visibly tinted white.
struct ColorChannelConfig {
    std::array<std::array<double, 3>, 3> mix = {{{0.82, 0.14, 0.04},
                                                 {0.12, 0.76, 0.12},
                                                 {0.03, 0.17, 0.80}}};
    std::array<double, 3> gain = {1.05, 0.95, 1.15};
    // Sensor noise, added per pixel and per channel AFTER the colour filters -- so, unlike the
    // optical noise in OpticalRenderConfig, no blur averages it away. Peak, in 8-bit levels.
    double sensor_noise = 0.0;
    // Colour shading: each channel's gain drifts linearly across the picture, by +/- this
    // fraction from the left edge to the right. Real lenses tint their edges, and OLED primaries
    // shift with viewing angle, so one set of colour centroids does not fit a whole frame.
    std::array<double, 3> shading = {0.0, 0.0, 0.0};
    // Colour resolution lost in the camera beyond 4:2:0 itself -- demosaicing, and the resampling
    // of a digital zoom -- as a box blur of this radius on the chroma planes, in chroma pixels.
    // Each cell's colour then bleeds into its neighbours' while its brightness stays sharp.
    int chroma_blur = 0;
};

// A YUV 4:2:0 capture: full-resolution luma, half-resolution chroma both ways, as Camera2 delivers.
struct YuvImage {
    Image8 y;
    Image8 u;
    Image8 v;
};

// Renders `cells` -- colour levels (ColorValue) as M5 draws them, or M0's two -- through the
// same optics as RenderView, one display channel at a time, then mixes them through the camera's
// response and subsamples the chroma.
[[nodiscard]] YuvImage RenderColorView(const CellMatrix& cells, const GridGeometry& g,
                                       const OpticalRenderConfig& cfg,
                                       const ColorChannelConfig& color,
                                       Homography* used_transform);

// The lens model's forward map: undistorted image pixel -> where the camera actually records it.
// Identity when `cfg.lens_k1` is zero.
[[nodiscard]] Point2 DistortPoint(Point2 p, const OpticalRenderConfig& cfg) noexcept;

// A HANDHELD receiver: the camera moves during and between exposures.
//
// Every other impairment here is static per frame, so the simulator could not say anything about
// the commonest real condition -- a person holding a phone -- and the tracker's inter-frame
// behaviour had only ever met i.i.d. jitter, which no hand produces. Hands drift smoothly with a
// small fast tremor on top, and that shape matters: a smooth drift is predictable from the last
// two frames, a random jump is not.
//
// Three effects, each of which breaks something different:
//  - inter-frame motion  -> the tracker's search window and plausibility bound;
//  - motion during the exposure -> blur along the motion, which smears cells into neighbours;
//  - rolling shutter    -> rows read at different times, so a moving screen is sheared.
struct MotionConfig {
    // Handheld shake intensity. 0 = rigid. 1 = a careful person holding the phone: sway of ~1.5% /
    // 1.0% of the image at ~1 Hz, tremor of 0.25% at ~9 Hz, ~0.8 deg roll and ~2% distance sway.
    // Scales linearly; 2-3 is a tired arm or a walk-up. [HYP]: shaped from the physiological
    // tremor literature, not measured on this rig.
    double shake = 0.0;
    // Sweeping pan, as fractions of the image per second, on top of the shake: constant speed,
    // reversing at +/-`pan_range` so the screen stays in view. An unbounded pan would carry the
    // screen out of frame within seconds and measure "nothing to see" instead of tracking.
    double pan_x = 0.0;
    double pan_y = 0.0;
    double pan_range = 0.12;
    double exposure_s = 0.015;  // the rig's measured setting (F36)
    double readout_s = 0.010;   // top-to-bottom rolling-shutter skew
    int subframes = 4;          // exposure integration steps (motion blur)
    int bands = 24;             // rolling-shutter row bands
    std::uint64_t phase_seed = 1;

    [[nodiscard]] bool active() const noexcept {
        return shake > 0.0 || pan_x != 0.0 || pan_y != 0.0;
    }
};

// The viewing geometry at time `t` seconds.
[[nodiscard]] ViewConfig ViewAt(const ViewConfig& base, const MotionConfig& m, double t) noexcept;

// Render one CAPTURE starting at `t_start`: integrated over the exposure, each row band at its own
// readout time. `truth` receives the transform at the middle of the exposure and readout -- the
// best single homography, and still not the whole truth once rows disagree.
[[nodiscard]] Image8 RenderMoving(const CellMatrix& cells, const GridGeometry& g,
                                  const OpticalRenderConfig& cfg, const MotionConfig& m,
                                  double t_start, Homography* truth);

// ---------------------------------------------------------------- unsynchronised capture

// A camera and a display that do NOT share a clock (EXP-006's `Pc`, unmodelled until F60).
//
// Everything else in the simulator renders one display state per capture: the camera is assumed to
// see each state exactly once, whole. A real phone camera free-runs at its own rate, and its rolling
// shutter reads rows over `readout_s`, each row integrating for `exposure_s`. Any row whose exposure
// window straddles a state change records a time-weighted MIX of the two states -- a torn capture,
// one part of the frame from each state and a smeared band where they overlap.
//
// Static geometry by design: this isolates the TIMING term. Motion is RenderMoving's job.
struct CaptureTiming {
    double exposure_s = 0.016;
    double readout_s = 0.030;  // first row to last row
    int bands = 64;            // row bands, each sampled at its own time
};

// `state(k)` is display state k, shown over [k/fd, (k+1)/fd). The capture's first row starts
// exposing at `t_start`. `states_hit`, if non-null, receives how many distinct states contributed.
[[nodiscard]] Image8 RenderUnsynced(const CellMatrix& (*state)(void* ctx, std::int64_t k), void* ctx,
                                    double fd, const GridGeometry& g,
                                    const OpticalRenderConfig& cfg, const CaptureTiming& ct,
                                    double t_start, Homography* truth, int* states_hit = nullptr);

}  // namespace fileflow::sim
