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
};

// Sends `file` until it verifies, through the simulated camera, and counts the display states it
// took. The receiver is told nothing about the profile: it reads it from each header.
Outcome Transfer(const std::vector<std::uint8_t>& file, ModulationProfile profile,
                 const sim::ColorChannelConfig& camera, int max_frames = 400) {
    const FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    TransmitConfig tc;
    tc.profile = profile;
    tc.manifest_interval = 8;
    auto tx = TransmitSession::Create(layout, file, "photo.jpg", 0xC010, tc);
    EXPECT_TRUE(tx.ok());
    auto rx = LiveReceiver::Create(kGrid);
    EXPECT_TRUE(rx.ok());

    Outcome out;
    out.symbol_bytes = tx.value().transmitter().manifest().symbol_size;
    CellMatrix frame(kGrid.cols, kGrid.rows);
    for (int n = 0; n < max_frames; ++n) {
        EXPECT_TRUE(tx.value().Next(&frame).ok());
        Homography truth;
        const sim::YuvImage img = sim::RenderColorView(frame, kGrid, Optics(static_cast<std::uint64_t>(n) + 1U), camera, &truth);
        const ChromaPlanes chroma{img.u.view(), img.v.view()};
        const LiveStatus s =
            rx.value().Ingest(img.y.view(), static_cast<std::int64_t>(n) * 33'333'333 + 1, 0, &chroma);
        if (s.phase == LivePhase::kVerified) {
            out.verified = rx.value().File().value() == file;
            out.frames = n + 1;
            break;
        }
    }
    out.color = rx.value().session().last_color();
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
    struct Arm { const char* name; int blur; double noise; double cross; };
    const std::vector<std::uint8_t> file = File(60000);
    for (const Arm a : {Arm{"clean", 1, 3.0, 1.0}, Arm{"blur 2", 2, 3.0, 1.0},
                        Arm{"blur 3", 3, 3.0, 1.0}, Arm{"crosstalk x3", 1, 3.0, 3.0},
                        Arm{"crosstalk x2", 1, 3.0, 2.0}, Arm{"blur 2 noise 8 x2", 2, 8.0, 2.0}}) {
        sim::ColorChannelConfig cam;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                if (i != j) cam.mix[i][j] *= a.cross;
            }
        }
        g_blur = a.blur;
        g_noise = a.noise;
        const Outcome c = Transfer(file, ModulationProfile::kM5EightColor, cam, 300);
        const Outcome m = Transfer(file, ModulationProfile::kM0BinaryLuminance, cam, 300);
        std::printf("   %-18s M0 %3d frames%s   M5 %3d frames%s   spread %.0f noise %.1f\n", a.name,
                    m.frames, m.verified ? "" : " (failed)", c.frames, c.verified ? "" : " (failed)",
                    c.color.centroid_spread, c.color.noise_sigma);
    }
    g_blur = 1;
    g_noise = 3.0;
}

}  // namespace fileflow
