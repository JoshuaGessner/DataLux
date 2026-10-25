// ffframe: why can't this phone read that one? Cell by cell, from a real camera frame.
//
// Takes a frame saved by the receiver (FrameDump: Y then U then V, packed), runs it through the
// same pipeline the phone runs, reads its header, and regenerates EXACTLY what the sender drew for
// that display state -- possible for the app's generated test file, whose bytes, envelope and
// session id are all deterministic. Then it compares the two, cell by cell: which colours are
// confused with which, how much each colour wanders, where on the screen the errors are, how they
// depend on a cell's neighbours, and whether the frame's error correction would have survived.
//
//   ffframe FRAME.yuv WIDTH HEIGHT [--grid 120x260] [--test-bytes 614400]
//           [--file PATH [--name NAME]] [--compress-ms N] [--settle N]
//
// A real file works too, given the file the sender sent: its envelope is rebuilt (version 2,
// zstd as the app's Auto mode does it) for each compress time the sender could have recorded,
// until one gives the session id in the frame's header. --compress-ms skips the search (the
// sender's log says "compressed in N ms").
//
// --settle N runs the frame through the pipeline N times first (default 40), as a phone held
// steady would see it, so what the receiver learns over frames -- the lens's distortion above
// all -- has converged before the frame is judged.
//   ffframe --make OUT.yuv COLOURS SEQUENCE [CHROMA_BLUR]   (a simulated 800x1300 frame, to check
//                                                            the tool against known truth)
#include <fileflow/compress.h>
#include <fileflow/frame.h>
#include <fileflow/hash.h>
#include <fileflow/modulation.h>
#include <fileflow/pipeline.h>
#include <fileflow/session.h>
#include <fileflow/sim/render.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace fileflow;

// java.util.Random, exactly: the app's test file is `Random(5).nextBytes(n)`.
class JavaRandom {
  public:
    explicit JavaRandom(std::int64_t seed)
        : seed_((static_cast<std::uint64_t>(seed) ^ 0x5DEECE66DULL) & kMask) {}
    std::int32_t NextInt() {
        seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & kMask;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(seed_ >> 16));
    }
    void NextBytes(std::vector<std::uint8_t>* out) {
        std::size_t i = 0;
        while (i < out->size()) {
            std::int32_t rnd = NextInt();
            for (int n = 0; n < 4 && i < out->size(); ++n) {
                (*out)[i++] = static_cast<std::uint8_t>(rnd & 0xFF);
                rnd >>= 8;
            }
        }
    }

  private:
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;
    std::uint64_t seed_;
};

