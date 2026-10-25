// What a transfer carries: the file (or several), named, compressed when that pays.
//
// This is the format the Android app's `Envelope.kt` writes and reads, so a sender built on this --
// the desktop sender -- is opened by any phone. Unencrypted only: sealing to a pairing or a team
// stays in the app for now, which has the keys.
//
//   "FFE2" | flags (bit 2: a batch) | name length (u16) | name | original size (u64) | codec (u8)
//          | compress ms (u32) | data, compressed by the codec (0 none, 2 zstd)
//   batch data = "FFB1" | count (u16) | count x [ name length (u16) | name | size (u64) | bytes ]
//
// Integers are big-endian, as Java's ByteBuffer writes them. Names are UTF-8, cut to 255 bytes.
#pragma once

#include <fileflow/result.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fileflow {

// How hard to compress (Envelope.Mode in the app). Auto: a quick probe of the first 256 KB decides
// whether the file compresses at all -- a photo or an archive costs a few milliseconds -- and if
// it does, zstd 19. Maximum: the strongest level, for everything that compresses.
enum class CompressionMode : std::uint8_t { kOff, kAuto, kMaximum };

struct PackedEnvelope {
    std::vector<std::uint8_t> bytes;
    std::uint64_t original_size = 0;  // of the data before compression (a batch's whole data)
    std::uint8_t codec = 0;           // 0 none, 2 zstd
    std::uint32_t compress_ms = 0;
};

inline constexpr std::size_t kEnvelopeMaxBatchFiles = 1000;
inline constexpr std::uint64_t kEnvelopeMaxOriginalBytes = 512ULL * 1024 * 1024;

// One file. kPayloadTooLarge past kEnvelopeMaxOriginalBytes.
[[nodiscard]] Result<PackedEnvelope> PackEnvelope(const std::vector<std::uint8_t>& data,
                                                  const std::string& name, CompressionMode mode);

// Several files as one transfer, named for the person by `name` ("3 files"). Compressed as a whole.
[[nodiscard]] Result<PackedEnvelope> PackBatchEnvelope(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files,
    const std::string& name, CompressionMode mode);

}  // namespace fileflow
