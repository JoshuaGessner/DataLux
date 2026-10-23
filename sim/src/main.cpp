// ffsim — end-to-end simulation driver.
//
// Runs a complete transfer through the REAL decode chain under a configured channel and
// reports the metrics the experiment registry asks for. Every run is reproducible from
// its flags plus the commit; the seed is always echoed.
//
// ⚠ Outputs are [HYP] until the simulator is calibrated against real captures in Phase 2
// (SIM-03 / RISK-024). Nothing printed here is a measured result.
#include <fileflow/frame.h>
#include <fileflow/harness/capture.h>
#include <fileflow/intra_fec.h>
#include <fileflow/link.h>
#include <fileflow/modulation.h>
#include <fileflow/pipeline.h>
#include <fileflow/session.h>
#include <fileflow/sim/channel.h>
#include <fileflow/sim/render.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace fileflow;

struct Options {
    std::uint32_t cols = 120;
    std::uint32_t rows = 200;
    std::size_t payload_bytes = 64 * 1024;
    std::uint32_t symbol_size = 0;  // 0 => size it to fill the frame
    std::uint32_t block_symbols = 0;      // 0 => AutoBlockSymbols (few, balanced blocks)
    std::uint32_t manifest_interval = 0;  // 0 => FileTransmitter::AutoManifestInterval
    std::uint64_t max_frames = 200000;
    double fd = 60.0;  // assumed distinct display states per second
    sim::ChannelConfig channel;

    // --- Image path (component C06/C08/C09) ---
    // Off by default. When off, cell samples are synthesised directly, which ISOLATES coding
    // and modulation. When on, the decoder must earn its homography from rendered pixels, so
    // geometric error, detection failures and photometric estimation error enter the budget.
    // The two modes answer different questions; neither replaces the other.
    bool image_path = false;
    bool no_tracking = false;  // force full acquisition every frame (ADR-0006 A/B baseline)
    bool no_subcell = false;   // disable sub-cell alignment (the F45/F51 A/B baseline)
    bool no_corners = false;   // global shift only, no corner refinement (the F51 A/B)
    bool no_predict = false;   // tracker searches around the last position only (the F53 A/B)
    sim::OpticalRenderConfig render;
    sim::MotionConfig motion;  // a handheld receiver; inactive unless --shake / --pan

    // Write the rendered frames out as a capture bundle. Lets the replay harness be exercised
    // end to end before any real capture exists, and gives the Android recorder a reference
    // bundle to produce byte-compatible output against.
    std::string record_dir;

    // Intra-frame FEC (ADR-0009's first half). nsym=0 disables it, which is the A/B baseline
    // for measuring what the layer actually buys.
    std::size_t fec_nsym = 32;

    // Erase cells whose |llr| falls below this. 0 = off (hard decisions only).
    int erase_below = 0;

    // Run the adaptive link controller (C14) alongside the transfer and report which nsym the
    // receiver's own telemetry would have chosen. REPORTING ONLY -- see the note it prints.
    bool adapt = false;

    // Suppress every line whose value depends on THIS HOST rather than on the channel: decoder
    // throughput, real-time headroom, and the simulator's own render time. What remains is
    // reproducible from the flags plus the commit, which is what a determinism gate needs to
    // compare (finding F24 -- the gate previously compared wall-clock timings and could not
    // ever have passed).
    bool no_host_metrics = false;
};

double Arg(const char* v) { return std::strtod(v, nullptr); }