// The envelope a plain, uncompressed transfer carries (app/.../secure/Envelope.kt).
std::vector<std::uint8_t> Envelope(const std::vector<std::uint8_t>& data, const std::string& name) {
    std::vector<std::uint8_t> out = {'F', 'F', 'E', '1', 0};
    out.push_back(static_cast<std::uint8_t>(name.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(name.size() & 0xFF));
    out.insert(out.end(), name.begin(), name.end());
    const auto size = static_cast<std::uint64_t>(data.size());
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// Version 2 (app/.../secure/Envelope.kt): codec 0 none, 2 zstd; the compress time is recorded.
std::vector<std::uint8_t> Envelope2(const std::vector<std::uint8_t>& data, const std::string& name,
                                    const std::vector<std::uint8_t>& body, std::uint8_t codec,
                                    std::uint32_t compress_ms) {
    std::vector<std::uint8_t> out = {'F', 'F', 'E', '2', 0};
    out.push_back(static_cast<std::uint8_t>(name.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(name.size() & 0xFF));
    out.insert(out.end(), name.begin(), name.end());
    const auto size = static_cast<std::uint64_t>(data.size());
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    out.push_back(codec);
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(compress_ms >> (8 * i)));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const char* kColourName[8] = {"black", "blue", "green", "cyan", "red", "magenta", "yellow", "white"};

}  // namespace

int MakeFrame(int argc, char** argv) {
    const char* out_path = argv[2];
    const int colours = std::atoi(argv[3]);
    const auto seq = static_cast<std::uint32_t>(std::atol(argv[4]));
    const int chroma_blur = argc > 5 ? std::atoi(argv[5]) : 0;
    const int width = argc > 6 ? std::atoi(argv[6]) : 800;
    const int height = argc > 7 ? std::atoi(argv[7]) : 1300;
    const GridGeometry g{120, 260};
    const FrameLayout layout = FrameLayout::Create(g, LayoutConfig{}).value();
    std::vector<std::uint8_t> file(600 * 1024);
    JavaRandom(5).NextBytes(&file);
    const std::string name = "fileflow-test.bin";
    const std::vector<std::uint8_t> env = Envelope(file, name);
    TransmitConfig tc;
    tc.profile = colours == 8   ? ModulationProfile::kM5EightColor
                 : colours == 4 ? ModulationProfile::kM3FourColor
                                : ModulationProfile::kM0BinaryLuminance;
    auto tx = TransmitSession::Create(layout, env, name, SessionIdFor(env, name), tc).value();
    CellMatrix cells(g.cols, g.rows);
    for (std::uint32_t n = 0; n <= seq; ++n) (void)tx.Next(&cells);
    sim::OpticalRenderConfig rc;
    rc.view.image_width = width;
    rc.view.image_height = height;
    rc.blur_radius = 1;
    rc.noise_amplitude = 3.0;
    sim::ColorChannelConfig cam;
    cam.chroma_blur = chroma_blur;
    const sim::YuvImage img = sim::RenderColorView(cells, g, rc, cam, nullptr);
    std::ofstream out(out_path, std::ios::binary);
    for (const Image8* plane : {&img.y, &img.u, &img.v}) {
        for (int yy = 0; yy < plane->height(); ++yy) {
            for (int xx = 0; xx < plane->width(); ++xx) {
                const auto px = static_cast<char>(plane->at(xx, yy));
                out.write(&px, 1);
            }
        }
    }
    std::printf("wrote %s: %dx%d, %d colours, sequence %u\n", out_path, width, height, colours, seq);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--make") == 0) return MakeFrame(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: ffframe FRAME.yuv WIDTH HEIGHT [--grid CxR] [--test-bytes N]\n");
        return 2;
    }
    const int w = std::atoi(argv[2]);
    const int h = std::atoi(argv[3]);
    std::uint32_t cols = 120;
    std::uint32_t rows = 260;
    std::size_t test_bytes = 600 * 1024;
    std::string file_path;
    std::string file_name;
    long compress_ms = -1;
    int settle = 40;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--grid") == 0) {
            std::sscanf(argv[i + 1], "%ux%u", &cols, &rows);
        } else if (std::strcmp(argv[i], "--test-bytes") == 0) {
            test_bytes = static_cast<std::size_t>(std::atol(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--file") == 0) {
            file_path = argv[i + 1];
        } else if (std::strcmp(argv[i], "--name") == 0) {
            file_name = argv[i + 1];
        } else if (std::strcmp(argv[i], "--compress-ms") == 0) {
            compress_ms = std::atol(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--settle") == 0) {
            settle = std::atoi(argv[i + 1]);
        }
    }

    // ---- the frame ----
    std::ifstream in(argv[1], std::ios::binary);
    const auto cw = static_cast<std::size_t>(w / 2);
    const auto ch = static_cast<std::size_t>(h / 2);
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) + 2 * cw * ch);
    if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
        std::fprintf(stderr, "could not read %zu bytes from %s\n", raw.size(), argv[1]);
        return 1;
    }
    const std::uint8_t* yp = raw.data();
    const std::uint8_t* up = yp + static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const std::uint8_t* vp = up + cw * ch;
    const ImageView8 y(yp, w, h, w);
    const ChromaPlanes chroma{ImageView8(up, static_cast<int>(cw), static_cast<int>(ch), static_cast<std::ptrdiff_t>(cw)),
                              ImageView8(vp, static_cast<int>(cw), static_cast<int>(ch), static_cast<std::ptrdiff_t>(cw))};

    const GridGeometry g{cols, rows};
    const FrameLayout layout = FrameLayout::Create(g, LayoutConfig{}).value();
    auto pipeline = FramePipeline::Create(layout).value();
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    // Settled first (--settle), then timed twice: luma only, then with colour -- each tracking,
    // which is what a phone does on all but its first frame.
    std::uint64_t n = 0;
    for (int i = 0; i < std::max(1, settle); ++i, ++n) {
        (void)pipeline.Process(y, n, static_cast<std::int64_t>(n + 1) * 33'333'333, &chroma);
    }
    const auto t0 = Clock::now();
    (void)pipeline.Process(y, n, static_cast<std::int64_t>(n + 1) * 33'333'333, nullptr);
    ++n;
    const auto t1 = Clock::now();
    const CapturedFrame cf = pipeline.Process(y, n, static_cast<std::int64_t>(n + 1) * 33'333'333, &chroma);
    const auto t2 = Clock::now();
    std::printf("timing (this host): pipeline %.1f ms luma only, %.1f ms with colour\n", ms(t0, t1),
                ms(t1, t2));
    std::printf("lens: learned distortion k1 %.4f after %d frames\n", pipeline.lens_strength(), settle);
    if (!pipeline.last_ok()) {
        std::printf("screen not found / not readable in this frame\n");
        return 1;
    }

    // ---- its header ----
    const M0Modulator m0(layout);
    const HeaderCodec hc;
    const PhotometricRef ref = m0.EstimateReference(cf.cell_samples);
    std::vector<std::size_t> erased;
    auto hb = m0.DemodulateHeader(cf.cell_samples, ref, hc.coded_size(), &erased);
    if (!hb.ok()) {
        std::printf("header did not demodulate\n");
        return 1;
    }
    std::vector<std::uint8_t> hbuf = hb.value();
    auto hr = hc.Decode(hbuf, erased);
    if (!hr.ok()) {
        std::printf("header did not decode\n");
        return 1;
    }
    const FrameHeader hd = hr.value();
    std::printf("header: session %08x sequence %u profile %u nsym %u%s\n", hd.session_id, hd.sequence,
                static_cast<unsigned>(hd.profile), hd.fec_nsym, hd.is_manifest() ? " (manifest)" : "");

    // ---- whether it decodes: any frame, no truth needed ----
    ReceiveSession rs = ReceiveSession::Create(layout).value();
    const auto t3 = Clock::now();
    const FrameOutcome outcome = rs.Ingest(cf.cell_samples, cf.cell_rgb, 1);
    std::printf("timing (this host): session %.1f ms (header, demodulation, FEC)\n",
                ms(t3, Clock::now()));
    const IntraFec::Stats fs = rs.last_fec_stats();
    std::printf("decode: %s; FEC worst codeword used %zu of %u parity bytes\n",
                std::string(FrameOutcomeName(outcome)).c_str(), fs.worst_budget_used, hd.fec_nsym);
    const ColorModulator mod(layout, hd.profile);
    const std::size_t bits = mod.bits_per_cell();
    SoftSymbolBuffer soft;
    ColorDiagnostics diag;
    mod.DemodulatePayload(cf.cell_rgb, &soft, &diag);
    std::printf("colours %.0f apart, noise %.1f (after bleed), %zu cells sampled\n",
                diag.centroid_spread, diag.noise_sigma, diag.cells);
    std::printf("learned:");
    for (std::size_t c = 0; c < (bits == 2 ? 4U : 8U); ++c) {
        std::printf(" [%.0f %.0f %.0f]", diag.centroids[c][0], diag.centroids[c][1], diag.centroids[c][2]);
    }
    std::printf("\n");

    // ---- what the sender drew ----
    std::vector<std::uint8_t> file;
    std::string name;
    if (file_path.empty()) {
        file.resize(test_bytes);
        JavaRandom(5).NextBytes(&file);
        name = "fileflow-test.bin";
    } else {
        std::ifstream fin(file_path, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(fin), std::istreambuf_iterator<char>());
        name = file_name.empty() ? file_path.substr(file_path.find_last_of('/') + 1) : file_name;
    }
    // The envelope: version 1 (older senders), or version 2 as sent as-is or zstd-compressed,
    // over every compress time it could have recorded.
    std::vector<std::uint8_t> env = Envelope(file, name);
    std::uint32_t sid = SessionIdFor(env, name);
    std::uint8_t env_codec = 0;
    long env_ms = 0;
    if (sid != hd.session_id) {
        std::vector<std::uint8_t> zstd;
        if (auto z = Compress(file, {.level = 19, .long_window = file.size() > 8U * 1024 * 1024}); z.ok()) {
            zstd = std::move(z).value();
        }
        const long lo = compress_ms >= 0 ? compress_ms : 0;
        const long hi = compress_ms >= 0 ? compress_ms : 3000;
        for (std::uint8_t codec : {std::uint8_t{0}, std::uint8_t{2}}) {
            if (codec == 2 && zstd.empty()) continue;
            const std::vector<std::uint8_t>& body = codec == 2 ? zstd : file;
            for (long t = lo; t <= hi && sid != hd.session_id; ++t) {
                env = Envelope2(file, name, body, codec, static_cast<std::uint32_t>(t));
                sid = SessionIdFor(env, name);
                env_codec = codec;
                env_ms = t;
            }
            if (sid == hd.session_id) break;
        }
    }
    if (sid != hd.session_id) {
        std::printf("session %08x is not %s's (as sent): cannot regenerate truth\n", hd.session_id,
                    name.c_str());
        return 1;
    }
    std::printf("truth: %s, %zu bytes, envelope codec %u, compress %ld ms\n", name.c_str(), file.size(),
                static_cast<unsigned>(env_codec), env_ms);
    TransmitConfig tc;
    tc.fec_nsym = hd.fec_nsym;
    tc.profile = hd.profile;
    if (env.size() > 4 && env[3] == '2') {
        tc.content = ContentHint{.original_size = file.size(), .codec = env_codec,
                                 .compress_ms = static_cast<std::uint32_t>(env_ms)};
    }
    auto tx = TransmitSession::Create(layout, env, name, sid, tc).value();
    CellMatrix truth(cols, rows);
    bool found = false;
    for (std::uint32_t k = 0; k <= hd.sequence + 1 && k < 1000000; ++k) {
        auto r = tx.Next(&truth);
        if (r.ok() && r.value().sequence == hd.sequence) {
            found = true;
            break;
        }
    }
    if (!found) {
        std::printf("sequence %u never produced\n", hd.sequence);
        return 1;
    }

    // ---- compare ----

    const auto& pay = layout.payload_cells();
    std::array<std::array<std::size_t, 8>, 8> confusion{};
    std::array<std::array<double, 3>, 8> sum{};
    std::array<std::array<double, 3>, 8> sum2{};
    std::array<std::size_t, 8> count{};
    std::array<std::size_t, 3> bit_err{};
    constexpr int kBands = 6;
    std::array<std::array<std::size_t, 3>, kBands> band_err{};  // [row band][col third]
    std::array<std::array<std::size_t, 3>, kBands> band_n{};
    std::array<std::size_t, 5> by_diff_err{};  // by how many of 4 edge neighbours differ
    std::array<std::size_t, 5> by_diff_n{};
    std::size_t cells_wrong = 0;
    std::size_t cells_seen = 0;
    for (std::size_t i = 0; i < pay.size(); ++i) {
        const std::uint32_t idx = pay[i];
        const unsigned true_v = truth.at(idx % cols, idx / cols) & 7U;
        bool erased_cell = false;
        unsigned got = 0;
        for (std::size_t j = 0; j < bits; ++j) {
            const SoftSymbol& sy = soft.symbols[i * bits + j];
            if (sy.erased) erased_cell = true;
            got = (got << 1U) | (sy.llr < 0 ? 1U : 0U);
        }
        if (erased_cell) continue;
        // M3 decodes to a palette index; map it back to the colour it names.
        static const unsigned kFour[4] = {0b000, 0b011, 0b101, 0b110};
        const unsigned got_v = bits == 2 ? kFour[got & 3U] : got;
        ++cells_seen;
        ++confusion[true_v][got_v];
        for (std::size_t k = 0; k < 3; ++k) {
            const double v = cf.cell_rgb[static_cast<std::size_t>(idx) * 3 + k];
            sum[true_v][k] += v;
            sum2[true_v][k] += v * v;
        }
        ++count[true_v];
        const bool wrong = got_v != true_v;
        cells_wrong += wrong ? 1 : 0;
        for (std::size_t k = 0; k < 3; ++k) {
            bit_err[k] += (((got_v ^ true_v) >> (2 - k)) & 1U);
        }
        const std::uint32_t c = idx % cols;
        const std::uint32_t r = idx / cols;
        const auto band = std::min<std::size_t>(kBands - 1, static_cast<std::size_t>(r) * kBands / rows);
        const auto third = std::min<std::size_t>(2, static_cast<std::size_t>(c) * 3 / cols);
        ++band_n[band][third];
        band_err[band][third] += wrong ? 1 : 0;
        int differ = 0;
        auto cmp = [&](std::uint32_t cc, std::uint32_t rr) {
            if ((truth.at(cc, rr) & 7U) != true_v) ++differ;
        };
        if (c > 0) cmp(c - 1, r);
        if (c + 1 < cols) cmp(c + 1, r);
        if (r > 0) cmp(c, r - 1);
        if (r + 1 < rows) cmp(c, r + 1);
        ++by_diff_n[static_cast<std::size_t>(differ)];
        by_diff_err[static_cast<std::size_t>(differ)] += wrong ? 1 : 0;
    }

    std::printf("cells wrong: %zu of %zu (%.2f%%); bit errors red %zu green %zu blue %zu\n",
                cells_wrong, cells_seen, 100.0 * static_cast<double>(cells_wrong) / static_cast<double>(cells_seen),
                bit_err[0], bit_err[1], bit_err[2]);
    std::printf("\nper colour: measured mean r,g,b (sd) and where it was decoded to\n");
    for (unsigned t = 0; t < 8; ++t) {
        if (count[t] == 0) continue;
        std::printf("  %-8s n=%5zu  ", kColourName[t], count[t]);
        for (std::size_t k = 0; k < 3; ++k) {
            const double m = sum[t][k] / static_cast<double>(count[t]);
            const double sd = std::sqrt(std::max(0.0, sum2[t][k] / static_cast<double>(count[t]) - m * m));
            std::printf("%4.0f(%3.0f) ", m, sd);
        }
        std::printf("  ->");
        for (unsigned d = 0; d < 8; ++d) {
            if (confusion[t][d] != 0 && d != t) std::printf(" %s:%zu", kColourName[d], confusion[t][d]);
        }
        std::printf("\n");
    }
    std::printf("\nerror rate by position (rows top to bottom; left | middle | right third)\n");
    for (int b = 0; b < kBands; ++b) {
        std::printf("  ");
        for (int t = 0; t < 3; ++t) {
            const auto bb = static_cast<std::size_t>(b);
            const auto tt = static_cast<std::size_t>(t);
            std::printf("%6.2f%% ", band_n[bb][tt] ? 100.0 * static_cast<double>(band_err[bb][tt]) /
                                                         static_cast<double>(band_n[bb][tt])
                                                   : 0.0);
        }
        std::printf("\n");
    }
    std::printf("\nerror rate by how many of the 4 edge neighbours differ in colour\n  ");
    for (std::size_t d = 0; d <= 4; ++d) {
        std::printf("%zu: %.2f%% (n=%zu)   ", d,
                    by_diff_n[d] ? 100.0 * static_cast<double>(by_diff_err[d]) / static_cast<double>(by_diff_n[d]) : 0.0,
                    by_diff_n[d]);
    }
    std::printf("\n");
    return 0;
}
