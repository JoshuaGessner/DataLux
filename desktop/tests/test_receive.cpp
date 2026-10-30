// Receiving on the desktop, end to end without a camera: a real transmit session's frames, drawn as
// a camera would see a screen held square to it, offered to the Receiver as camera frames -- through
// its thread, its grid search, opening the envelope and saving.
#include "receiver.h"
#include "camera.h"

#include <fileflow/envelope.h>
#include <fileflow/session.h>
#include <fileflow/modulation.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

namespace fileflow::desktop {
namespace {

constexpr int kPitch = 4;
constexpr int kMargin = 48;

// The cells as a screen shows them: black and white (Steady), on black, a margin round them.
std::vector<std::uint8_t> Draw(const CellMatrix& cells, int* w, int* h) {
    *w = static_cast<int>(cells.cols()) * kPitch + 2 * kMargin;
    *h = static_cast<int>(cells.rows()) * kPitch + 2 * kMargin;
    std::vector<std::uint8_t> img(static_cast<std::size_t>(*w) * static_cast<std::size_t>(*h), 0);
    for (int y = 0; y < static_cast<int>(cells.rows()) * kPitch; ++y) {
        for (int x = 0; x < static_cast<int>(cells.cols()) * kPitch; ++x) {
            img[static_cast<std::size_t>((y + kMargin) * *w + x + kMargin)] =
                cells.data()[static_cast<std::size_t>(y / kPitch) * cells.cols() + static_cast<std::size_t>(x / kPitch)] != 0 ? 230 : 20;
        }
    }
    return img;
}

// The desktop renderer's RGB channel rule, with row padding as camera surfaces can have.
// These bytes go through the production RGB conversion before they reach the real receiver.
std::vector<std::uint8_t> DrawRgb(const CellMatrix& cells, int* w, int* h, int* pitch) {
    *w = static_cast<int>(cells.cols()) * kPitch + 2 * kMargin;
    *h = static_cast<int>(cells.rows()) * kPitch + 2 * kMargin;
    *pitch = *w * 4 + 16;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(*pitch) * static_cast<std::size_t>(*h), 0);
    for (int y = 0; y < static_cast<int>(cells.rows()) * kPitch; ++y) {
        for (int x = 0; x < static_cast<int>(cells.cols()) * kPitch; ++x) {
            const auto cell = cells.data()[static_cast<std::size_t>(y / kPitch) * cells.cols() + static_cast<std::size_t>(x / kPitch)];
            const auto at = static_cast<std::size_t>(y + kMargin) * static_cast<std::size_t>(*pitch) + static_cast<std::size_t>(x + kMargin) * 4;
            for (int channel = 0; channel < 3; ++channel) rgba[at + static_cast<std::size_t>(channel)] = ColorChannelOn(cell, channel) ? 255 : 0;
            rgba[at + 3] = 255;
        }
    }
    return rgba;
}

std::vector<std::uint8_t> Text(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>("light camera screen "[i % 20]);
    return v;
}

// Runs a whole transfer of [envelope] into a Receiver; returns its result.
std::optional<ReceiveResult> Transfer(const std::vector<std::uint8_t>& envelope, const std::string& link_name,
                                      std::vector<secure::SealKey> keys, bool require_encrypted, const std::filesystem::path& dir,
                                      bool full_rgb = false) {
    auto layout = FrameLayout::Create({120, 260}, LayoutConfig{});
    EXPECT_TRUE(layout.ok());
    TransmitConfig tc;
    tc.profile = full_rgb ? ModulationProfile::kM5EightColor : ModulationProfile::kM0BinaryLuminance;
    auto tx = TransmitSession::Create(layout.value(), envelope, link_name, SessionIdFor(envelope, link_name), tc);
    EXPECT_TRUE(tx.ok());
    Receiver rx(std::move(keys), require_encrypted, dir);
    CellMatrix cells(120, 260);
    std::uint64_t ts = 0;
    for (int i = 0; i < 2000 && !rx.Result(); ++i) {
        ASSERT_TRUE(tx.value().Next(&cells).ok()) << i, std::nullopt;
        int w = 0;
        int h = 0;
        const auto before = rx.decoded();
        if (full_rgb) {
            int pitch = 0;
            const auto rgba = DrawRgb(cells, &w, &h, &pitch);
            std::vector<std::uint8_t> luma, u, v;
            detail::RgbaToYuv444(rgba.data(), w, h, pitch, &luma, &u, &v);
            EXPECT_EQ(u.size(), luma.size());
            EXPECT_EQ(v.size(), luma.size());
            rx.Offer(luma, u, v, w, h, ts += 33'000'000, true);
        } else {
            const auto luma = Draw(cells, &w, &h);
            const std::vector<std::uint8_t> chroma(static_cast<std::size_t>(w / 2) * static_cast<std::size_t>(h / 2), 128);
            rx.Offer(luma, chroma, chroma, w, h, ts += 33'000'000);
        }
        // One frame at a time: wait for the decoder to take this one before showing the next.
        for (int k = 0; k < 2000 && rx.decoded() == before && !rx.Result(); ++k) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return rx.Result();
}

class ReceiveTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = std::filesystem::path(::testing::TempDir()) /
               ("ff-receive-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path dir_;
};

TEST(CameraColor, AdjacentRgbPixelsKeepTheirOwnChromaWithPaddedRows) {
    // Opposite primaries sit at adjacent pixels, including odd x/y. Decimating a 2x2 block
    // would erase their distinction even though the camera's RGB surface retained it.
    const std::vector<std::uint8_t> rgba = {
        255, 0, 0, 255, 0, 0, 255, 255, 99, 99, 99, 99,
        0, 255, 0, 255, 255, 255, 255, 255, 99, 99, 99, 99};
    std::vector<std::uint8_t> luma, u, v;
    detail::RgbaToYuv444(rgba.data(), 2, 2, 12, &luma, &u, &v);
    ASSERT_EQ(luma.size(), 4U);
    ASSERT_EQ(u.size(), 4U);
    ASSERT_EQ(v.size(), 4U);
    EXPECT_GT(v[0], v[1]);  // red carries more V than blue
    EXPECT_GT(u[1], u[0]);  // blue carries more U than red
    EXPECT_LT(u[2], u[3]);  // green and white remain distinct on the second row
    EXPECT_LT(v[2], v[3]);
    EXPECT_EQ(u[3], 128);
    EXPECT_EQ(v[3], 128);
}

TEST_F(ReceiveTest, FullResolutionRgbEightColorFileVerifiesAndIsSaved) {
    const auto data = Text(30'000);
    const auto env = PackEnvelope(data, "color.bin", CompressionMode::kOff).value().bytes;
    const auto r = Transfer(env, "color.bin", {}, false, dir_, true);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->ok) << r->problem;
    ASSERT_EQ(r->files.size(), 1U);
    std::ifstream in(r->files[0].path, std::ios::binary);
    const std::vector<std::uint8_t> got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got, data);
}

TEST(ReceiveDelivery, BusyWorkerOwnsItsFrameAndNextDecodeGetsNewestPending) {
    detail::LatestFrame frames;
    std::vector<std::uint8_t> luma(16, 10), chroma(16, 128);
    ASSERT_TRUE(frames.Offer(luma, chroma, chroma, 4, 4, 10, true));
    std::promise<void> active_started, release;
    auto started = active_started.get_future();
    auto resume = release.get_future();
    struct Observed { int active = -1, next = -1; std::uint64_t timestamp = 0; bool full_resolution = false; };
    auto worker = std::async(std::launch::async, [&] {
        Observed observed;
        detail::CameraFrame active;
        if (!frames.Take(&active)) return observed;
        active_started.set_value();
        resume.wait();  // deterministic busy decoder, while the producer replaces pending frames
        observed.active = active.luma[0];
        if (frames.Take(&active)) {
            observed.next = active.luma[0];
            observed.timestamp = active.timestamp_ns;
            observed.full_resolution = active.full_resolution;
        }
        return observed;
    });
    const bool running = started.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!running) {
        frames.Close();
        release.set_value();
        worker.wait();
        FAIL() << "Worker did not take the first frame";
    }
    std::fill(luma.begin(), luma.end(), 20);
    EXPECT_TRUE(frames.Offer(luma, chroma, chroma, 4, 4, 20, true));
    std::fill(luma.begin(), luma.end(), 30);
    EXPECT_TRUE(frames.Offer(luma, chroma, chroma, 4, 4, 30, true));
    std::fill(luma.begin(), luma.end(), 40);  // producer reuses its buffer after the copy
    EXPECT_FALSE(frames.Offer(luma, chroma, chroma, 4, 4, 20, true));
    EXPECT_FALSE(frames.Offer(luma, chroma, chroma, 4, 4, 30, true));
    release.set_value();
    const bool completed = worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!completed) frames.Close();
    const auto observed = worker.get();
    EXPECT_TRUE(completed) << "Finishing the active decode lost the newer pending frame";
    EXPECT_EQ(observed.active, 10);
    EXPECT_EQ(observed.next, 30);
    EXPECT_EQ(observed.timestamp, 30U);
    EXPECT_TRUE(observed.full_resolution);
    const auto stats = frames.Stats();
    EXPECT_EQ(stats.offered, 5U);
    EXPECT_EQ(stats.accepted, 3U);
    EXPECT_EQ(stats.replaced, 1U);
    EXPECT_EQ(stats.stale, 2U);
    frames.Close();
    EXPECT_FALSE(frames.Offer(luma, chroma, chroma, 4, 4, 40, true));
}

