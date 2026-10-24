// Live receiver (C13b): a handheld phone, and what the person holding it is told.
#include <fileflow/live.h>
#include <fileflow/session.h>
#include <fileflow/sim/render.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <iterator>
#include <atomic>
#include <map>
#include <thread>
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

    explicit Rig(std::size_t bytes, std::uint32_t session = 0x11FE,
                 const char* name = "photo.jpg")
        : file(File(bytes)) {
        TransmitConfig tc;
        tc.manifest_interval = 8;  // short transfers: a late joiner should not wait long
        tx.emplace(TransmitSession::Create(layout, file, name, session, tc).value());
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

namespace {

// Display states generated on demand, in order, for RenderUnsynced. Keeps a short window: a
// capture only ever reaches back a state or two.
struct StateStream {
    TransmitSession* tx = nullptr;
    std::map<std::int64_t, CellMatrix> frames;
    std::int64_t next = 0;

    static const CellMatrix& At(void* self, std::int64_t k) {
        auto* s = static_cast<StateStream*>(self);
        while (s->next <= k) {
            CellMatrix m(kGrid.cols, kGrid.rows);
            EXPECT_TRUE(s->tx->Next(&m).ok());
            s->frames.emplace(s->next++, std::move(m));
        }
        while (!s->frames.empty() && s->frames.begin()->first < k - 4) {
            s->frames.erase(s->frames.begin());
        }
        return s->frames.at(k);
    }
};

// Big enough that the time is dominated by the steady rate, not by the first manifest.
constexpr std::size_t kSweepBytes = 200000;

struct TimingResult {
    double seconds = -1.0;   // simulated time to a verified file; < 0 = did not finish
    double torn = 0.0;       // share of captures that saw more than one display state
    double payload_ok = 0.0; // share of captures whose payload decoded
};

// One unsynchronised transfer: camera at `fc`, display at `fd`, the given shutter timing, the
// camera's phase against the display drawn from `seed`. The decoder is assumed to keep up.
TimingResult RunUnsynced(double fc, double fd, double exposure_ms, double readout_ms,
                         std::uint64_t seed, std::size_t bytes = kSweepBytes,
                         double limit_s = 60.0) {
    // Never exactly the nominal rate: a camera and a display share no clock, and at EXACTLY equal
    // or harmonic rates the tear would sit at the same row in every capture, forever -- which
    // flatters or damns a configuration depending only on the phase (the first sweep did both).
    fc *= 1.0013;
    const FrameLayout layout = FrameLayout::Create(kGrid, LayoutConfig{}).value();
    const std::vector<std::uint8_t> file = File(bytes);
    TransmitConfig tc;
    auto tx = TransmitSession::Create(layout, file, "f.bin", 0x5EED, tc).value();
    StateStream stream;
    stream.tx = &tx;
    sim::OpticalRenderConfig rc;
    rc.view.image_width = 800;
    rc.view.image_height = 1300;
    rc.blur_radius = 1;
    rc.noise_amplitude = 3.0;
    sim::CaptureTiming ct;
    ct.exposure_s = exposure_ms / 1000.0;
    ct.readout_s = readout_ms / 1000.0;
    LiveConfig lc;
    lc.exposure_s = ct.exposure_s;
    auto rx = LiveReceiver::Create(kGrid, lc).value();

    SplitMix64 rng(seed);
    const double phase = static_cast<double>(rng.Next() % 1000000) / 1e6 / fc;
    TimingResult r;
    int captures = 0, torn = 0;
    std::uint64_t payloads_before = 0;
    int payload_ok = 0;
    for (int i = 0; static_cast<double>(i) / fc < limit_s; ++i) {
        const double t = phase + static_cast<double>(i) / fc;
        rc.seed = seed * 7919 + static_cast<std::uint64_t>(i);
        int hit = 0;
        Homography truth;
        const Image8 img = sim::RenderUnsynced(&StateStream::At, &stream, fd, kGrid, rc, ct, t,
                                               &truth, &hit);
        ++captures;
        if (hit > 1) ++torn;
        const LiveStatus& s = rx.Ingest(img.view(), static_cast<std::int64_t>(t * 1e9) + 1);
        if (s.payload_frames > payloads_before) ++payload_ok;
        payloads_before = s.payload_frames;
        if (s.phase == LivePhase::kVerified) {
            r.seconds = static_cast<double>(i + 1) / fc;
            break;
        }
    }
    r.torn = static_cast<double>(torn) / captures;
    r.payload_ok = static_cast<double>(payload_ok) / captures;
    return r;
}

}  // namespace

// Not an assertion: the evidence for choosing the camera rate, the display state rate and the
// exposure (F60). A capture is clean only if every row's exposure window fits inside one display
// state, so with a free-running camera the useful state rate is bounded by
//     Fd_clean <= 1 / (exposure + readout + 1/Fc)
// and a display faster than that loses states to torn captures. Run with
// --gtest_also_run_disabled_tests; it renders a few thousand captures (~1 min).
TEST(LiveReceiver, DISABLED_UnsynchronisedTimingSweep) {
    struct Arm { double fc, fd, exp_ms, readout_ms; };
    const std::vector<Arm> arms = {
        {30, 15, 16, 30},  // the configuration of the first verified transfer (F59)
        {30, 10, 8, 30},   {30, 15, 8, 30},   {30, 20, 8, 30},  {30, 24, 8, 30},
        {60, 15, 8, 15},   {60, 20, 8, 15},   {60, 24, 8, 15},  {60, 30, 8, 15},
        {60, 40, 8, 15},   {60, 30, 4, 15},   {60, 40, 4, 15},
    };
    std::printf("   camera  display  exposure readout   bound   torn  payload  verified   goodput\n");
    for (const Arm& a : arms) {
        double secs = 0.0, torn = 0.0, ok = 0.0;
        int done = 0;
        constexpr int kSeeds = 3;
        for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
            const TimingResult r = RunUnsynced(a.fc, a.fd, a.exp_ms, a.readout_ms, seed);
            torn += r.torn / kSeeds;
            ok += r.payload_ok / kSeeds;
            if (r.seconds > 0) {
                secs += r.seconds;
                ++done;
            }
        }
        const double bound = 1.0 / ((a.exp_ms + a.readout_ms) / 1000.0 + 1.0 / a.fc);
        std::printf("   %4.0f/s  %5.1f/s  %5.0f ms %5.0f ms  %5.1f/s  %4.0f%%  %5.0f%%  %d/%d %6.1f s  %5.1f KB/s\n",
                    a.fc, a.fd, a.exp_ms, a.readout_ms, bound, torn * 100, ok * 100, done, kSeeds,
                    done ? secs / done : 0.0,
                    done ? static_cast<double>(kSweepBytes) / 1024.0 / (secs / done) : 0.0);
    }
}

