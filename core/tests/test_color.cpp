// M5: eight colours, three bits a cell (F62).
#include <fileflow/live.h>
#include <fileflow/modulation.h>
#include <fileflow/pipeline.h>
#include <fileflow/session.h>
#include <fileflow/sim/render.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

namespace fileflow {
namespace {

constexpr GridGeometry kGrid{120, 200};
int g_blur = 1;          // the margin table varies these
double g_noise = 3.0;
double g_lens_k1 = 0.0;  // the lens test varies these
double g_distance = 1.0;

std::vector<std::uint8_t> File(std::size_t n) {
    std::vector<std::uint8_t> f(n);
    std::uint32_t x = 0x9E3779B9U;
    for (auto& b : f) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        b = static_cast<std::uint8_t>(x);
    }
    return f;
}

sim::OpticalRenderConfig Optics(std::uint64_t seed) {
    sim::OpticalRenderConfig rc;
    rc.view.image_width = 800;
    rc.view.image_height = 1300;
    rc.view.distance = g_distance;
    rc.lens_k1 = g_lens_k1;
    rc.blur_radius = g_blur;
    rc.noise_amplitude = g_noise;
    rc.seed = seed;
    return rc;
}

struct Outcome {
    bool verified = false;
    int frames = 0;
    std::size_t symbol_bytes = 0;
    ColorDiagnostics color;
    ReceiveStats stats;
    double lens = 0.0;  // the distortion the receiver learned
};

// Sends `file` until it verifies, through the simulated camera, and counts the display states it
// took. The receiver is told nothing about the profile: it reads it from each header. Each state
// is photographed `captures` times, each with its own noise -- a camera at twice the state rate.
Outcome Transfer(const std::vector<std::uint8_t>& file, ModulationProfile profile,
                 const sim::ColorChannelConfig& camera, int max_frames = 400, int captures = 1,
                 bool fit_lens = true) {
    const FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    TransmitConfig tc;
    tc.profile = profile;
    tc.manifest_interval = 8;
    auto tx = TransmitSession::Create(layout, file, "photo.jpg", 0xC010, tc);
    EXPECT_TRUE(tx.ok());
    LiveConfig lc;
    lc.pipeline.subcell.fit_lens = fit_lens;
    auto rx = LiveReceiver::Create(kGrid, lc);
    EXPECT_TRUE(rx.ok());

    Outcome out;
    out.symbol_bytes = tx.value().transmitter().manifest().symbol_size;
    CellMatrix frame(kGrid.cols, kGrid.rows);
    for (int n = 0; n < max_frames && !out.verified; ++n) {
        EXPECT_TRUE(tx.value().Next(&frame).ok());
        for (int c = 0; c < captures; ++c) {
            Homography truth;
            // Each capture its own noise: the seed drives the sensor noise too.
            const auto seed = static_cast<std::uint64_t>(n * captures + c) + 1U;
            const sim::YuvImage img = sim::RenderColorView(frame, kGrid, Optics(seed), camera, &truth);
            const ChromaPlanes chroma{img.u.view(), img.v.view()};
            const auto t = static_cast<std::int64_t>(n) * 33'333'333 + c * 16'666'666 + 1;
            const LiveStatus s = rx.value().Ingest(img.y.view(), t, 0, &chroma);
            if (s.phase == LivePhase::kVerified) {
                out.verified = rx.value().File().value() == file;
                out.frames = n + 1;
                break;
            }
        }
    }
    out.color = rx.value().session().last_color();
    out.stats = rx.value().session().stats();
    out.lens = rx.value().pipeline().lens_strength();
    return out;
}

}  // namespace

