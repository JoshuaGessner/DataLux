// What the desktop sender sends, and how: the file packed in the app's envelope, a transmit session
// over it at one of the app's speeds. Everything here matches the Android app setting for setting,
// so a receiver cannot tell this screen from a phone's.
#pragma once

#include <fileflow/envelope.h>
#include <fileflow/grid.h>
#include <fileflow/modulation.h>
#include <fileflow/session.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fileflow::desktop {

// The app's Speed.kt.
struct Speed {
    const char* name;
    std::uint32_t cols;
    std::uint32_t rows;
    double states_per_second;
    ModulationProfile profile;
    const char* hint;  // one line, for the person choosing
};
inline constexpr std::array<Speed, 3> kSpeeds = {{
    {"Steady", 120, 260, 20.0, ModulationProfile::kM0BinaryLuminance,
     "Black and white: for any camera, in any light."},
    {"Fast", 120, 260, 30.0, ModulationProfile::kM3FourColor,
     "Four colours: suits most phones."},
    {"Fastest", 120, 260, 30.0, ModulationProfile::kM5EightColor,
     "Eight colours: for cameras that see colour sharply."},
}};

inline constexpr std::size_t kFecNsym = 32;
// What one transfer may carry, as on the phone: a receiver holds the whole file in memory.
inline constexpr std::uint64_t kMaxFileBytes = 64ULL * 1024 * 1024;

std::string FormatBytes(std::uint64_t b);

// Paths arrive as UTF-8 (SDL converts Windows' arguments and dialogs); the name a receiver saves
// under is the last component.
std::filesystem::path PathOf(const std::string& utf8);
std::string NameOf(const std::filesystem::path& p);

// Files read and packed for sending, or why not.
struct Prepared {
    PackedEnvelope envelope;
    std::string name;     // what the receiver shows: the file's, or "3 files"
    std::size_t files = 0;
    std::string problem;  // non-empty: nothing to send
    // Sealed to a pairing or team: the link then shows only "Encrypted file" and says nothing of
    // the real name and size -- inside the envelope, as on the phone. A batch advisory may remain.
    bool sealed = false;
};

// Reads and packs `paths`. Slow for a big file -- reading and compressing -- so run it off the
// thread that draws.
Prepared Prepare(const std::vector<std::string>& paths, CompressionMode mode);

// Packs [text] as a file named [name]: the clipboard, sent as a note.
Prepared PrepareText(const std::string& text, const std::string& name, CompressionMode mode);

// One transmit session over a prepared file at one speed.
struct Transfer {
    std::unique_ptr<FrameLayout> layout;
    std::optional<TransmitSession> session;
    CellMatrix cells;
    Speed speed;

    Transfer(std::unique_ptr<FrameLayout> l, const Speed& s)
        : layout(std::move(l)), cells(s.cols, s.rows), speed(s) {}

    // Frames in one pass over the file's data: how long one pass takes, at the speed's rate.
    [[nodiscard]] std::uint64_t PassFrames() const;
};

std::unique_ptr<Transfer> Open(const Prepared& p, const Speed& speed, std::string* problem);

// The app's fragment shader: a cell's low three bits light red, green and blue. M0's 0 and 255
// come out black and white by the same rule.
inline std::uint32_t CellColour(std::uint8_t v) {
    return 0xFF000000U | ((v & 4U) ? 0xFF0000U : 0U) | ((v & 2U) ? 0x00FF00U : 0U) | ((v & 1U) ? 0x0000FFU : 0U);
}

}  // namespace fileflow::desktop