TEST(LiveReceiver, LanesOnSeparateThreadsCompleteTheSameTransfer) {
    // F60: one core cannot decode a 60 fps camera, so the app runs several lanes at once. Frames
    // rendered first, then dealt to three threads in turn exactly as LiveDecoder deals them; the
    // transfer must verify and every lane must have done work. Run under ASan in CI.
    Rig rig(30000);
    rig.motion.shake = 1.0;
    std::vector<Image8> frames;
    std::vector<std::int64_t> stamps;
    for (int i = 0; i < 90; ++i) {
        frames.push_back(rig.Capture());
        stamps.push_back(rig.ts());
    }
    LiveConfig lc;
    lc.lanes = 3;
    auto rx = LiveReceiver::Create(kGrid, lc);
    ASSERT_TRUE(rx.ok());
    LiveReceiver& r = rx.value();
    std::atomic<std::size_t> next{0};
    std::atomic<int> per_lane[3] = {0, 0, 0};
    std::vector<std::thread> threads;
    for (std::size_t lane = 0; lane < 3; ++lane) {
        threads.emplace_back([&, lane] {
            for (std::size_t i; (i = next.fetch_add(1)) < frames.size();) {
                const LiveStatus s = r.Ingest(frames[i].view(), stamps[i], lane);
                ++per_lane[lane];
                if (s.phase == LivePhase::kVerified) break;
            }
        });
    }
    for (auto& t : threads) t.join();
    const LiveStatus s = r.status();
    ASSERT_EQ(s.phase, LivePhase::kVerified) << s.message;
    auto out = r.File();
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), rig.file);
    for (int lane = 0; lane < 3; ++lane) EXPECT_GT(per_lane[lane].load(), 0) << "lane " << lane;
    EXPECT_EQ(LiveReceiver::Create(kGrid, LiveConfig{.lanes = 0}).error(), Error::kValueOutOfRange);
}

