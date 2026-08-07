// ffreplay — decode a recorded capture bundle through the production decode chain.
//
// Reports the same metric names as ffsim so replayed and simulated runs are directly
// comparable. The difference between the two IS the simulator's error, which is how RISK-024
// (a simulator kinder than reality) gets measured rather than worried about.
#include <fileflow/frame.h>
#include <fileflow/harness/capture.h>
#include <fileflow/modulation.h>
#include <fileflow/modulation.h>

#include <cstdio>
#include <cstdlib>
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
    bool info_only = false;
    bool want_map = false;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--help" || a == "-h") { Usage(); return 0; }
        else if (a == "--no-tracking") cfg.disable_tracking = true;
        else if (a == "--margin") cfg.sampler.interior_margin = std::strtod(next(), nullptr);
        else if (a == "--samples") {
            cfg.sampler.samples_per_axis = static_cast<int>(std::strtol(next(), nullptr, 10));
        } else if (a == "--info") info_only = true;
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

    // Attempt the HEADER on every frame that produced cell samples.
    //
    // This tool used to stop at cell samples, so a run could report "37 frames decoded" while being
    // completely unable to read a single byte. "We can see the screen" and "we can read it" are
    // different claims and only the second leads anywhere. `H` is also a term in the goodput model
    // and had never been measured on real optics.
    const M0Modulator mod(layout);
    const HeaderCodec hdr_codec;
    std::uint64_t header_ok = 0, header_fail = 0;

    while (auto f = src.Next()) {
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

        const PhotometricRef ref = mod.EstimateReference(f->cell_samples);
        auto hbytes = mod.DemodulateHeader(f->cell_samples, ref, hdr_codec.coded_size());
        if (!hbytes.ok()) { ++header_fail; continue; }
        std::vector<std::uint8_t> hbuf = hbytes.value();
        auto h = hdr_codec.Decode(hbuf);
        if (!h.ok()) { ++header_fail; continue; }
        ++header_ok;
    }

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

    // Deliberately NOT reported: goodput. That needs a full transfer with a verified hash, and
    // this tool decodes frames to cell samples. Printing a rate here would invite exactly the
    // metric confusion ADR-0012 exists to prevent.
    std::printf("\n(no goodput reported: this tool decodes frames, it does not run a "
                "verified transfer)\n");
    return d.frames_decoded > 0 ? 0 : 1;
}