TEST(ColorModulator, RenderedColoursDecodeBackToTheBits) {
    // No optics at all: render, read each cell's colour straight off the matrix, demodulate.
    const FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    const ColorModulator mod(layout);
    const std::vector<std::uint8_t> payload = File(mod.payload_capacity_bytes());
    CellMatrix cells(kGrid.cols, kGrid.rows);
    ASSERT_TRUE(mod.Render({}, payload, &cells).ok());

    std::vector<double> rgb(static_cast<std::size_t>(kGrid.cells()) * 3);
    for (std::uint32_t i = 0; i < kGrid.cells(); ++i) {
        for (int k = 0; k < 3; ++k) {
            rgb[i * 3 + static_cast<std::size_t>(k)] =
                ColorChannelOn(cells.at(i % kGrid.cols, i / kGrid.cols), k) ? 255.0 : 0.0;
        }
    }
    SoftSymbolBuffer soft;
    mod.DemodulatePayload(rgb, &soft);
    const HardDecision hd = HardDecide(soft, payload.size());
    EXPECT_EQ(hd.erasures, 0U);
    EXPECT_EQ(hd.bytes, payload);
}

TEST(ColorModulator, ThreeTimesTheBitsOfM0) {
    const FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    EXPECT_EQ(PayloadCapacityBytes(layout, ModulationProfile::kM5EightColor),
              layout.payload_cells().size() * 3 / 8);
    EXPECT_EQ(PayloadCapacityBytes(layout, ModulationProfile::kM0BinaryLuminance),
              layout.payload_cells().size() / 8);
}

TEST(ColorTransfer, EightColoursThroughACrosstalkingCameraVerify) {
    // The default camera model has 15-20% crosstalk between channels and a tinted white balance,
    // and the chroma planes are a quarter the resolution of the luma -- the receiver has to learn
    // where the eight colours land, every frame.
    const std::vector<std::uint8_t> file = File(60000);
    const sim::ColorChannelConfig camera;
    const Outcome color = Transfer(file, ModulationProfile::kM5EightColor, camera);
    const Outcome mono = Transfer(file, ModulationProfile::kM0BinaryLuminance, camera);
    ASSERT_TRUE(color.verified) << "M5 did not complete in 400 frames";
    ASSERT_TRUE(mono.verified);
    std::printf("   60 KB: M0 %d frames (%zu-byte symbols), M5 %d frames (%zu-byte symbols); "
                "colours %.0f apart, noise %.1f\n",
                mono.frames, mono.symbol_bytes, color.frames, color.symbol_bytes,
                color.color.centroid_spread, color.color.noise_sigma);
    EXPECT_GT(color.symbol_bytes, mono.symbol_bytes * 5 / 2) << "three bits a cell, less header";
    EXPECT_LT(color.frames * 2, mono.frames) << "the same file in well under half the frames";
}

TEST(ColorTransfer, ColourThatBleedsBetweenCellsStillVerifies) {
    // What the Pixel 8 reported reading the S26 Ultra: the eight colours ~165 apart where the S26
    // reading the Pixel saw ~190, and noise ~38 against ~30 -- and almost no frame decoded (F65).
    // The cause modelled here is colour resolution: chroma blurred past 4:2:0, so each cell
    // carries its neighbours' colour, with crosstalk, sensor noise and colour shading on top.
    // Read as noise, this failed every frame; learned and subtracted, it verifies.
    sim::ColorChannelConfig camera;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            if (i != j) camera.mix[i][j] *= 2.0;
        }
    }
    camera.sensor_noise = 30.0;
    camera.shading = {0.15, -0.045, -0.15};
    camera.chroma_blur = 1;
    g_blur = 2;
    const Outcome o = Transfer(File(60000), ModulationProfile::kM5EightColor, camera, 200);
    g_blur = 1;
    EXPECT_TRUE(o.verified) << "colours " << o.color.centroid_spread << " apart, noise "
                            << o.color.noise_sigma;
    EXPECT_LT(o.frames, 20) << "and in about the frames a clean camera needs";
}

