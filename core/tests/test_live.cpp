// Live receiver (C13b): a handheld phone, and what the person holding it is told.
#include <fileflow/live.h>
#include <fileflow/session.h>
#include <fileflow/sim/render.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace fileflow;

namespace {

constexpr GridGeometry kGrid{120, 200};
constexpr double kFps = 30.0;

std::vector<std::uint8_t> File(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(4242);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

// A transmitter and a camera on a moving hand. Each Capture() advances the transmitter one state
// and renders what the camera saw over that frame's exposure.
struct Rig {
    FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    std::vector<std::uint8_t> file;
    std::optional<TransmitSession> tx;
    sim::OpticalRenderConfig rc;
    sim::MotionConfig motion;
    CellMatrix frame{kGrid.cols, kGrid.rows};
    int n = 0;

    explicit Rig(std::size_t bytes) : file(File(bytes)) {
        TransmitConfig tc;
        tc.manifest_interval = 8;  // short transfers: a late joiner should not wait long
        tx.emplace(TransmitSession::Create(layout, file, "photo.jpg", 0x11FE, tc).value());
        rc.view.image_width = 800;
        rc.view.image_height = 1300;
        rc.blur_radius = 1;
        rc.noise_amplitude = 3.0;
    }

    double t() const { return static_cast<double>(n) / kFps; }
    std::int64_t ts() const { return static_cast<std::int64_t>(n) * 33'333'333 + 1; }

    Image8 Capture() {
        EXPECT_TRUE(tx->Next(&frame).ok());
        rc.seed = static_cast<std::uint64_t>(n) + 1;
        Homography truth;
        Image8 img = motion.active() ? sim::RenderMoving(frame, kGrid, rc, motion, t(), &truth)
                                     : sim::RenderView(frame, kGrid, rc, &truth);
        ++n;
        return img;
    }
};

}  // namespace

// Not an assertion: the evidence behind LiveConfig's "hold steadier" threshold. For each kind of
// motion, the motion blur the receiver estimates and the share of frames it actually read -- the
// hint should fire where reading suffers, and not before. Disabled because it renders ~300
// handheld frames (~8 s); run it with --gtest_also_run_disabled_tests when the threshold or the
// motion model changes.
TEST(LiveReceiver, DISABLED_BlurCalibrationTable) {
    struct Arm { const char* name; double shake; double pan; };
    for (const Arm arm : {Arm{"shake 1", 1.0, 0.0}, Arm{"shake 2", 2.0, 0.0},
                          Arm{"shake 3", 3.0, 0.0}, Arm{"shake 4", 4.0, 0.0},
                          Arm{"pan 0.3", 0.5, 0.3}, Arm{"pan 0.6", 0.5, 0.6},
                          Arm{"pan 1.0", 0.5, 1.0}}) {
        Rig rig(200000);
        rig.motion.shake = arm.shake;
        rig.motion.pan_x = arm.pan;
        auto rx = LiveReceiver::Create(kGrid);
        ASSERT_TRUE(rx.ok());
        double blur = 0.0, peak = 0.0;
        int n = 0;
        for (int i = 0; i < 45; ++i) {
            const Image8 img = rig.Capture();
            const LiveStatus& s = rx.value().Ingest(img.view(), rig.ts());
            if (i < 15) continue;  // let the velocity estimate settle
            blur += s.motion_blur_cells;
            peak = std::max(peak, s.motion_blur_cells);
            ++n;
        }
        const ReceiveStats& st = rx.value().session().stats();
        std::printf("   %-8s blur mean %.2f peak %.2f cells   header success %.2f\n", arm.name,
                    blur / n, peak, st.header_success());
    }
}

TEST(LiveReceiver, HandheldTransferCompletesAndSaysSo) {
    Rig rig(20000);
    rig.motion.shake = 1.0;
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());

    double last = 0.0;
    for (int i = 0; i < 120 && rx.value().status().phase != LivePhase::kVerified; ++i) {
        const Image8 img = rig.Capture();
        const LiveStatus& s = rx.value().Ingest(img.view(), rig.ts());
        ASSERT_GE(s.progress, last) << "progress went backwards at frame " << i;
        last = s.progress;
        ASSERT_NE(s.phase, LivePhase::kFailed) << s.message;
    }
    const LiveStatus& s = rx.value().status();
    ASSERT_EQ(s.phase, LivePhase::kVerified) << s.message;
    EXPECT_EQ(s.file_name, "photo.jpg");
    EXPECT_NE(s.message.find("verified"), std::string::npos) << s.message;
    auto out = rx.value().File();
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), rig.file);
}

