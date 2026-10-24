// File compression before transfer: Zstandard.
//
// The light link is slow -- hundreds of kilobytes a second -- so time spent compressing is almost
// always repaid many times over in time not spent transmitting. That inverts the usual trade:
// strong levels, and a long match window, are worth it here for anything that compresses at all.
// Deflate, which this replaced, sees repeats only 32 KB back; zstd with a long window sees them
// across the whole file (up to 128 MB), which is where logs, datasets and documents gain most.
//
// Decompression is BOUNDED: the size it must produce is declared by the sender, checked against
// the stream's own claim, and the output buffer is exactly that size -- a stream that would expand
// past it fails rather than growing (INPUT-VALIDATION: no decompression bombs).
#pragma once

#include <fileflow/result.h>

#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

struct CompressOptions {
    int level = 19;             // 1 (fastest) .. MaxCompressionLevel() (smallest)
    bool long_window = false;   // long-distance matching over a 128 MB window
};

[[nodiscard]] int MaxCompressionLevel() noexcept;

// Compresses `in` as one zstd frame recording its own size.
[[nodiscard]] Result<std::vector<std::uint8_t>> Compress(std::span<const std::uint8_t> in,
                                                        CompressOptions options = {});

// Decompresses a zstd frame that must yield exactly `expected` bytes -- no more, no fewer.
// kLengthMismatch if it claims or produces any other size; kUncorrectable if it is not valid.
[[nodiscard]] Result<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> in,
                                                          std::size_t expected);

}  // namespace fileflow