TEST(ReceiveDelivery, CloseWakesAnIdleWorkerAndDiscardsPendingFrames) {
    detail::LatestFrame idle;
    auto waiter = std::async(std::launch::async, [&] { detail::CameraFrame active; return idle.Take(&active); });
    idle.Close();
    ASSERT_EQ(waiter.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(waiter.get());
    detail::LatestFrame pending;
    const std::vector<std::uint8_t> luma(16, 10), chroma(4, 128);
    ASSERT_TRUE(pending.Offer(luma, chroma, chroma, 4, 4, 0, false));
    pending.Close();
    detail::CameraFrame active;
    EXPECT_FALSE(pending.Take(&active));
}

TEST(CameraFormat, PreservesMoreSamplesAlongTheCodeInsteadOfMaximizingSquareArea) {
    SDL_CameraSpec square{SDL_PIXELFORMAT_NV12, SDL_COLORSPACE_BT709_LIMITED, 1552, 1552, 30, 1};
    SDL_CameraSpec wide{SDL_PIXELFORMAT_NV12, SDL_COLORSPACE_BT709_LIMITED, 1920, 1080, 30, 1};
    auto portrait = wide;
    std::swap(portrait.width, portrait.height);
    EXPECT_GT(square.width * square.height, wide.width * wide.height);
    EXPECT_TRUE(detail::PreferReceiveFormat(wide, &square));
    EXPECT_FALSE(detail::PreferReceiveFormat(square, &wide));
    EXPECT_DOUBLE_EQ(detail::ReceiveCellPitch(wide), detail::ReceiveCellPitch(portrait));
    EXPECT_GT(detail::ReceiveCellPitch(wide), detail::ReceiveCellPitch(square) * 1.2);
}

TEST(CameraFormat, RejectsInvalidSlowAndOversizedModesAndBreaksPitchTiesByRate) {
    SDL_CameraSpec mode{SDL_PIXELFORMAT_NV12, SDL_COLORSPACE_BT709_LIMITED, 1920, 1080, 30, 1};
    auto faster = mode;
    faster.framerate_numerator = 60;
    EXPECT_TRUE(detail::PreferReceiveFormat(faster, &mode));
    EXPECT_FALSE(detail::PreferReceiveFormat(mode, &faster));
    for (int value : {0, -1, 2000}) {
        auto invalid = mode;
        invalid.height = value;
        EXPECT_FALSE(detail::PreferReceiveFormat(invalid, nullptr));
    }
    mode.framerate_numerator = 15;
    EXPECT_FALSE(detail::PreferReceiveFormat(mode, nullptr));
    mode.framerate_numerator = 30;
    mode.framerate_denominator = 0;
    EXPECT_FALSE(detail::PreferReceiveFormat(mode, nullptr));
}

TEST_F(ReceiveTest, AFileArrivesWholeAndIsSaved) {
    const auto data = Text(30'000);
    const auto env = PackEnvelope(data, "notes.txt", CompressionMode::kOff).value().bytes;
    const auto r = Transfer(env, "notes.txt", {}, false, dir_);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->ok) << r->problem;
    ASSERT_EQ(r->files.size(), 1U);
    EXPECT_EQ(r->files[0].name, "notes.txt");
    std::ifstream in(r->files[0].path, std::ios::binary);
    const std::vector<std::uint8_t> got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got, data);
}