TEST(LiveReceiver, LosingTheScreenPausesAndNothingIsLost) {
    // The person's arm drops for a second mid-transfer. They must be told the transfer is PAUSED,
    // not failed -- and when they aim again it must carry on from where it was.
    Rig rig(60000);
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());

    // Receive until roughly a third of the way.
    for (int i = 0; i < 200 && rx.value().status().progress < 0.3; ++i) {
        const Image8 img = rig.Capture();
        rx.value().Ingest(img.view(), rig.ts());
    }
    const double before = rx.value().status().progress;
    ASSERT_GT(before, 0.0);
    ASSERT_LT(before, 1.0);

    // Pointed at the floor: frames keep coming, the screen is gone.
    const Image8 floor(rig.rc.view.image_width, rig.rc.view.image_height, 20);
    for (int i = 0; i < 30; ++i) {
        (void)rig.tx->Next(&rig.frame);  // the transmitter does not wait
        ++rig.n;
        rx.value().Ingest(floor.view(), rig.ts());
    }
    const LiveStatus& paused = rx.value().status();
    EXPECT_EQ(paused.hint, LiveHint::kPointAtScreen);
    EXPECT_NE(paused.message.find("Paused at"), std::string::npos) << paused.message;
    EXPECT_NE(paused.message.find("Nothing received so far is lost"), std::string::npos);
    EXPECT_DOUBLE_EQ(paused.progress, before) << "progress must hold, not reset";
    EXPECT_LT(paused.seconds_remaining, 0.0) << "no time estimate while nothing arrives";

    // Back on target.
    for (int i = 0; i < 300 && rx.value().status().phase != LivePhase::kVerified; ++i) {
        const Image8 img = rig.Capture();
        rx.value().Ingest(img.view(), rig.ts());
    }
    ASSERT_EQ(rx.value().status().phase, LivePhase::kVerified) << rx.value().status().message;
    EXPECT_EQ(rx.value().File().value(), rig.file);
}

TEST(LiveReceiver, FastMotionAsksToHoldSteadier) {
    Rig rig(60000);
    rig.motion.shake = 0.5;
    rig.motion.pan_x = 1.0;  // sweeping a full image width per second: reading collapses (F54)
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());

    int steadier = 0;
    for (int i = 0; i < 40; ++i) {
        const Image8 img = rig.Capture();
        const LiveStatus& s = rx.value().Ingest(img.view(), rig.ts());
        if (s.hint == LiveHint::kHoldSteadier) {
            ++steadier;
            EXPECT_NE(s.message.find("Hold steadier"), std::string::npos);
        }
    }
    EXPECT_GT(steadier, 10) << "the person was never told why reading stalled";
}

TEST(LiveReceiver, AShakyHandThatStillReadsIsNotNagged) {
    // Shake 2 reads every header (F54). Telling this person to hold steadier would be wrong, and a
    // warning that fires while everything works is one people learn to ignore.
    Rig rig(60000);
    rig.motion.shake = 2.0;
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());
    for (int i = 0; i < 40; ++i) {
        const Image8 img = rig.Capture();
        const LiveStatus& s = rx.value().Ingest(img.view(), rig.ts());
        EXPECT_NE(s.hint, LiveHint::kHoldSteadier) << "frame " << i << ": " << s.message;
    }
}

TEST(LiveReceiver, AnUnreadableScreenSaysSoInsteadOfWaitingSilently) {
    // Screen in view and steady, but blurred past reading -- focus or distance. The person needs a
    // different instruction from "point at the screen", because they already are.
    Rig rig(20000);
    rig.rc.blur_radius = 5;
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());
    LiveStatus last;
    for (int i = 0; i < 40; ++i) {
        const Image8 img = rig.Capture();
        last = rx.value().Ingest(img.view(), rig.ts());
    }
    if (!last.screen_in_view) GTEST_SKIP() << "blur also defeated detection; nothing to assert";
    EXPECT_EQ(last.hint, LiveHint::kNotReadable) << last.message;
    EXPECT_EQ(rx.value().File().error(), Error::kFountainIncomplete);
}
