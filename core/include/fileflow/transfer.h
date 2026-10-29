// Session and file layers: manifest, block partitioning, reassembly, hash verification.
//
// THE GUARANTEE (G3): a completed transfer means the receiver holds a file whose SHA-256
// matches the sender's. Partial, corrupt or silently truncated output is a defect of the
// highest severity, and a hash mismatch delivers NOTHING (docs/security/THREAT-MODEL.md T5).
#pragma once

#include <fileflow/fountain.h>
#include <fileflow/frame.h>
#include <fileflow/hash.h>
#include <fileflow/result.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace fileflow {

// What the transferred bytes ARE, said up front so a receiver can show the speed that matters to the
// person -- the original file's bytes per second, not the link's -- before the file has arrived.
// Optional: a manifest without it is still complete (an encrypted transfer omits it).
struct ContentHint {
    std::uint64_t original_size = 0;  // the file before compression; 0 = not said
    std::uint8_t codec = 0;           // 0 none, 1 deflate, 2 zstd
    std::uint32_t compress_ms = 0;    // what compressing it cost the sender
    // Advisory only: lets an app explain batch requirements before the file arrives.
    // The verified envelope, never this hint, decides whether a received file is a batch.
    bool batch = false;

    static constexpr std::uint8_t kExtensionType = 1;
    static constexpr std::uint8_t kEncodedSize = 8 + 1 + 4;
    static constexpr std::uint8_t kBatchExtensionType = 2;
};

struct FileManifest {
    std::uint64_t file_size = 0;
    Sha256::Digest sha256{};
    std::string file_name;      // a DISPLAY HINT, never a path (INPUT-VALIDATION)
    std::uint32_t block_count = 0;
    std::uint32_t block_symbols = 0;   // k per block
    std::uint32_t symbol_size = 0;
    std::uint32_t session_id = 0;
    ContentHint content;

    // Hard bounds, all enforced before any allocation derived from them.
    static constexpr std::uint64_t kMaxFileSize = 4ULL * 1024 * 1024 * 1024;
    static constexpr std::uint32_t kMaxBlockCount = 1u << 24;
    static constexpr std::size_t kMaxNameLength = 255;

    [[nodiscard]] Status Validate() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> Encode() const;
    [[nodiscard]] static Result<FileManifest> Decode(std::span<const std::uint8_t> in);
};

// Strips a received name down to something safe to use. Sanitises rather than merely
// validating: rejecting is fine too, but we must never pass an attacker's string through
// to the filesystem (THREAT-MODEL T3).
[[nodiscard]] std::string SanitiseFileName(std::string_view raw);

// ---------------------------------------------------------------- transmitter

// Source symbols of `block` that lie ENTIRELY past the end of the file: [first, block_symbols).
// Both ends derive this from the manifest alone, so these symbols are never transmitted -- the
// receiver primes them as known zeros instead (ADR-0015). Returns block_symbols when none are.
[[nodiscard]] std::uint32_t FirstPaddingSymbol(const FileManifest& m, std::uint32_t block) noexcept;

// Balanced block size for a file of `total_symbols` symbols: as FEW blocks as the decoder bound
// allows, all the same size.
//
// Fewer, larger blocks are faster on a lossy channel. With no back channel the transmitter cannot
// know which blocks a receiver has finished, so it keeps cycling all of them and the transfer
// ends when the UNLUCKIEST block completes. The spread in what each block needs shrinks as
// 1/sqrt(k), so the tail it costs shrinks with block size (F49). Balanced rather than greedy so the
// last block is not a stub carrying mostly padding.
[[nodiscard]] std::uint32_t AutoBlockSymbols(std::uint64_t total_symbols) noexcept;

class FileTransmitter {
  public:
    // `payload` is the original file bytes. Block size is bounded so the fountain decoder's
    // working set and per-symbol cost stay bounded regardless of file size.
    //
    // `block_symbols` = 0 chooses `AutoBlockSymbols`. `manifest_interval` = 0 chooses
    // `AutoManifestInterval`.
    static Result<FileTransmitter> Create(std::span<const std::uint8_t> payload,
                                          std::string file_name, std::uint32_t session_id,
                                          std::uint32_t symbol_size = 512,
                                          std::uint32_t block_symbols = 0,
                                          std::uint32_t manifest_interval = 0);

