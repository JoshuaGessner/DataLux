// QR codes as pairing uses them: a real invite drawn as the screen draws it, read back by the reader
// the camera feeds -- at camera-ish scales, offsets, and mirrored.
#include "camera.h"
#include "secure.h"

#include <qrcodegen.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace fileflow::desktop {
namespace {

// [text]'s code in a grey image: [module] pixels a module, at (ox, oy), on mid-grey, as a camera
// sees a white screen in a dim room.
std::vector<std::uint8_t> Picture(const std::string& text, int w, int h, int module, int ox, int oy, bool mirror) {
    const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
    std::vector<std::uint8_t> img(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 110);
    const int n = qr.getSize();
    constexpr int kMargin = 3;
    for (int y = 0; y < (n + 2 * kMargin) * module; ++y) {
        for (int x = 0; x < (n + 2 * kMargin) * module; ++x) {
            const int mx = x / module - kMargin;
            const int my = y / module - kMargin;
            const bool dark = mx >= 0 && my >= 0 && mx < n && my < n && qr.getModule(mx, my);
            const int px = mirror ? ox + (n + 2 * kMargin) * module - 1 - x : ox + x;
            img[static_cast<std::size_t>((oy + y) * w + px)] = dark ? 30 : 225;
        }
    }
    return img;
}

std::string AnInvite() {
    auto a = secure::Pairing::Start();
    auto b = secure::Pairing::Start();
    b->ReadCode(a->QrText());
    const auto team = secure::Team::Create("Search and rescue, north", "Laptop", 1);
    return secure::invite::Encode(team, *b->Key(0));
}

TEST(Qr, AnInviteIsReadBackAtCameraScales) {
    const std::string text = AnInvite();
    for (int module : {3, 5, 8}) {
        const auto img = Picture(text, 1280, 720, module, 200, 40, false);
        EXPECT_EQ(ReadQr(img.data(), 1280, 720).value_or("(nothing)"), text) << module;
    }
}

TEST(Qr, AMirroredCodeIsReadToo) {
    const std::string text = AnInvite();
    const auto img = Picture(text, 1280, 720, 5, 300, 60, true);
    EXPECT_EQ(ReadQr(img.data(), 1280, 720).value_or("(nothing)"), text);
}

TEST(Qr, APairingCodeWithItsAckFits) {
    auto a = secure::Pairing::Start();
    auto b = secure::Pairing::Start();
    a->ReadCode(b->QrText());
    const std::string text = a->QrText();  // key and acknowledgement
    const auto img = Picture(text, 1280, 720, 6, 100, 100, false);
    EXPECT_EQ(ReadQr(img.data(), 1280, 720).value_or("(nothing)"), text);
}

TEST(Qr, NoCodeIsNothing) {
    std::vector<std::uint8_t> img(640 * 480, 128);
    EXPECT_FALSE(ReadQr(img.data(), 640, 480).has_value());
}

}  // namespace
}  // namespace fileflow::desktop