void Usage() {
    std::puts(
        "ffsim — FileFlow offline channel simulator\n"
        "\n"
        "  --cols N --rows N            grid geometry (default 120x200)\n"
        "  --payload N                  payload bytes (default 65536)\n"
        "  --symbol-size N              fountain symbol size (default: fill the frame)\n"
        "  --fd RATE                    assumed display states/sec for goodput (default 60)\n"
        "  --block-symbols N            source symbols per block (default: as few balanced\n"
        "                               blocks as the decoder bound allows, ADR-0015)\n"
        "  --manifest-interval N        one manifest frame every N display states\n"
        "                               (default: scaled to the transfer, 16..128)\n"
        "  --seed N                     RNG seed (default 20260802)\n"
        "  --noise SIGMA                Gaussian read noise (default 2.0)\n"
        "  --shot SCALE                 signal-dependent shot noise (default 0)\n"
        "  --crosstalk F                spatial crosstalk 0..1 (default 0)\n"
        "  --gamma G                    camera response gamma (default 1.0)\n"
        "  --vignetting F               0..1 (default 0)\n"
        "  --exposure G                 exposure gain (default 1.0)\n"
        "  --glare S --glare-radius R   local saturation\n"
        "  --occlusion F                occluded grid fraction 0..1\n"
        "  --drop F                     frame drop rate 0..1\n"
        "  --duplicate F                duplicate frame rate 0..1\n"
        "  --mixed F                    rolling-shutter mixed frame rate 0..1\n"
        "  --max-frames N               safety bound on the run\n"
        "\n"
        "IMAGE PATH (renders real pixels; decoder must earn its homography)\n"
        "  --image-path                 enable render -> detect/track -> sample -> normalise\n"
        "  --no-tracking                full acquisition every frame (ADR-0006 A/B baseline)\n"
        "  --no-subcell                 disable sub-cell grid alignment (A/B baseline)\n"
        "  --no-corners                 global sub-cell shift only, no corner refinement\n"
        "  --no-predict                 tracker ignores motion: searches around the last\n"
        "                               position with a fixed window (A/B baseline)\n"
        "  --image-size W H            capture resolution (default 900x1500)\n"
        "  --distance D                 1.0 fills ~80%% of frame height (default 1.0)\n"
        "  --yaw D --pitch D --roll D   viewing angles in degrees\n"
        "  --falloff F                  corner luminance vs centre, 1.0 = flat\n"
        "  --img-glare L                directional black-level lift (finding F7)\n"
        "  --blur R                     defocus box-blur radius in pixels\n"
        "  --lens-k1 K                  radial lens distortion (negative = barrel); the one\n"
        "                               image-path impairment a homography cannot represent\n"
        "  --shake S                    handheld receiver: smooth sway + tremor, with motion\n"
        "                               blur and rolling shutter. 1 = careful hold, 2-3 = poor\n"
        "  --pan X Y                    steady pan, fractions of the image per second\n"
        "  --exposure-ms T              exposure time for motion blur (default 15)\n"
        "  --readout-ms T               rolling-shutter readout skew (default 10)\n"
        "  --img-noise A                additive pixel noise amplitude\n"
        "  --record DIR                 write frames as a replayable capture bundle\n"
        "\n"
        "INTRA-FRAME FEC (ADR-0009)\n"
        "  --fec-nsym N                 RS parity bytes per 255-byte codeword (0 = off,\n"
        "                               default 32). Corrects N erasures or N/2 errors.\n"
        "  --erase-below L              erase cells with |llr| < L (0 = off). Trades a\n"
        "                               cheap certain erasure for a costly probable error.\n"
        "\n"
        "ADAPTIVE LINK CONTROLLER (C14)\n"
        "  --adapt                      score every nsym rung from this run's receiver\n"
        "                               telemetry and report the one that maximises expected\n"
        "                               payload. REPORTING ONLY: nsym fixes the fountain\n"
        "                               symbol size, which the manifest fixes for the whole\n"
        "                               session, so it cannot be changed mid-transfer.\n"
        "\n"
        "REPRODUCIBILITY\n"
        "  --no-host-metrics            omit decoder throughput, real-time headroom and render\n"
        "                               time -- the only lines whose value depends on THIS\n"
        "                               HOST. What remains is reproducible from the flags plus\n"
        "                               the commit, so a determinism gate can compare it.\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options o;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };

        if (a == "--help" || a == "-h") { Usage(); return 0; }
        else if (a == "--cols") o.cols = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--rows") o.rows = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--payload") o.payload_bytes = static_cast<std::size_t>(Arg(next()));
        else if (a == "--symbol-size") o.symbol_size = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--block-symbols") o.block_symbols = static_cast<std::uint32_t>(Arg(next()));
        else if (a == "--manifest-interval") {
            o.manifest_interval = static_cast<std::uint32_t>(Arg(next()));
        }
        else if (a == "--seed") o.channel.seed = static_cast<std::uint64_t>(Arg(next()));
        else if (a == "--noise") o.channel.read_noise_sigma = Arg(next());
        else if (a == "--shot") o.channel.shot_noise_scale = Arg(next());
        else if (a == "--crosstalk") o.channel.crosstalk = Arg(next());
        else if (a == "--gamma") o.channel.gamma = Arg(next());
        else if (a == "--vignetting") o.channel.vignetting = Arg(next());
        else if (a == "--exposure") o.channel.exposure_gain = Arg(next());
        else if (a == "--glare") o.channel.glare_strength = Arg(next());
        else if (a == "--glare-radius") o.channel.glare_radius_cells = Arg(next());
        else if (a == "--occlusion") o.channel.occlusion_fraction = Arg(next());
        else if (a == "--drop") o.channel.frame_drop_rate = Arg(next());
        else if (a == "--duplicate") o.channel.duplicate_rate = Arg(next());
        else if (a == "--mixed") o.channel.mixed_rate = Arg(next());
        else if (a == "--max-frames") o.max_frames = static_cast<std::uint64_t>(Arg(next()));
        else if (a == "--fd") o.fd = Arg(next());
        else if (a == "--image-path") o.image_path = true;
        else if (a == "--no-tracking") o.no_tracking = true;
        else if (a == "--no-subcell") o.no_subcell = true;
        else if (a == "--no-corners") o.no_corners = true;
        else if (a == "--no-predict") o.no_predict = true;
        else if (a == "--image-size") {
            o.render.view.image_width = static_cast<int>(Arg(next()));
            o.render.view.image_height = static_cast<int>(Arg(next()));
        }
        else if (a == "--distance") o.render.view.distance = Arg(next());
        else if (a == "--yaw") o.render.view.yaw_deg = Arg(next());
        else if (a == "--pitch") o.render.view.pitch_deg = Arg(next());
        else if (a == "--roll") o.render.view.roll_deg = Arg(next());
        else if (a == "--falloff") o.render.corner_falloff = Arg(next());
        else if (a == "--img-glare") o.render.glare_lift = Arg(next());
        else if (a == "--blur") o.render.blur_radius = static_cast<int>(Arg(next()));
        else if (a == "--lens-k1") o.render.lens_k1 = Arg(next());
        else if (a == "--shake") o.motion.shake = Arg(next());
        else if (a == "--pan") {
            o.motion.pan_x = Arg(next());
            o.motion.pan_y = Arg(next());
        }
        else if (a == "--exposure-ms") o.motion.exposure_s = Arg(next()) / 1000.0;
        else if (a == "--readout-ms") o.motion.readout_s = Arg(next()) / 1000.0;
        else if (a == "--img-noise") o.render.noise_amplitude = Arg(next());
        else if (a == "--record") o.record_dir = next();
        else if (a == "--fec-nsym") o.fec_nsym = static_cast<std::size_t>(Arg(next()));
        else if (a == "--erase-below") o.erase_below = static_cast<int>(Arg(next()));
        else if (a == "--adapt") o.adapt = true;
        else if (a == "--no-host-metrics") o.no_host_metrics = true;
        else { std::fprintf(stderr, "unknown flag: %s\n", a.c_str()); Usage(); return 2; }
    }

    if (auto s = o.channel.Validate(); !s.ok()) {
        std::fprintf(stderr, "bad channel config: %s\n", ErrorName(s.error()).data());
        return 2;
    }

    // Refuse impairments the chosen path cannot apply, rather than accepting and ignoring them.
    //
    // The cell-sample path and the image path have DIFFERENT impairment sets: `--occlusion`,
    // `--crosstalk`, `--gamma` and friends are applied by `sim::Channel`, which only runs when
    // `--image-path` is off. Setting one alongside `--image-path` used to be accepted in silence,
    // so a sweep could vary a knob that did nothing and report the resulting flat response as a
    // finding. That is the same silent-no-op failure as F31's fractional pitch and F33's clipped
    // screen: the run looks healthy and the conclusion is wrong.
    if (o.image_path) {
        struct Ignored { const char* flag; double value; };
        const Ignored ignored[] = {
            {"--occlusion", o.channel.occlusion_fraction},
            {"--crosstalk", o.channel.crosstalk},
            {"--vignetting", o.channel.vignetting},
            {"--glare", o.channel.glare_strength},
            {"--shot", o.channel.shot_noise_scale},
        };
        bool bad = false;
        for (const auto& i : ignored) {
            if (i.value > 0.0) {
                std::fprintf(stderr,
                             "%s is a CELL-PATH impairment and does nothing with --image-path.\n",
                             i.flag);
                bad = true;
            }
        }
        if (o.channel.gamma != 1.0) {
            std::fprintf(stderr, "--gamma is a CELL-PATH impairment and does nothing with "
                                 "--image-path.\n");
            bad = true;
        }
        if (bad) {
            std::fprintf(stderr,
                         "\nThe image path renders real pixels, so its impairments are optical:\n"
                         "  --blur, --img-noise, --falloff, --img-glare, and the view geometry\n"
                         "  (--distance, --yaw, --pitch, --roll). Refusing rather than ignoring,\n"
                         "  because a knob that silently does nothing produces a flat response\n"
                         "  curve that looks like a finding.\n");
            return 2;
        }
    }

    if (o.motion.active() && !o.image_path) {
        std::fputs("--shake / --pan move the CAMERA, so they need --image-path; refusing rather "
                   "than ignoring them.\n", stderr);
        return 2;
    }

    // Deterministic payload from a fixed seed: reproducible without storing the file.
    std::vector<std::uint8_t> payload(o.payload_bytes);
    SplitMix64 prng(0xF11EF10ULL ^ o.channel.seed);
    for (auto& b : payload) b = static_cast<std::uint8_t>(prng.Next() & 0xFF);

    GridGeometry g{o.cols, o.rows};
    auto layout_r = FrameLayout::Create(g, LayoutConfig{});
    if (!layout_r.ok()) {
        std::fprintf(stderr, "layout: %s\n", ErrorName(layout_r.error()).data());
        return 1;
    }
    const FrameLayout layout = std::move(layout_r).value();
    const M0Modulator mod(layout);
    const HeaderCodec hdr_codec;

    // FrameLayout::Create validates geometry but cannot know how large the CODED header is --
    // that belongs to HeaderCodec, a layer it does not depend on. So a grid too small for its
    // own header builds a valid-looking layout and then fails at Render with a generic
    // out-of-range error. Check it here, where both facts are in scope, and say so plainly.
    if (mod.header_capacity_bytes() < hdr_codec.coded_size()) {
        std::fprintf(stderr,
                     "grid %ux%u is too small for the coded header: band holds %zu bytes, "
                     "header needs %zu.\nReduce --marker-size/--header-rows via the layout, "
                     "or use a larger grid.\n",
                     g.cols, g.rows, mod.header_capacity_bytes(), hdr_codec.coded_size());
        return 2;
    }

    // The ONE transmit path and the ONE receive path (C13a), the same ones ffreplay and the phone
    // run. The receiver is told the grid and nothing else: code rate from each header, manifest
    // from the frames (ADR-0015).
    if (o.fec_nsym > 0 && o.symbol_size != 0) {
        std::fprintf(stderr, "note: --symbol-size %u ignored; intra-frame FEC fixes the symbol "
                             "size at its message capacity\n", o.symbol_size);
        o.symbol_size = 0;
    }
    TransmitConfig tcfg;
    tcfg.fec_nsym = o.fec_nsym;
    tcfg.symbol_size = o.symbol_size;
    tcfg.block_symbols = o.block_symbols;
    tcfg.manifest_interval = o.manifest_interval;
    auto tx_r = TransmitSession::Create(layout, payload, "sim.bin", 0x51533101, tcfg);
    if (!tx_r.ok()) {
        std::fprintf(stderr, "transmitter: %s\n", ErrorName(tx_r.error()).data());
        return 1;
    }
    TransmitSession tx = std::move(tx_r).value();
    const std::optional<IntraFec>& fec = tx.fec();

    ReceiveConfig rcfg;
    rcfg.erase_below = static_cast<Llr>(o.erase_below);
    auto rx_r = ReceiveSession::Create(layout, rcfg);
    if (!rx_r.ok()) {
        std::fprintf(stderr, "receiver: %s\n", ErrorName(rx_r.error()).data());
        return 1;
    }
    ReceiveSession rx = std::move(rx_r).value();

    // Adaptive link controller (C14). Built from the SAME IntraFec the decoder runs, because
    // every score it computes is arithmetic over that code's geometry.
    std::optional<ChannelEstimator> estimator;
    std::optional<LinkController> controller;
    if (o.adapt) {
        if (!fec.has_value()) {
            std::fputs("--adapt requires intra-frame FEC: with --fec-nsym 0 there is no code "
                       "rate to select and no erasure-load telemetry to select it from\n",
                       stderr);
            return 2;
        }
        auto e = ChannelEstimator::Create(fec->codewords(), fec->params().codeword_bytes);
        if (!e.ok()) {
            std::fprintf(stderr, "channel estimator: %s\n", ErrorName(e.error()).data());
            return 2;
        }
        estimator.emplace(std::move(e).value());

        LinkControllerConfig lcfg;
        // The running nsym must be a rung, or the controller has nowhere to step from. Insert
        // it rather than refusing: an operator sweeping --fec-nsym should not have to keep the
        // ladder in sync by hand.
        if (std::find(lcfg.ladder.begin(), lcfg.ladder.end(), o.fec_nsym) ==
            lcfg.ladder.end()) {
            lcfg.ladder.push_back(o.fec_nsym);
            std::sort(lcfg.ladder.begin(), lcfg.ladder.end());
        }
        auto c = LinkController::Create(fec->codewords(), fec->params().codeword_bytes,
                                        LinkProfile{.nsym = o.fec_nsym}, lcfg);
        if (!c.ok()) {
            std::fprintf(stderr,
                         "link controller: %s (a --fec-nsym inserted into the ladder can sit "
                         "too close to a neighbour for the promote margin to clear)\n",
                         ErrorName(c.error()).data());
            return 2;
        }
        controller.emplace(std::move(c).value());
    }

    sim::Channel channel(o.channel);

    // Image path: the production FramePipeline, sub-cell alignment included. ffsim used to wire
    // the tracker, sampler and photometry together by hand, which meant every fix to the pipeline
    // after that (F45's alignment above all) was invisible here and the simulator was measuring a
    // receiver nobody ships.
    std::optional<FramePipeline> pipeline;
    if (o.image_path) {
        if (o.render.view.image_width <= 0) o.render.view.image_width = 900;
        if (o.render.view.image_height <= 0) o.render.view.image_height = 1500;

        PipelineConfig pcfg;
        pcfg.disable_tracking = o.no_tracking;
        pcfg.subcell.enabled = !o.no_subcell;
        pcfg.subcell.refine_corners = !o.no_corners;
        pcfg.tracker.predict_motion = !o.no_predict;
        auto p = FramePipeline::Create(layout, pcfg);
        if (!p.ok()) {
            std::fprintf(stderr, "pipeline: %s\n", ErrorName(p.error()).data());
            return 1;
        }
        pipeline.emplace(std::move(p).value());
    }

    std::optional<harness::CaptureWriter> recorder;
    if (!o.record_dir.empty()) {
        if (!o.image_path) {
            std::fputs("--record requires --image-path: there are no frames to record "
                       "without it\n", stderr);
            return 2;
        }
        harness::CaptureMetadata meta;
        meta.sender_model = "ffsim (synthetic)";
        meta.receiver_model = "ffsim (synthetic)";
        meta.app_commit = "simulated";
        meta.notes = "SYNTHETIC capture from the channel simulator -- NOT a real measurement";
        meta.grid_cols = g.cols;
        meta.grid_rows = g.rows;
        meta.modulation_profile = "M0";
        meta.width = static_cast<std::uint32_t>(o.render.view.image_width);
        meta.height = static_cast<std::uint32_t>(o.render.view.image_height);
        meta.fps = o.fd;
        meta.angle_deg = o.render.view.yaw_deg;
        meta.motion_condition = "synthetic";
        meta.source_payload_bytes = o.payload_bytes;
        meta.source_payload_sha256 = ToHex(Sha256::Of(payload));
        auto w = harness::CaptureWriter::Create(o.record_dir, meta);
        if (!w.ok()) {
            std::fprintf(stderr, "cannot open bundle %s: %s\n", o.record_dir.c_str(),
                         ErrorName(w.error()).data());
            return 1;
        }
        recorder.emplace(std::move(w).value());
    }

    const auto t0 = std::chrono::steady_clock::now();

    std::uint64_t presented = 0, captured = 0;
    double worst_geo_cells = 0.0;
    double render_seconds = 0.0;
    const auto state_ns = static_cast<std::int64_t>(1e9 / o.fd);
    CellMatrix frame(g.cols, g.rows);

    while (!rx.complete() && presented < o.max_frames) {
        if (auto h = tx.Next(&frame); !h.ok()) {
            std::fprintf(stderr, "transmit: %s\n", ErrorName(h.error()).data());
            return 1;
        }
        ++presented;
        const auto ts = static_cast<std::int64_t>(presented) * state_ns;

        // Frame drop: presented but never captured. A Pc term.
        if (o.channel.frame_drop_rate > 0.0 &&
            channel.rng().NextDouble() < o.channel.frame_drop_rate) {
            // A drop is a pre-FEC loss: parity cannot recover a frame the camera never saw.
            // The receiver observes it as a sequence-number gap, so this is telemetry it really
            // does have, not simulator omniscience.
            if (estimator.has_value()) estimator->ObservePreFecLoss();
            continue;
        }
        ++captured;

        std::vector<double> samples;
        if (o.image_path) {
            // Render real pixels and make the decoder find the screen for itself.
            Homography truth;
            sim::OpticalRenderConfig rc = o.render;
            rc.seed = o.channel.seed + presented;  // decorrelate noise between frames

            // Rendering is TRANSMITTER-SIDE simulation, not receiver work, and it dominates
            // wall time in this mode. Timing it separately keeps "decoder throughput" honest
            // -- folding it in would understate the decoder by several times and produce
            // exactly the sort of conflated metric ADR-0012 exists to prevent.
            const auto r0 = std::chrono::steady_clock::now();
            sim::MotionConfig mc = o.motion;
            mc.phase_seed = o.channel.seed;
            const Image8 img =
                mc.active()
                    ? sim::RenderMoving(frame, g, rc, mc,
                                        static_cast<double>(presented - 1) / o.fd, &truth)
                    : sim::RenderView(frame, g, rc, &truth);
            render_seconds += std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - r0)
                                  .count();

            if (recorder.has_value()) {
                if (auto s = recorder->WriteFrame(img.view()); !s.ok()) {
                    std::fprintf(stderr, "record: %s\n", ErrorName(s.error()).data());
                    return 1;
                }
            }

            const std::uint64_t geo_fail_before = pipeline->diagnostics().geometry_failures;
            CapturedFrame cf = pipeline->Process(img.view(), presented, ts);
            if (pipeline->diagnostics().geometry_failures == geo_fail_before) {
                // Geometric error against ground truth, in CELLS -- only the simulator can
                // measure this, and it is the term that gates everything downstream. Measured on
                // the homography the samples were actually read with, sub-cell shift included.
                const Homography& est = pipeline->last_homography();
                for (std::uint32_t r = 0; r < g.rows; r += 8) {
                    for (std::uint32_t c = 0; c < g.cols; c += 8) {
                        const Point2 gp{static_cast<double>(c) + 0.5,
                                        static_cast<double>(r) + 0.5};
                        const Point2 a = sim::DistortPoint(truth.Apply(gp), rc);
                        const Point2 b = est.Apply(gp);
                        if (!std::isfinite(a.x) || !std::isfinite(b.x)) continue;
                        const Point2 a1 = sim::DistortPoint(truth.Apply({gp.x + 1.0, gp.y}), rc);
                        const double px_per_cell = std::hypot(a1.x - a.x, a1.y - a.y);
                        if (px_per_cell < 1e-6) continue;
                        const double err = std::hypot(a.x - b.x, a.y - b.y) / px_per_cell;
                        if (err > worst_geo_cells) worst_geo_cells = err;
                    }
                }
            }
            if (!pipeline->last_ok()) {
                if (estimator.has_value()) estimator->ObservePreFecLoss();
                continue;
            }
            samples = std::move(cf.cell_samples);
        } else {
            samples = channel.Apply(frame);
        }

        rx.Ingest(samples, ts);

        if (estimator.has_value()) {
            // Observe the load whether or not the frame decoded. The failures are the
            // informative half: a frame that exceeded its budget is the only evidence that a
            // stronger code was needed, and dropping it here would bias the estimate toward
            // exactly the rung already in use.
            if (rx.last_reached_fec()) {
                estimator->ObserveFrame(rx.last_fec_stats().worst_budget_used,
                                        rx.last_fec_stats().budget_censored);
                controller->Select(*estimator, presented);
            } else {
                estimator->ObservePreFecLoss();
            }
        }
    }

    if (recorder.has_value()) {
        if (auto s = recorder->Finish(); !s.ok()) {
            std::fprintf(stderr, "record finish: %s\n", ErrorName(s.error()).data());
            return 1;
        }
        std::printf("recorded %u frames to %s\n", recorder->frames_written(),
                    o.record_dir.c_str());
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    auto out = rx.Finish();
    // Verified means the bytes match the SENDER's file, not merely the manifest's hash: a
    // receiver that latched a wrong manifest could verify against it and still be wrong.
    const bool verified = out.ok() && out.value() == payload;
    const ReceiveStats& st = rx.stats();

    // Metric names follow docs/vision/TERMINOLOGY.md exactly. An unlabelled rate is a
    // documentation defect (ADR-0012).
    std::printf("--- ffsim (simulated; [HYP] until Phase 2 calibration) ---\n");
    std::printf("seed                     %llu\n", static_cast<unsigned long long>(o.channel.seed));
    std::printf("grid                     %ux%u  (%u cells)\n", g.cols, g.rows, g.cells());
    std::printf("payload cells / frame    %zu\n", layout.payload_cells().size());
    std::printf("frame overhead O         %.4f\n", layout.overhead_fraction());
    std::printf("payload bytes            %zu\n", o.payload_bytes);
    std::printf("fountain blocks          %u x %u symbols of %u bytes\n",
                tx.manifest().block_count, tx.manifest().block_symbols,
                tx.manifest().symbol_size);
    std::printf("manifest interval        %u display states\n",
                tx.transmitter().manifest_interval());
    std::printf("display states presented %llu\n", static_cast<unsigned long long>(presented));
    std::printf("frames captured          %llu\n", static_cast<unsigned long long>(captured));
    std::printf("header success H         %.4f\n", st.header_success());
    std::printf("header failures          %llu\n", static_cast<unsigned long long>(st.header_fail));
    std::printf("headers rescued by erasures %llu\n",
                static_cast<unsigned long long>(st.header_rescued_by_erasures));
    std::printf("cell erasure rate        %.4f\n", st.cell_erasure_rate());
    std::printf("manifest frames read     %llu  (%llu symbols read before the first)\n",
                static_cast<unsigned long long>(st.manifests),
                static_cast<unsigned long long>(st.buffered));
    if (o.image_path) {
        const PipelineDiagnostics& d = pipeline->diagnostics();
        std::printf("\n--- geometry (image path) ---\n");
        std::printf("capture resolution       %dx%d\n", o.render.view.image_width,
                    o.render.view.image_height);
        std::printf("view                     distance %.2f, yaw %.1f, pitch %.1f, roll %.1f\n",
                    o.render.view.distance, o.render.view.yaw_deg, o.render.view.pitch_deg,
                    o.render.view.roll_deg);
        std::printf("tracking                 %s\n",
                    o.no_tracking ? "DISABLED (full acquisition per frame)" : "enabled");
        std::printf("sub-cell alignment       %s\n", o.no_subcell ? "DISABLED" : "enabled");
        if (o.motion.active()) {
            std::printf("motion                   shake %.2f, pan %.3f/%.3f per s, exposure %.1f ms, "
                        "readout %.1f ms\n",
                        o.motion.shake, o.motion.pan_x, o.motion.pan_y,
                        o.motion.exposure_s * 1e3, o.motion.readout_s * 1e3);
        }
        std::printf("refined frames           %llu\n",
                    static_cast<unsigned long long>(pipeline->tracker().refined_frames()));
        std::printf("full acquisitions        %llu\n",
                    static_cast<unsigned long long>(pipeline->tracker().full_acquisitions()));
        std::printf("tracking losses          %llu\n",
                    static_cast<unsigned long long>(pipeline->tracker().losses()));
        std::printf("detection failures       %llu\n",
                    static_cast<unsigned long long>(d.geometry_failures));
        std::printf("photometric failures     %llu\n",
                    static_cast<unsigned long long>(d.photometric_failures));
        // Worst, not mean: a mean hides the one bad frame that ruins an edge.
        std::printf("worst geometric error    %.4f cells\n", worst_geo_cells);
        if (d.subcell_frames > 0) {
            std::printf("sub-cell shift / gain    %.3f cells mean, %.3fx pilot contrast\n",
                        d.mean_subcell_shift(), d.mean_subcell_gain());
        }
        const double px_per_frame = captured ? static_cast<double>(d.total_pixels_examined) /
                                                   static_cast<double>(captured)
                                             : 0.0;
        const double full_frame_px = static_cast<double>(o.render.view.image_width) *
                                     static_cast<double>(o.render.view.image_height);
        std::printf("geometry pixels/frame    %.0f  (%.3f of a full-image scan)\n",
                    px_per_frame, full_frame_px > 0 ? px_per_frame / full_frame_px : 0.0);
        std::printf("  compare --no-tracking to get the ADR-0006 A/B\n");
        std::printf("\n");
    }

    if (fec.has_value()) {
        std::printf("\n--- intra-frame FEC (ADR-0009) ---\n");
        std::printf("RS parity per codeword   %zu bytes (corrects %zu erasures / %zu errors)\n",
                    fec->params().nsym, fec->params().nsym, fec->params().nsym / 2);
        std::printf("codewords per frame      %zu (interleaved)\n", fec->codewords());
        std::printf("FEC code rate Rfec       %.4f\n", fec->params().code_rate());
        std::printf("frames FEC-recovered     %llu\n", static_cast<unsigned long long>(st.fec_ok));
        std::printf("frames uncorrectable     %llu\n",
                    static_cast<unsigned long long>(st.fec_fail));
        // HEADROOM, not just success. A run that peaked at 31 of 32 decoded fine and was one
        // bad cell from failing -- the adaptive controller needs to see that difference.
        std::printf("worst codeword load      %zu / %zu erasures (%.0f%% of budget)\n",
                    st.worst_codeword_erasures, fec->params().nsym,
                    100.0 * static_cast<double>(st.worst_codeword_erasures) /
                        static_cast<double>(fec->params().nsym));
        std::printf("\n");
    }

    if (estimator.has_value()) {
        const ChannelEstimator& est = *estimator;
        std::printf("\n--- adaptive link controller (C14; ADP-01 estimate, ADP-02 policy) ---\n");
        std::printf("frames into estimator    %llu  (%llu pre-FEC losses)\n",
                    static_cast<unsigned long long>(est.frames_observed()),
                    static_cast<unsigned long long>(est.pre_fec_losses()));
        std::printf("P(frame reaches FEC)     %.4f\n", est.pre_fec_success());
        std::printf("codeword budget used     mean %.1f, median %zu, p95 %zu, worst %zu bytes"
                    "  (2*errors + erasures)\n",
                    est.mean_budget(), est.BudgetQuantile(0.5), est.BudgetQuantile(0.95),
                    est.worst_budget_seen());
        if (est.censored_observations() > 0) {
            // Not a footnote: these frames were bounded, not measured, and the bound is
            // optimistic for every rung above the running one.
            std::printf("censored observations    %llu of %llu  <-- frames that did not decode, "
                        "so their budget is a FLOOR.\n"
                        "                         Predictions for rungs ABOVE %zu are "
                        "optimistic by up to this share.\n",
                        static_cast<unsigned long long>(est.censored_observations()),
                        static_cast<unsigned long long>(est.frames_observed()), o.fec_nsym);
        }

        // The full response surface, not just the winner: a sharp peak and a broad plateau call
        // for different decisions, and only the table shows which one this is (EXP-013's rule,
        // applied here for the same reason).
        std::printf("\n  nsym    Rfec   msg bytes   P(decode)   expected bytes / display state\n");
        for (const RungScore& r : controller->ScoreLadder(est)) {
            std::printf("  %4zu  %.4f      %6.0f      %.4f   %10.1f%s\n", r.nsym, r.code_rate,
                        r.message_bytes_per_frame, r.predicted_frame_success, r.score,
                        r.nsym == o.fec_nsym ? "   <- running" : "");
        }

        const RungScore best = controller->BestRung(est);
        const double running = [&] {
            for (const RungScore& r : controller->ScoreLadder(est)) {
                if (r.nsym == o.fec_nsym) return r.score;
            }
            return 0.0;
        }();
        std::printf("\nrecommended nsym         %zu   (from receiver telemetry alone)\n",
                    best.nsym);
        std::printf("running nsym             %zu\n", o.fec_nsym);
        if (running > 0.0) {
            std::printf("predicted headroom       %+.1f%% expected bytes/display state vs "
                        "the running rung\n",
                        100.0 * (best.score / running - 1.0));
        }
        std::printf("hysteresis walk ended at %zu after %llu profile change(s)\n",
                    controller->current().nsym,
                    static_cast<unsigned long long>(controller->changes()));
        // The recommendation is the unconstrained argmax; the walk is what the policy would
        // actually have done in the frames available. On a short transfer those differ for a
        // mundane reason worth stating, or the two lines look contradictory.
        if (controller->changes() == 0 && best.nsym != o.fec_nsym) {
            const auto dwell = controller->config().promote_dwell_frames;
            if (presented < dwell + controller->config().min_observations) {
                std::printf("  the walk did not move because this transfer (%llu display "
                            "states) is shorter than\n  the promote dwell (%llu frames) plus "
                            "the observation minimum -- for a session this\n  short only the "
                            "session-START choice matters, which is F20's point exactly.\n",
                            static_cast<unsigned long long>(presented),
                            static_cast<unsigned long long>(dwell));
            }
        }

        // Without this note the number above reads as an achievable gain. It is not one yet.
        std::printf(
            "\n  NOTE: reported, NOT actuated. nsym sets the FEC message size, which sets the\n"
            "  fountain symbol size, which the manifest fixes for the whole session -- so this\n"
            "  is a session-START decision, not a mid-session control loop. And on a one-way\n"
            "  link the transmitter cannot hear it at all (OQ-013). See finding F20.\n");
        std::printf("\n");
    }

    const FileReceiver* frx = rx.receiver();
    std::printf("fountain overhead        %.4f\n", frx ? frx->overhead() : 0.0);
    std::printf("blocks complete          %u / %u\n", frx ? frx->blocks_complete() : 0u,
                tx.manifest().block_count);
    std::printf("VERIFIED                 %s\n", verified ? "yes (SHA-256 match)" : "NO");
    if (!out.ok()) std::printf("failure                  %s\n", ErrorName(out.error()).data());

    // ---- Rates. ADR-0012: name the metric, always. These are DIFFERENT quantities and
    // conflating them is the single failure mode this project exists to avoid.
    const double payload_bits_per_frame =
        static_cast<double>(layout.payload_cells().size());  // M0 = 1 bit per payload cell
    const double raw_bit_rate = payload_bits_per_frame * o.fd;
    const double optical_seconds = static_cast<double>(presented) / o.fd;
    const double goodput = static_cast<double>(o.payload_bytes) / optical_seconds;

    std::printf("\n--- rates (see docs/vision/TERMINOLOGY.md) ---\n");
    std::printf("assumed display rate Fd  %.1f states/s   [ASSUMPTION, not measured]\n", o.fd);
    std::printf("raw optical bit rate     %.2f Mb/s\n", raw_bit_rate / 1e6);
    std::printf("optical transfer time    %.3f s  (%llu states / Fd)\n", optical_seconds,
                static_cast<unsigned long long>(presented));
    if (verified) {
        std::printf("PAYLOAD GOODPUT          %.1f KB/s   <-- the metric (ADR-0012)\n",
                    goodput / 1024.0);
    }
    // Reported separately and NEVER as goodput: this is how fast THIS DESKTOP chews through
    // frames, which says nothing about the optical channel. It matters only as a check that
    // the decoder could keep up in real time (RISK-006).
    // Simulator rendering is transmitter-side and must not be charged to the decoder.
    //
    // These three lines are also the ONLY output above that is not reproducible from the flags
    // plus the commit, which is why --no-host-metrics suppresses exactly them and nothing else.
    if (!o.no_host_metrics) {
        const double decode_secs = secs - render_seconds;
        const double decode_fps =
            decode_secs > 0.0 ? static_cast<double>(captured) / decode_secs : 0.0;
        std::printf("decoder throughput       %.0f frames/s on this host (NOT a channel rate)\n",
                    decode_fps);
        std::printf("  real-time headroom     %.1fx vs Fd=%.0f\n", decode_fps / o.fd, o.fd);
        if (o.image_path) {
            std::printf("  (simulator rendering    %.1f s of %.1f s wall, excluded above)\n",
                        render_seconds, secs);
        }
    }
    return verified ? 0 : 1;
}