    // One manifest frame every this many display states, the first at sequence 0.
    //
    // The receiver cannot finish without the manifest but CAN use every symbol it reads before
    // the manifest arrives (`ReceiveSession` buffers them), so the interval only bounds how long
    // a receiver that missed every manifest waits at the end -- it costs no symbols. Each manifest
    // frame does cost one display state, so the interval grows with the transfer: an eighth of
    // the symbols needed, clamped to [8, 32], which is 3-12.5% of display states.
    //
    // It was half, clamped to [16, 128], until the first hand-held transfer (F59). Where only one
    // payload in twenty decodes, a manifest every 32 states at 7.5 states/s arrives on average a
    // minute and a half apart -- and until it does the person sees 0% however much has been read.
    // That run sat at 0% for 2.5 minutes holding 98% of the file. What a manifest costs is a
    // display state; what a missed one costs is the person giving up.
    [[nodiscard]] static std::uint32_t AutoManifestInterval(std::uint64_t symbols_needed) noexcept;

    [[nodiscard]] const FileManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] std::uint32_t manifest_interval() const noexcept { return manifest_interval_; }

    // Adds what the payload is (ContentHint) to every manifest frame from now on.
    // Fails without changing the manifest if the extended manifest cannot fit one symbol.
    [[nodiscard]] Status SetContentHint(const ContentHint& hint);

    // Produces the next frame payload in the transmission schedule: a manifest frame every
    // `manifest_interval()` states, otherwise a fountain symbol, round-robining over blocks so a
    // receiver joining late makes progress on every block rather than waiting for the schedule
    // to come around.
    //
    // `data` is always exactly `symbol_size` bytes, so every frame fills the optical payload. For
    // a manifest frame the manifest occupies the first `header.payload_bytes` bytes and the rest
    // is pseudorandom filler -- never zeros, which render as a near-black frame and confound every
    // optical measurement taken across it (F15, F39). `header.payload_crc` always covers the first
    // `header.payload_bytes` bytes.
    struct FramePayload {
        FrameHeader header;
        std::vector<std::uint8_t> data;
    };
    [[nodiscard]] FramePayload NextFrame();

    [[nodiscard]] std::uint64_t frames_generated() const noexcept { return counter_; }

  private:
    FileTransmitter(FileManifest m, std::vector<FountainEncoder> enc,
                    std::vector<std::uint32_t> first_padding, std::uint32_t manifest_interval);

    FileManifest manifest_;
    std::vector<std::uint8_t> manifest_bytes_;
    std::vector<FountainEncoder> encoders_;
    std::vector<std::uint32_t> first_padding_;  // per block, see FirstPaddingSymbol
    std::vector<std::uint32_t> next_esi_;       // per block
    std::uint32_t manifest_interval_;
    std::uint64_t counter_ = 0;
    std::uint64_t data_frames_ = 0;
};

// ---------------------------------------------------------------- receiver

class FileReceiver {
  public:
    // Bounds the eagerly allocated decoder working set, including matrices and per-block
    // overhead. This is a receiver resource limit, separate from the protocol's file-size bound.
    static constexpr std::uint64_t kMaxWorkingSetBytes = 512ULL * 1024 * 1024;
    [[nodiscard]] static Status ValidateWorkingSet(const FileManifest& m) noexcept;
    static Result<FileReceiver> Create(const FileManifest& m);

    [[nodiscard]] const FileManifest& manifest() const noexcept { return manifest_; }

    // Feed one successfully decoded frame. Duplicates and out-of-order arrivals are
    // tolerated by construction. Manifest frames are refused here -- they are not symbols.
    // Returns true once every block is recovered.
    bool Ingest(const FrameHeader& h, std::span<const std::uint8_t> data);

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::uint32_t blocks_complete() const noexcept;
    [[nodiscard]] double progress() const noexcept;

    // Total symbols ingested across all blocks, and the resulting reception overhead --
    // the Rfountain term the goodput model needs and EXP-012 measures. Symbols arriving for a
    // block that has already completed still count: the channel delivered them and they bought
    // nothing, which is the block-tail cost AutoBlockSymbols exists to shrink.
    [[nodiscard]] std::uint64_t symbols_ingested() const noexcept { return ingested_; }
    [[nodiscard]] std::uint64_t symbols_needed() const noexcept { return needed_; }
    [[nodiscard]] double overhead() const noexcept;

    // Reassembles and VERIFIES. A hash mismatch is a hard failure that returns
    // kHashMismatch and yields no data -- never a partial or unverified result.
    [[nodiscard]] Result<std::vector<std::uint8_t>> Finish();

  private:
    explicit FileReceiver(const FileManifest& m) : manifest_(m) {}

    FileManifest manifest_;
    std::vector<FountainDecoder> decoders_;
    std::uint64_t ingested_ = 0;
    std::uint64_t needed_ = 0;  // source symbols not primed: what the channel must deliver
};

}  // namespace fileflow