TEST_F(ReceiveTest, ATeamsFileOpensForAMemberAndNotForAStranger) {
    const auto team = secure::Team::Create("Crew", "Phone", 1);
    const auto plain = PackEnvelope(Text(8000), "orders.txt", CompressionMode::kAuto).value().bytes;
    const auto sealed = secure::Seal(plain, team.AsSealKey());

    const auto mine = Transfer(sealed, "Encrypted file", {team.AsSealKey()}, true, dir_);
    ASSERT_TRUE(mine.has_value());
    ASSERT_TRUE(mine->ok) << mine->problem;
    EXPECT_EQ(mine->files[0].name, "orders.txt");
    EXPECT_EQ(mine->sealed_to, "team Crew");

    const auto other = secure::Team::Create("Crew", "Phone", 1);  // same name, another key
    const auto theirs = Transfer(sealed, "Encrypted file", {other.AsSealKey()}, true, dir_ / "x");
    ASSERT_TRUE(theirs.has_value());
    EXPECT_FALSE(theirs->ok);
    EXPECT_FALSE(theirs->problem.empty());
}

TEST_F(ReceiveTest, AFileInTheClearIsRefusedInEncryptedMode) {
    const auto env = PackEnvelope(Text(2000), "plain.txt", CompressionMode::kOff).value().bytes;
    const auto r = Transfer(env, "plain.txt", {}, true, dir_);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->ok);
    EXPECT_TRUE(std::filesystem::is_empty(dir_));  // nothing written
}