TEST(ColorTransfer, FourColoursSurviveBlurThatDefeatsEight) {
    // Colour blurred across a whole cell (chroma blur 3 here, with cells three chroma pixels wide):
    // eight colours cannot be told apart, four -- each pair two channels apart -- still can (F66).
    // This is what the Fast setting is for. (Blur 2 was the case until the demodulator dropped its
    // covariance weighting; eight colours read that now, F67.)
    sim::ColorChannelConfig camera;
    camera.chroma_blur = 3;
    const std::vector<std::uint8_t> file = File(60000);
    const Outcome four = Transfer(file, ModulationProfile::kM3FourColor, camera, 120);
    const Outcome eight = Transfer(file, ModulationProfile::kM5EightColor, camera, 40);
    EXPECT_TRUE(four.verified) << "four colours should survive this blur";
    EXPECT_FALSE(eight.verified) << "if eight now survive it too, this test needs a harder case";
    EXPECT_LT(four.frames, 31) << "and still beat black and white's 31 frames";
}

TEST(ColorTransfer, AnM0ReceiverWithoutColourRefusesM5FramesRatherThanMisreadingThem) {
    // A receiver given no chroma must call an M5 frame unsupported, never decode it as M0.
    const FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    TransmitConfig tc;
    tc.profile = ModulationProfile::kM5EightColor;
    auto tx = TransmitSession::Create(layout, File(20000), "a.bin", 7, tc);
    ASSERT_TRUE(tx.ok());
    auto pipeline = FramePipeline::Create(layout);
    auto rx = ReceiveSession::Create(layout);
    ASSERT_TRUE(pipeline.ok() && rx.ok());
    CellMatrix frame(kGrid.cols, kGrid.rows);
    int unsupported = 0;
    for (int n = 0; n < 10; ++n) {
        ASSERT_TRUE(tx.value().Next(&frame).ok());
        const sim::YuvImage img =
            sim::RenderColorView(frame, kGrid, Optics(static_cast<std::uint64_t>(n) + 1U), sim::ColorChannelConfig{}, nullptr);
        const CapturedFrame cf = pipeline.value().Process(img.y.view(), static_cast<std::uint64_t>(n), n + 1);
        const FrameOutcome o = rx.value().Ingest(cf.cell_samples, n + 1);
        EXPECT_NE(o, FrameOutcome::kSymbol);
        EXPECT_NE(o, FrameOutcome::kManifest);
        unsupported += o == FrameOutcome::kPayloadUnsupported ? 1 : 0;
    }
    EXPECT_GT(unsupported, 5) << "headers should read; payloads should be refused";
}

}  // namespace fileflow

namespace fileflow {

// Not an assertion: where M5 stops working, against M0, as blur, noise and crosstalk grow. Run it
// with --gtest_also_run_disabled_tests when the colour demodulator changes.
TEST(ColorTransfer, DISABLED_MarginTable) {
    struct Arm {
        const char* name;
        int blur;
        double cross;
        double sensor;   // per-pixel sensor noise after the filters, peak levels
        double shading;  // colour shading, +/- across the picture (red up, blue down)
        int chroma_blur = 0;  // chroma pixels
    };
    const std::vector<std::uint8_t> file = File(60000);
    for (const Arm a : {Arm{"clean", 1, 1.0, 0.0, 0.0}, Arm{"blur 2", 2, 1.0, 0.0, 0.0},
                        Arm{"crosstalk x3", 1, 3.0, 0.0, 0.0},
                        Arm{"sensor noise 30", 1, 1.0, 30.0, 0.0},
                        Arm{"sensor noise 50", 1, 1.0, 50.0, 0.0},
                        Arm{"shading 15%", 1, 1.0, 0.0, 0.15},
                        Arm{"shading 25%", 1, 1.0, 0.0, 0.25},
                        // What the Pixel 8 reported reading the S26: centroids ~165 apart, noise ~38.
                        Arm{"chroma blur 1", 1, 1.0, 0.0, 0.0, 1},
                        Arm{"chroma blur 2", 1, 1.0, 0.0, 0.0, 2},
                        Arm{"pixel-like", 2, 2.0, 30.0, 0.15, 1},
                        Arm{"pixel-like, worse", 2, 2.0, 30.0, 0.15, 2},
                        Arm{"chroma blur 3", 1, 1.0, 0.0, 0.0, 3}}) {
        sim::ColorChannelConfig cam;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                if (i != j) cam.mix[i][j] *= a.cross;
            }
        }
        cam.sensor_noise = a.sensor;
        cam.shading = {a.shading, -0.3 * a.shading, -a.shading};
        cam.chroma_blur = a.chroma_blur;
        g_blur = a.blur;
        const Outcome c = Transfer(file, ModulationProfile::kM5EightColor, cam, 300);
        const Outcome m = Transfer(file, ModulationProfile::kM0BinaryLuminance, cam, 300);
        const Outcome f = Transfer(file, ModulationProfile::kM3FourColor, cam, 300);
        std::printf("   %-18s M0 %3d%s   M3 %3d%s   M5 %3d%s frames   M5 spread %.0f noise %.1f\n",
                    a.name, m.frames, m.verified ? "" : "(failed)", f.frames,
                    f.verified ? "" : "(failed)", c.frames, c.verified ? "" : "(failed)",
                    c.color.centroid_spread, c.color.noise_sigma);
    }
    g_blur = 1;
}

}  // namespace fileflow