TEST(LiveReceiver, ASenderThatSwitchesFilesIsFollowedNotIgnored) {
    // The person sends one file, changes their mind and sends another while the receiver is
    // still up. Every frame of the second file is "foreign" to the first transfer's session, so
    // the receiver used to sit at its old percentage forever. It must notice, start over, say so,
    // and deliver the SECOND file.
    Rig first(60000, 0xAAAA, "first.bin");
    Rig second(25000, 0xBBBB, "second.bin");
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());
    LiveReceiver& r = rx.value();
    for (int i = 0; i < 12; ++i) {
        const Image8 img = first.Capture();
        r.Ingest(img.view(), first.ts());
    }
    ASSERT_GT(r.status().progress, 0.0) << "the first file never started: " << r.status().message;
    ASSERT_NE(r.status().phase, LivePhase::kVerified) << "first file too small for this test";

    bool told = false;
    for (int i = 0; i < 150 && r.status().phase != LivePhase::kVerified; ++i) {
        const Image8 img = second.Capture();
        const LiveStatus s = r.Ingest(img.view(), first.ts() + (i + 1) * 33'333'333LL);
        if (s.message.find("switched files") != std::string::npos) told = true;
    }
    const LiveStatus& s = r.status();
    ASSERT_EQ(s.phase, LivePhase::kVerified) << s.message;
    EXPECT_EQ(s.file_name, "second.bin");
    EXPECT_EQ(s.sender_switches, 1u);
    EXPECT_TRUE(told) << "the switch was never explained";
    auto out = r.File();
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), second.file);
}

TEST(LiveReceiver, ResendingTheSameFileIsNotASwitch) {
    // The other half of the rule: the same file sent again (the sender reopened it) carries the
    // same session id, so the receiver must carry on, not start over.
    Rig a(60000, 0xCCCC, "same.bin");
    Rig b(60000, 0xCCCC, "same.bin");
    auto rx = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(rx.ok());
    for (int i = 0; i < 10; ++i) {
        const Image8 img = a.Capture();
        rx.value().Ingest(img.view(), a.ts());
    }
    for (int i = 0; i < 120 && rx.value().status().phase != LivePhase::kVerified; ++i) {
        const Image8 img = b.Capture();
        rx.value().Ingest(img.view(), a.ts() + (i + 1) * 33'333'333LL);
    }
    EXPECT_EQ(rx.value().status().phase, LivePhase::kVerified) << rx.value().status().message;
    EXPECT_EQ(rx.value().status().sender_switches, 0u);
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

TEST(LiveReceiver, OnlyTheSendersGridDecodesAPayloadSoTheGridCanBeDetected) {
    // The app finds the sender's grid by running a receiver per candidate and keeping the first
    // that decodes a frame's payload (LiveDecoder, F57). That is only sound if a receiver on the
    // WRONG grid never does -- committing to one would leave a transfer that cannot finish.
    //
    // It was first written to commit on a decoded HEADER, and this test refuted that: a 120x208
    // receiver read a 120x200 sender's header on the very first frame, because the header band is
    // a few rows at the top and a near-miss grid puts them in nearly the same place (F58). So the
    // near-miss stays in the list, and the headers it reads are recorded rather than forbidden.
    Rig rig(20000);
    rig.motion.shake = 1.0;
    std::vector<LiveReceiver> wrong;
    for (GridGeometry g : {GridGeometry{120, 260}, GridGeometry{144, 240}, GridGeometry{180, 390},
                           GridGeometry{120, 208}}) {
        auto rx = LiveReceiver::Create(g);
        ASSERT_TRUE(rx.ok());
        wrong.push_back(std::move(rx).value());
    }
    auto right = LiveReceiver::Create(kGrid);
    ASSERT_TRUE(right.ok());

    std::vector<int> headers(wrong.size(), 0);
    for (int i = 0; i < 60; ++i) {
        const Image8 img = rig.Capture();
        for (std::size_t k = 0; k < wrong.size(); ++k) {
            const LiveStatus& s = wrong[k].Ingest(img.view(), rig.ts());
            ASSERT_EQ(s.payload_frames, 0u)
                << "a receiver on the wrong grid (candidate " << k << ") decoded a payload at "
                << "frame " << i;
            ASSERT_NE(s.phase, LivePhase::kReceiving) << "candidate " << k;
            if (s.phase == LivePhase::kReading) ++headers[k];
        }
        right.value().Ingest(img.view(), rig.ts());
    }
    for (std::size_t k = 0; k < wrong.size(); ++k) {
        std::printf("   wrong-grid candidate %zu: header-only frames %d of 60\n", k, headers[k]);
    }
    // It finishes the 20 KB file and then stops reading, so a handful is all there can be.
    EXPECT_GE(right.value().status().payload_frames, 5u)
        << "the right grid decoded too few payloads, so the test proves little";
    // The three grids the app actually offers are far enough apart that not even a header reads.
    for (std::size_t k = 0; k < 3; ++k) EXPECT_EQ(headers[k], 0) << "candidate " << k;
}

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

std::int64_t At(double seconds) { return static_cast<std::int64_t>(seconds * 1e9); }

}  // namespace