TEST_F(ReceiveTest, ExistingFilesAreNeverOverwritten) {
    { std::ofstream existing(dir_ / "notes.txt"); existing << "keep me"; }
    const auto env = PackEnvelope(Text(2000), "notes.txt", CompressionMode::kOff).value().bytes;
    const auto r = Transfer(env, "notes.txt", {}, false, dir_);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->ok) << r->problem;
    ASSERT_EQ(r->files.size(), 1U);
    EXPECT_EQ(r->files[0].name, "notes (1).txt");
    std::ifstream in(dir_ / "notes.txt");
    std::string original;
    std::getline(in, original);
    EXPECT_EQ(original, "keep me");
}

TEST_F(ReceiveTest, WindowsPathsStreamsAndDevicesBecomeOrdinaryFiles) {
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files = {
        {"C:escape.txt", Text(50)}, {"file.txt:stream", Text(50)}, {"NUL.txt", Text(50)}};
    const auto env = PackBatchEnvelope(files, "3 files", CompressionMode::kOff).value().bytes;
    const auto r = Transfer(env, "3 files", {}, false, dir_);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->ok) << r->problem;
    ASSERT_EQ(r->files.size(), 3U);
    EXPECT_EQ(r->files[0].name, "C_escape.txt");
    EXPECT_EQ(r->files[1].name, "file.txt_stream");
    EXPECT_EQ(r->files[2].name, "_NUL.txt");
    for (const auto& file : r->files) EXPECT_EQ(file.path.parent_path(), dir_);
}

TEST_F(ReceiveTest, DanglingSymlinksAreNotFollowed) {
    const auto target = dir_ / "must-not-be-created.txt";
    std::error_code ec;
    std::filesystem::create_symlink(target, dir_ / "notes.txt", ec);
    if (ec) GTEST_SKIP() << "Symlinks unavailable: " << ec.message();
    const auto env = PackEnvelope(Text(2000), "notes.txt", CompressionMode::kOff).value().bytes;
    const auto r = Transfer(env, "notes.txt", {}, false, dir_);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->ok) << r->problem;
    ASSERT_EQ(r->files.size(), 1U);
    EXPECT_EQ(r->files[0].name, "notes (1).txt");
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_TRUE(std::filesystem::is_symlink(dir_ / "notes.txt"));
}

}  // namespace
}  // namespace fileflow::desktop