namespace fileflow {

TEST(ColorTransfer, TwoCapturesOfAStateDecodeWhereOneAloneCannot) {
    // F68. A camera at twice the state rate photographs each state twice. On a channel where a
    // single capture almost never passes FEC -- eight colours with heavy crosstalk, bleed and
    // sensor noise -- the two captures' soft decisions, summed, decode most states.
    sim::ColorChannelConfig camera;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            if (i != j) camera.mix[i][j] *= 2.0;
        }
    }
    camera.sensor_noise = 95.0;
    camera.shading = {0.15, -0.045, -0.15};
    camera.chroma_blur = 1;
    g_blur = 2;
    const Outcome one = Transfer(File(60000), ModulationProfile::kM5EightColor, camera, 60, 1);
    const Outcome two = Transfer(File(60000), ModulationProfile::kM5EightColor, camera, 60, 2);
    g_blur = 1;
    EXPECT_FALSE(one.verified) << "one capture a state should not be enough here";
    EXPECT_TRUE(two.verified) << "two captures, combined, should be";
    EXPECT_LT(two.frames, 20);
    EXPECT_GT(two.stats.fec_ok_combined, two.stats.fec_ok / 2) << "and mostly because of combining";
}

TEST(ColorTransfer, LensDistortionIsLearnedNotTrusted) {
    // F72. A Pixel 8 held close to the sender read every header and decoded nothing: its lens bowed
    // the screen by a cell or more between the corners a homography pins, though the camera
    // reported its own distortion correction on. The receiver now learns the bow from the frames.
    // Here: barrel distortion of the strength measured on that phone, the screen filling the view.
    g_lens_k1 = -0.045;
    g_distance = 0.8;
    const std::vector<std::uint8_t> file = File(60000);
    const Outcome learned = Transfer(file, ModulationProfile::kM5EightColor, {}, 120, 1, true);
    const Outcome trusting = Transfer(file, ModulationProfile::kM5EightColor, {}, 120, 1, false);
    g_lens_k1 = 0.0;
    g_distance = 1.0;
    EXPECT_TRUE(learned.verified) << "learned k1 " << learned.lens;
    EXPECT_NEAR(learned.lens, -0.045, 0.015);
    EXPECT_TRUE(!trusting.verified || trusting.frames > 2 * learned.frames)
        << "without the lens model: " << trusting.frames << " frames, with: " << learned.frames;
}

TEST(ColorTransfer, AnUndistortedLensIsLeftAlone) {
    // The lens is learned only while the screen reaches far enough out for it to matter; an ideal
    // lens must come out at (about) zero, never pushed somewhere by noise.
    g_distance = 0.8;
    const Outcome o = Transfer(File(60000), ModulationProfile::kM5EightColor, {}, 120, 1, true);
    g_distance = 1.0;
    EXPECT_TRUE(o.verified);
    EXPECT_NEAR(o.lens, 0.0, 0.01);
}

}  // namespace fileflow