TEST(TransferRate, TheRealTransferCountsDownInsteadOfJumping) {
    // Progress from the first 2.2 MB hand-held transfer (Pixel 8 -> S26 Ultra, 2026-10-23), as the
    // receiver logged it: seconds since the manifest arrived, percent. The rate changed for real --
    // held at 3.5 px/cell for the first fifteen seconds, then brought in to 11 -- and in bursts as
    // the hand moved. The estimate the receiver SHOWED then read 2 min, 4, 6, 8, 12, 9, 12, 9, 6,
    // 2 min, 81 s in the first sixteen seconds.
    const double trace[][2] = {
        {0.0, 4}, {1.1, 5}, {5.9, 7}, {6.5, 8}, {10.2, 9}, {14.5, 10}, {15.0, 11}, {15.4, 12},
        {15.8, 13}, {16.1, 14}, {17.9, 15}, {19.4, 16}, {21.6, 17}, {22.1, 18}, {22.2, 19},
        {22.6, 20}, {22.9, 21}, {23.4, 22}, {23.7, 23}, {24.1, 24}, {24.5, 25}, {24.9, 26},
        {25.3, 27}, {25.7, 28}, {26.2, 29}, {26.5, 30}, {26.9, 31}, {27.4, 32}, {27.6, 33},
        {28.5, 35}, {29.3, 37}, {29.5, 38}, {30.4, 40}, {31.7, 43}, {32.4, 45}, {32.6, 46},
        {33.5, 48}, {34.2, 50}, {34.5, 51}, {35.4, 53}, {36.7, 56}, {37.4, 58}, {37.6, 59},
        {38.4, 61}, {39.7, 64}, {40.5, 66}, {41.6, 69}, {42.6, 71}, {42.7, 72}, {43.5, 74},
        {44.6, 77}, {45.5, 79}, {46.5, 82}, {47.6, 84}, {47.6, 85}, {48.5, 87}, {49.6, 90},
        {50.6, 92}, {51.5, 95}, {52.6, 97},
    };
    const double origin = 20.0;  // the first payload arrived 11 s before the manifest
    TransferRate rate;
    std::deque<std::pair<double, double>> window;  // what the receiver used to do: 4 s, raw
    double shown_prev = -1.0;
    double raw_prev = -1.0;
    double shown_wander = 0.0;  // total distance each moved away from a steady countdown
    double raw_wander = 0.0;
    // Seconds in which the time left, AS THE CARD WORDS IT ("4 min left", "42 s left"), went up.
    const auto worded = [](double s) { return s < 90.0 ? std::ceil(s) : 60.0 * std::ceil(s / 60.0); };
    int shown_rises = 0;
    int raw_rises = 0;
    std::string shown_log;
    std::string raw_log;
    std::size_t k = 0;
    for (int step = 0; step <= 52 * 50; ++step) {  // camera frames at 50 fps
        const double t = step / 50.0;
        while (k + 1 < std::size(trace) && trace[k + 1][0] <= t) ++k;
        const double p = trace[k][1] / 100.0;
        rate.Update(At(origin + t), p, false, At(origin - 11.0));
        window.emplace_back(t, p);
        while (window.size() > 2 && t - window.front().first > 4.0) window.pop_front();
        if (step % 50 != 0) continue;

        const double span = t - window.front().first;
        const double windowed = span > 0.5 ? (p - window.front().second) / span : 0.0;
        const double raw = windowed > 0.0 ? (1.0 - p) / windowed : -1.0;
        const double shown = rate.seconds_remaining();
        ASSERT_GE(shown, 0.0) << "an estimate at every second of a transfer that never paused";
        if (shown_prev > 0.0 && raw_prev > 0.0 && raw > 0.0 && p < 0.9) {
            shown_wander += std::abs(shown - (shown_prev - 1.0));
            raw_wander += std::abs(raw - (raw_prev - 1.0));
            shown_rises += worded(shown) > worded(shown_prev) ? 1 : 0;
            raw_rises += worded(raw) > worded(raw_prev) ? 1 : 0;
        }
        if (step % 100 == 0 && t <= 30.0) {
            shown_log += " " + std::to_string(static_cast<int>(std::ceil(shown)));
            raw_log += " " + std::to_string(raw > 0.0 ? static_cast<int>(std::ceil(raw)) : -1);
        }
        shown_prev = shown;
        raw_prev = raw;
    }
    std::printf("   seconds left, every 2 s for the first 30:\n     shown:%s\n     raw:  %s\n"
                "   wandered from a countdown by %.0f s shown, %.0f s raw; rose in %d s, %d s\n",
                shown_log.c_str(), raw_log.c_str(), shown_wander, raw_wander, shown_rises,
                raw_rises);
    EXPECT_LT(shown_wander, raw_wander / 3) << "the time left should count down, not wander";
    EXPECT_LT(shown_rises, raw_rises / 2) << "and should rarely go up";
}

