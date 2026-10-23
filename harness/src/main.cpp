// ffreplay — decode a recorded capture bundle through the production decode chain.
//
// Reports the same metric names as ffsim so replayed and simulated runs are directly
// comparable. The difference between the two IS the simulator's error, which is how RISK-024
// (a simulator kinder than reality) gets measured rather than worried about.
#include <fileflow/frame.h>
#include <fileflow/harness/capture.h>
#include <fileflow/hash.h>
#include <fileflow/modulation.h>
#include <fileflow/session.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

using namespace fileflow;

void Usage() {
    std::puts(
        "ffreplay — replay a recorded FileFlow capture bundle\n"
        "\n"
        "  ffreplay <bundle-dir> [options]\n"
        "\n"
        "  --no-tracking          full acquisition every frame (ADR-0006 A/B)\n"
        "  --margin F             sampler interior margin (default 0.3)\n"
        "  --samples N            sampler subsamples per axis (default 3)\n"
        "  --no-subcell           disable sub-cell grid alignment (A/B baseline)\n"
        "  --erase-below L        erase payload cells with |llr| < L (0 = off)\n"
        "  --out FILE             write the received file here if, and only if, it verifies\n"
        "  --map                  print one frame's separation and residual maps\n"
        "  --info                 print metadata and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 2;
    }
    const std::string bundle = argv[1];

    PipelineConfig cfg;
    ReceiveConfig rcfg;
    bool info_only = false;
    bool want_map = false;
    std::string out_path;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--help" || a == "-h") { Usage(); return 0; }
        else if (a == "--no-tracking") cfg.disable_tracking = true;
        else if (a == "--margin") cfg.sampler.interior_margin = std::strtod(next(), nullptr);
        else if (a == "--samples") {
            cfg.sampler.samples_per_axis = static_cast<int>(std::strtol(next(), nullptr, 10));
        } else if (a == "--info") info_only = true;
        else if (a == "--no-subcell") cfg.subcell.enabled = false;
        else if (a == "--erase-below") {
            rcfg.erase_below = static_cast<Llr>(std::strtol(next(), nullptr, 10));
        } else if (a == "--out") out_path = next();
        else if (a == "--map") want_map = true;
        else if (a == "--residual") cfg.photometric.max_pilot_residual_ratio = std::strtod(next(), nullptr);
        else if (a == "--minsep") cfg.photometric.min_separation = std::strtod(next(), nullptr);
        else { std::fprintf(stderr, "unknown flag: %s\n", a.c_str()); return 2; }
    }

    // Read metadata first so --info works on a bundle whose grid does not match any layout we
    // would build, and so the grid comes FROM the bundle rather than being assumed.
    std::string meta_text;
    {
        const std::string path = bundle + "/capture.meta";
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            return 1;
        }
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) meta_text.append(buf, n);
        std::fclose(f);
    }

    auto meta_r = harness::CaptureMetadata::Parse(meta_text);
    if (!meta_r.ok()) {
        std::fprintf(stderr, "bad metadata: %s\n", ErrorName(meta_r.error()).data());
        return 1;
    }
    const harness::CaptureMetadata& meta = meta_r.value();

    std::printf("--- capture bundle ---\n");
    std::printf("sender / receiver        %s -> %s\n",
                meta.sender_model.empty() ? "(unrecorded)" : meta.sender_model.c_str(),
                meta.receiver_model.empty() ? "(unrecorded)" : meta.receiver_model.c_str());
    std::printf("app commit               %s\n",
                meta.app_commit.empty() ? "(unrecorded)" : meta.app_commit.c_str());
    std::printf("grid                     %ux%u\n", meta.grid_cols, meta.grid_rows);
    std::printf("capture                  %ux%u @ %.1f fps, %u frames\n", meta.width,
                meta.height, meta.fps, meta.frame_count);
    std::printf("rig                      distance %.1f cm, angle %.1f deg, %s\n",
                meta.distance_cm, meta.angle_deg,
                meta.motion_condition.empty() ? "(unrecorded)" : meta.motion_condition.c_str());

    // An incomplete capture is still replayable, but it is NOT evidence. Say so loudly rather
    // than letting a half-labelled dataset quietly become a cited result.
    const auto missing = meta.MissingRequiredFields();
    if (!missing.empty()) {
        std::printf("\n⚠ INCOMPLETE METADATA — not usable as experimental evidence.\n");
        std::printf("  missing:");
        for (const auto& f : missing) std::printf(" %s", f.c_str());
        std::printf("\n");
    }
    if (info_only) return 0;

    auto layout_r = FrameLayout::Create(GridGeometry{meta.grid_cols, meta.grid_rows},
                                        LayoutConfig{});
    if (!layout_r.ok()) {
        std::fprintf(stderr, "layout: %s\n", ErrorName(layout_r.error()).data());
        return 1;
    }
    const FrameLayout layout = std::move(layout_r).value();

    auto src_r = harness::ReplaySource::Create(bundle, layout, cfg);
    if (!src_r.ok()) {
        std::fprintf(stderr, "replay: %s\n", ErrorName(src_r.error()).data());
        return 1;
    }
    harness::ReplaySource src = std::move(src_r).value();

    std::uint64_t usable_cells = 0;
    std::uint64_t erased_cells = 0;
    bool map_printed = false;

    // Run the WHOLE receive path on every frame that produced cell samples: header, payload FEC,
    // in-band manifest, fountain, hash gate -- the session layer the phone runs (C13a).
    //
    // This tool once stopped at cell samples, so a run could report "37 frames decoded" while being
    // completely unable to read a single byte; then it stopped at the header, so it could read bytes
    // but never say whether a file arrived. Only a verified transfer yields goodput, and the
    // receiver here is told nothing but the grid -- exactly what a real capture gives it.
    auto rx_r = ReceiveSession::Create(layout, rcfg);
    if (!rx_r.ok()) {
        std::fprintf(stderr, "receiver: %s\n", ErrorName(rx_r.error()).data());
        return 1;
    }
    ReceiveSession session = std::move(rx_r).value();
    std::optional<std::int64_t> first_ts;
    std::optional<std::int64_t> complete_ts;
    std::uint64_t frames_seen = 0;
    std::uint64_t frames_to_complete = 0;

    while (auto f = src.Next()) {
        ++frames_seen;
        if (!first_ts) first_ts = f->timestamp_ns;
        // One frame's worth of spatial structure, printed once. See PipelineDiagnostics: the
        // aggregates cannot separate drift from glare from moire, and these maps can.
        if (want_map && !map_printed) {
            if (const PhotometricField* pf = src.pipeline().last_field()) {
                map_printed = true;
                const GridGeometry g = layout.geometry();
                const int kCols = 48;
                const int kRows = 60;
                std::printf("\n--- separation across the frame (first field) ---\n");
                std::printf("    digit = local separation / 20, capped; '.' = under 12 (erases)\n");
                for (int ry = 0; ry < kRows; ++ry) {
                    const auto row = static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(ry) * (g.rows - 1)) / (kRows - 1));
                    std::printf("    ");
                    for (int rx = 0; rx < kCols; ++rx) {
                        const auto col = static_cast<std::uint32_t>(
                            (static_cast<std::uint64_t>(rx) * (g.cols - 1)) / (kCols - 1));
                        const PhotometricRef r = pf->RefAt(col, row);
                        const double sep = r.bright - r.dark;
                        if (!(sep >= 12.0)) { std::printf("."); continue; }
                        const int d = static_cast<int>(sep / 20.0);
                        std::printf("%c", d > 9 ? '#' : static_cast<char>('0' + d));
                    }
                    std::printf("\n");
                }
                std::printf("\n--- pilot residual / separation (budget 0.25) ---\n");
                std::printf("    '.' inside budget, digit = ratio x 10, '#' >= 1.0\n");
                for (int ry = 0; ry < kRows; ++ry) {
                    const auto row = static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(ry) * (g.rows - 1)) / (kRows - 1));
                    std::printf("    ");
                    for (int rx = 0; rx < kCols; ++rx) {
                        const auto col = static_cast<std::uint32_t>(
                            (static_cast<std::uint64_t>(rx) * (g.cols - 1)) / (kCols - 1));
                        const PhotometricRef r = pf->RefAt(col, row);
                        const double sep = r.bright - r.dark;
                        const double res = pf->ResidualAt(col, row);
                        if (!(sep > 0.0)) { std::printf(" "); continue; }
                        const double ratio = res / sep;
                        if (ratio < 0.25) { std::printf("."); continue; }
                        const int d = static_cast<int>(ratio * 10.0);
                        std::printf("%c", d > 9 ? '#' : static_cast<char>('0' + d));
                    }
                    std::printf("\n");
                }
                std::printf("\n");
            }
        }
        bool any_usable = false;
        for (const double v : f->cell_samples) {
            if (std::isnan(v)) {
                ++erased_cells;
            } else {
                ++usable_cells;
                any_usable = true;
            }
        }
        if (!any_usable) continue;

        // Keep decoding after completion: the header, erasure and state-rate statistics are
        // measurements in their own right and should cover the whole capture.
        const bool was_complete = session.complete();
        session.Ingest(f->cell_samples, f->timestamp_ns);
        if (!was_complete && session.complete()) {
            complete_ts = f->timestamp_ns;
            frames_to_complete = frames_seen;
        }
    }
    const ReceiveStats& rs = session.stats();
    const std::uint64_t header_ok = rs.header_ok;
    const std::uint64_t header_fail = rs.header_fail;

    const auto& d = src.pipeline().diagnostics();
    std::printf("\n--- decode (production chain; docs/vision/TERMINOLOGY.md) ---\n");
    std::printf("frames in                %llu\n",
                static_cast<unsigned long long>(d.frames_in));
    std::printf("frames decoded           %llu\n",
                static_cast<unsigned long long>(d.frames_decoded));
    std::printf("geometry failures        %llu\n",
                static_cast<unsigned long long>(d.geometry_failures));
    std::printf("photometric failures     %llu\n",
                static_cast<unsigned long long>(d.photometric_failures));
    std::printf("full acquisitions        %llu\n",
                static_cast<unsigned long long>(src.pipeline().tracker().full_acquisitions()));
    std::printf("refined frames           %llu\n",
                static_cast<unsigned long long>(src.pipeline().tracker().refined_frames()));
    const double px_per_frame =
        d.frames_in ? static_cast<double>(d.total_pixels_examined) /
                          static_cast<double>(d.frames_in)
                    : 0.0;
    std::printf("geometry pixels/frame    %.0f\n", px_per_frame);
    const std::uint64_t total_cells = usable_cells + erased_cells;
    const double erasure = total_cells ? static_cast<double>(erased_cells) /
                                             static_cast<double>(total_cells)
                                       : 0.0;
    std::printf("cell erasure rate        %.4f\n", erasure);

    const std::uint64_t header_attempts = header_ok + header_fail;
    std::printf("\n--- header (the gate between seeing and reading) ---\n");
    std::printf("header attempts          %llu\n",
                static_cast<unsigned long long>(header_attempts));
    std::printf("header success H         %.4f  (%llu ok, %llu failed)\n",
                header_attempts ? static_cast<double>(header_ok) /
                                      static_cast<double>(header_attempts)
                                : 0.0,
                static_cast<unsigned long long>(header_ok),
                static_cast<unsigned long long>(header_fail));
    if (header_ok == 0 && header_attempts > 0) {
        std::printf("  Not one header decoded. The screen is being FOUND but not READ, so no\n");
        std::printf("  payload is recoverable at this erasure rate whatever the fountain layer\n");
        std::printf("  does -- the header is RS-protected and replicated, and it still failed.\n");
    }

    // Photometric detail, printed whenever anything reached the photometry stage. A high erasure
    // rate has two entirely different causes and they need opposite fixes, so the rate alone is not
    // actionable -- which is where the first real capture stalled.
    if (d.photometric_frames > 0) {
        std::printf("\n--- photometry (why cells were erased) ---\n");
        std::printf("frames with a field      %llu\n",
                    static_cast<unsigned long long>(d.photometric_frames));
        std::printf("pilots used / frame      %.0f bright, %.0f dark\n",
                    d.mean_bright_pilots(), d.mean_dark_pilots());
        std::printf("mean local separation    %.1f  (erased below %.1f)\n",
                    d.mean_separation(), cfg.photometric.min_separation);
        std::printf("mean pilot residual      %.1f  (erased above %.2f x separation = %.1f)\n",
                    d.mean_residual(), cfg.photometric.max_pilot_residual_ratio,
                    d.mean_separation() * cfg.photometric.max_pilot_residual_ratio);
        std::printf("bright nonuniformity     %.2f  (1.0 = flat; RISK-025 predicts >1)\n",
                    d.mean_bright_nonuniformity());
        // Per-node, because the means above can look healthy while half the cells erase: erasure is
        // decided node by node and the failures cluster spatially.
        std::printf("nodes below separation   %.4f of lattice\n", d.fraction_low_separation());
        std::printf("nodes over residual      %.4f of lattice\n", d.fraction_high_residual());

        // Why the tracker never refined. Without this, "refined frames 0" is a dead end: four
        // different gates produce it and they need opposite fixes.
        const ScreenTracker& tr = src.pipeline().tracker();
        std::printf("\n--- why refinement was rejected (ADR-0006's fast path) ---\n");
        std::printf("no extremes in window    %llu\n",
                    static_cast<unsigned long long>(tr.refine_rejects_no_extremes()));
        std::printf("corner jump too large    %llu  (bound %.2f of quad scale)\n",
                    static_cast<unsigned long long>(tr.refine_rejects_corner_jump()),
                    cfg.tracker.max_corner_jump);
        std::printf("homography degenerate    %llu\n",
                    static_cast<unsigned long long>(tr.refine_rejects_homography()));
        std::printf("marker score too low     %llu  (needed %.2f, best rejected %.3f)\n",
                    static_cast<unsigned long long>(tr.refine_rejects_marker_score()),
                    cfg.tracker.detection.min_marker_score, tr.worst_rejected_score());
        if (erasure > 0.5) {
            std::printf("\n  READING THIS: a high erasure rate is two different problems.\n");
            if (d.mean_separation() < cfg.photometric.min_separation * 2.0) {
                std::printf("  * SEPARATION is low. The two levels are not far enough apart to\n");
                std::printf("    threshold -- defocus, or an exposure that crushed one level.\n");
            }
            if (d.mean_residual() >
                d.mean_separation() * cfg.photometric.max_pilot_residual_ratio) {
                std::printf("  * RESIDUAL exceeds its budget. Pilots disagree with their own\n");
                std::printf("    fitted field: occlusion, glare, or a sampling grid sitting off\n");
                std::printf("    the cells because the homography is slightly wrong (F8).\n");
            }
        }
    }

    // ---- Display states (EXP-006). Three rates, kept apart (ADR-0012).
    std::printf("\n--- display states (EXP-006; from header sequence numbers) ---\n");
    std::printf("distinct states read     %llu  (%llu repeat captures of a state already read)\n",
                static_cast<unsigned long long>(rs.distinct_sequences),
                static_cast<unsigned long long>(rs.repeated_sequences));
    if (rs.seconds_observed() > 0.0) {
        const std::uint64_t span = rs.last_sequence - rs.first_sequence + 1;
        std::printf("sequence span            %u..%u over %.3f s\n", rs.first_sequence,
                    rs.last_sequence, rs.seconds_observed());
        std::printf("submission rate          %.2f states/s  (transmitter's own counter)\n",
                    rs.submission_rate());
        std::printf("distinct-state rate      %.2f states/s  (read by THIS receiver; a lower "
                    "bound on Fd)\n", rs.distinct_rate());
        std::printf("states never read        %llu of %llu in span\n",
                    static_cast<unsigned long long>(span - rs.distinct_sequences),
                    static_cast<unsigned long long>(span));
        if (meta.fps > 0.0 && rs.submission_rate() > 0.0 && meta.fps >= 2.0 * rs.submission_rate()) {
            std::printf("  camera >= 2x the state rate, so a state never read was most likely\n"
                        "  never PRESENTED -- the dropped-state term Fd exists to measure.\n");
        } else {
            std::printf("  camera < 2x the state rate: a state never read may simply have fallen\n"
                        "  between captures, so this is NOT a measurement of display drops.\n");
        }
    } else {
        std::printf("  no usable timestamps, so no rate can be stated.\n");
    }

    // ---- The transfer itself.
    std::printf("\n--- transfer (session layer C13a; verified or nothing, G3) ---\n");
    std::printf("headers rescued by erasures %llu  (F50)\n",
                static_cast<unsigned long long>(rs.header_rescued_by_erasures));
    std::printf("payload FEC ok / failed  %llu / %llu  (worst codeword %zu erasures)\n",
                static_cast<unsigned long long>(rs.fec_ok),
                static_cast<unsigned long long>(rs.fec_fail), rs.worst_codeword_erasures);
    std::printf("payload CRC rejections   %llu\n", static_cast<unsigned long long>(rs.crc_fail));
    std::printf("manifest frames read     %llu  (%llu rejected)\n",
                static_cast<unsigned long long>(rs.manifests),
                static_cast<unsigned long long>(rs.manifests_rejected));
    std::printf("symbols ingested         %llu  (%llu read before the manifest)\n",
                static_cast<unsigned long long>(rs.symbols),
                static_cast<unsigned long long>(rs.buffered));
    if (rs.foreign > 0) {
        std::printf("foreign-session frames   %llu  (another transmitter in view)\n",
                    static_cast<unsigned long long>(rs.foreign));
    }

    const FileManifest* m = session.manifest();
    if (m == nullptr) {
        std::printf("manifest                 NOT RECEIVED -- no transfer can complete without "
                    "it\n");
        std::printf("VERIFIED                 NO\n");
        std::printf("\n(no goodput: nothing was verified)\n");
        return d.frames_decoded > 0 ? 1 : 2;
    }
    std::printf("manifest                 \"%s\", %llu bytes, %u x %u symbols of %u bytes\n",
                m->file_name.c_str(), static_cast<unsigned long long>(m->file_size),
                m->block_count, m->block_symbols, m->symbol_size);
    std::printf("progress                 %.1f%%  (%u / %u blocks)\n", 100.0 * session.progress(),
                session.receiver()->blocks_complete(), m->block_count);
    if (session.receiver() != nullptr) {
        std::printf("fountain overhead        %.4f\n", session.receiver()->overhead());
    }

    auto out = session.Finish();
    if (!out.ok()) {
        std::printf("VERIFIED                 NO (%s)\n", ErrorName(out.error()).data());
        std::printf("\n(no goodput: nothing was verified)\n");
        return 1;
    }
    std::printf("VERIFIED                 yes (SHA-256 match)\n");

    // The bundle may also record what the transmitter was told to send. When it does, check the
    // received file against THAT, not only against the manifest that arrived over the same channel.
    if (!meta.source_payload_sha256.empty()) {
        const bool match = ToHex(Sha256::Of(out.value())) == meta.source_payload_sha256;
        std::printf("matches recorded source  %s\n", match ? "yes" : "NO -- the manifest and the "
                                                                    "bundle's source disagree");
        if (!match) return 1;
    }

    // Goodput: verified bytes over the capture time it took, first frame to the completing one.
    // Frames after completion are excluded; frames before the first readable one are not, because
    // a receiver pays for them in wall-clock time.
    double seconds = 0.0;
    const char* basis = "";
    if (first_ts && complete_ts && *complete_ts > *first_ts) {
        // N frames span N-1 timestamp intervals; the completing frame occupies one more period,
        // so add it -- otherwise replay reads ~1/N faster than ffsim on the identical transfer.
        seconds = static_cast<double>(*complete_ts - *first_ts) * 1e-9 +
                  (meta.fps > 0.0 ? 1.0 / meta.fps : 0.0);
        basis = "capture timestamps";
    } else if (meta.fps > 0.0) {
        seconds = static_cast<double>(frames_to_complete) / meta.fps;
        basis = "frame count / recorded fps";
    }
    if (seconds > 0.0) {
        std::printf("transfer time            %.3f s  (%llu frames; from %s)\n", seconds,
                    static_cast<unsigned long long>(frames_to_complete), basis);
        std::printf("PAYLOAD GOODPUT          %.1f KB/s   <-- the metric (ADR-0012)\n",
                    static_cast<double>(out.value().size()) / seconds / 1024.0);
    } else {
        std::printf("(verified, but no timestamps or fps recorded, so no rate can be stated)\n");
    }

    if (!out_path.empty()) {
        // Written only after the hash gate, and only to the path the operator named (PROTOCOL-SPEC
        // file layer). The manifest's file name is a display hint and is never used as a path.
        std::FILE* f = std::fopen(out_path.c_str(), "wb");
        if (f == nullptr ||
            std::fwrite(out.value().data(), 1, out.value().size(), f) != out.value().size()) {
            std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
            if (f != nullptr) std::fclose(f);
            return 1;
        }
        std::fclose(f);
        std::printf("written                  %s\n", out_path.c_str());
    }
    return 0;
}
