// Receiving on the desktop, end to end without a camera: a real transmit session's frames, drawn as
// a camera would see a screen held square to it, offered to the Receiver as camera frames -- through
// its thread, its grid search, opening the envelope and saving.
#include "receiver.h"

#include <fileflow/envelope.h>
#include <fileflow/session.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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

std::vector<std::uint8_t> Text(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>("light camera screen "[i % 20]);
    return v;
}

// Runs a whole transfer of [envelope] into a Receiver; returns its result.
std::optional<ReceiveResult> Transfer(const std::vector<std::uint8_t>& envelope, const std::string& link_name,
                                      std::vector<secure::SealKey> keys, bool require_encrypted, const std::filesystem::path& dir) {
    auto layout = FrameLayout::Create({120, 260}, LayoutConfig{});
    EXPECT_TRUE(layout.ok());
    TransmitConfig tc;
    tc.profile = ModulationProfile::kM0BinaryLuminance;
    auto tx = TransmitSession::Create(layout.value(), envelope, link_name, SessionIdFor(envelope, link_name), tc);
    EXPECT_TRUE(tx.ok());
    Receiver rx(std::move(keys), require_encrypted, dir);
    CellMatrix cells(120, 260);
    std::uint64_t ts = 0;
    for (int i = 0; i < 2000 && !rx.Result(); ++i) {
        ASSERT_TRUE(tx.value().Next(&cells).ok()) << i, std::nullopt;
        int w = 0;
        int h = 0;
        const auto luma = Draw(cells, &w, &h);
        const std::vector<std::uint8_t> chroma(static_cast<std::size_t>(w / 2) * static_cast<std::size_t>(h / 2), 128);
        const auto before = rx.decoded();
        rx.Offer(luma, chroma, chroma, w, h, ts += 33'000'000);
        // One frame at a time: wait for the decoder to take this one before showing the next.
        for (int k = 0; k < 2000 && rx.decoded() == before && !rx.Result(); ++k) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return rx.Result();
}

class ReceiveTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = std::filesystem::path(::testing::TempDir()) / ("ff-receive-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path dir_;
};

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

}  // namespace
}  // namespace fileflow::desktop