TEST(TransferRate, ASteadyTransferReadsExactly) {
    TransferRate rate;
    for (int step = 0; step <= 10 * 30; ++step) {
        rate.Update(At(1.0 + step / 30.0), 0.02 * step / 30.0, false);  // 2% a second
    }
    // Ten seconds in: 20% done at 2% a second leaves 40 s.
    EXPECT_NEAR(rate.rate(), 0.02, 0.0002);
    EXPECT_NEAR(rate.seconds_remaining(), 40.0, 0.5);
}

TEST(TransferRate, APauseHasNoTimeLeftAndDoesNotSlowTheResumedEstimate) {
    TransferRate rate;
    double p = 0.0;
    std::int64_t t = kSecond;
    const auto run = [&](double seconds, double per_second, bool paused) {
        for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
            t += kSecond / 30;
            p += per_second / 30.0;
            rate.Update(t, p, paused);
        }
    };
    run(10.0, 0.02, false);
    run(5.0, 0.0, true);  // aimed at the floor
    EXPECT_LT(rate.seconds_remaining(), 0.0) << "no promise while nothing arrives";
    EXPECT_EQ(rate.rate(), 0.0) << "the speed right now is nothing";
    run(1.0, 0.02, false);
    // 22% done at 2% a second leaves 39 s. A window still holding the pause would say ~3x that.
    EXPECT_NEAR(rate.seconds_remaining(), 39.0, 4.0);
    EXPECT_NEAR(rate.rate(), 0.02, 0.002);
}

TEST(TransferRate, ProgressMadeBeforeTheFirstUpdateIsAnAverageNotASpike) {
    // The manifest arrived last, with half the file already buffered over the 10 s before it.
    TransferRate rate;
    rate.Update(At(20.0), 0.5, false, At(10.0));
    rate.Update(At(20.1), 0.5, false, At(10.0));
    EXPECT_NEAR(rate.rate(), 0.05, 1e-9);
    EXPECT_NEAR(rate.seconds_remaining(), 10.0, 0.2);
}

TEST(TransferRate, AnOlderUpdateFromASlowerLaneIsIgnored) {
    TransferRate rate;
    for (int step = 0; step <= 5 * 30; ++step) rate.Update(At(1.0 + step / 30.0), 0.02 * step / 30.0, false);
    const double before = rate.seconds_remaining();
    rate.Update(At(3.0), 0.0, false);  // decoded late, from two seconds ago
    EXPECT_EQ(rate.seconds_remaining(), before);
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
    EXPECT_NE(paused.message.find("nothing received so far is lost"), std::string::npos);
    EXPECT_DOUBLE_EQ(paused.progress, before) << "progress must hold, not reset";
    EXPECT_LT(paused.seconds_remaining, 0.0) << "no time estimate while nothing arrives";
    EXPECT_EQ(paused.bytes_per_second, 0.0) << "and no speed";

    // Back on target.
    for (int i = 0; i < 300 && rx.value().status().phase != LivePhase::kVerified; ++i) {
        const Image8 img = rig.Capture();
        rx.value().Ingest(img.view(), rig.ts());
    }
    ASSERT_EQ(rx.value().status().phase, LivePhase::kVerified) << rx.value().status().message;
    EXPECT_EQ(rx.value().File().value(), rig.file);
    // The link's own time: from the first data, so less than the whole run, which began with the
    // screen in view -- and, here, includes the second spent pointed at the floor.
    EXPECT_GT(rx.value().status().transfer_seconds, 1.0);
    EXPECT_LT(rx.value().status().transfer_seconds, rig.t());
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
