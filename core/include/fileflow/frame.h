// Optical frame header — the self-describing part of every display state.
//
// HARD REQUIREMENT (docs/research/android-display-pipeline.md): presentation confirmation
// may be unreliable, and camera timestamps may be uncorrelated with display time on
// devices reporting SENSOR_INFO_TIMESTAMP_SOURCE = UNKNOWN. The receiver must therefore
// determine WHICH DISPLAY STATE it is looking at from frame content alone. Hence a
// sequence number and phase indicator in EVERY frame, never inferred from the
// transmitter's intended schedule.
//
// The header is the single point of failure: lose it and the frame is unusable even if
// every payload cell decoded perfectly. It gets M0 modulation regardless of payload mode,
// a very low code rate, and RS protection. `H` appears directly in the goodput model.
#pragma once

#include <fileflow/gf256.h>
#include <fileflow/result.h>

#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

inline constexpr std::uint8_t kFrameMagic = 0xF1;
// v2 (ADR-0015): dense GF(2) fountain rows, in-band manifest frames, and the intra-frame code
// rate carried in every header. A v1 receiver must refuse v2 rather than misparse it -- its LT
// decoder would ingest v2 repair symbols against the wrong rows and fail only at the hash.
inline constexpr std::uint8_t kProtocolVersion = 2;

enum class ModulationProfile : std::uint8_t {
    kM0BinaryLuminance = 0,
    kM1Differential = 1,
    kM2FourLevel = 2,
    kM3FourColor = 3,
    kM4MixedFrame = 4,
    // Every payload cell's red, green and blue are three independent bits: eight colours, three
    // bits a cell where M0 carries one. The header band stays M0 -- black and white read the same
    // in any colour space -- so a receiver learns the profile before it needs it (F62).
    kM5EightColor = 5,
};

struct FrameHeader {
    std::uint8_t version = kProtocolVersion;
    std::uint32_t session_id = 0;
    std::uint32_t sequence = 0;              // display state index
    ModulationProfile profile = ModulationProfile::kM0BinaryLuminance;
    std::uint8_t phase = 0;                  // cycling indicator for mixture detection
    std::uint32_t block_id = 0;              // fountain block
    std::uint32_t esi = 0;                   // fountain encoding symbol id
    std::uint32_t payload_bytes = 0;         // bounded before any allocation
    std::uint8_t flags = 0;

    // Reed-Solomon parity bytes per 255-byte intra-frame codeword; 0 = no intra-frame FEC.
    //
    // In the header because the receiver needs it BEFORE it can read anything else in the frame,
    // including the manifest: the payload of a manifest frame is FEC-protected like any other.
    // Without it the code rate was a convention both ends had to share out of band, and a
    // receiver built for nsym = 32 read an nsym = 24 transmitter as garbage (ADR-0015).
    std::uint8_t fec_nsym = 0;

    // CRC-32 over the frame's PAYLOAD bytes.
    //
    // Without this the fountain layer happily ingests corrupted symbols -- an erasure code
    // cannot tell a damaged symbol from a good one, so the damage propagates through
    // peeling and surfaces only as a final hash mismatch, with no way to attribute it.
    // A frame failing this check is a FRAME ERASURE and must never be ingested
    // (PROTOCOL-SPEC: "never pass through best-effort data").
    // Caught by EndToEnd.OcclusionProducesErasuresAndStillVerifies.
    std::uint32_t payload_crc = 0;

    static constexpr std::uint8_t kFlagLastBlock = 0x01;
    // This frame's payload is the session MANIFEST (`FileManifest::Encode`), not a fountain
    // symbol: its first `payload_bytes` bytes are the manifest and `block_id`/`esi` mean nothing.
    // A receiver must never hand such a frame to the fountain layer (ADR-0015).
    static constexpr std::uint8_t kFlagManifest = 0x02;

    // Serialised size before FEC. Keep in sync with Encode/Decode.
    static constexpr std::size_t kPlainSize = 1 + 1 + 4 + 4 + 1 + 1 + 4 + 4 + 4 + 1 + 1 + 4 + 4;

    [[nodiscard]] bool is_manifest() const noexcept { return (flags & kFlagManifest) != 0; }

    // Hard bound: a frame cannot carry more payload than the grid has cells.
    static constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024;

    [[nodiscard]] std::vector<std::uint8_t> EncodePlain() const;

    // Parses attacker-controlled bytes. Every field is bounds-checked and the CRC is
    // verified before any value is trusted (docs/security/INPUT-VALIDATION.md).
    [[nodiscard]] static Result<FrameHeader> DecodePlain(std::span<const std::uint8_t> in);
};

// Header codec with Reed-Solomon protection. Deliberately heavy: the header is a tiny
// fraction of total cells, so over-protecting it is cheap insurance.
class HeaderCodec {
  public:
    // nsym parity symbols. Default corrects 16 symbol errors in the header.
    explicit HeaderCodec(std::size_t nsym = 32) : rs_(nsym), nsym_(nsym) {}

    [[nodiscard]] std::size_t coded_size() const noexcept {
        return FrameHeader::kPlainSize + nsym_;
    }

    [[nodiscard]] Result<std::vector<std::uint8_t>> Encode(const FrameHeader& h) const;

    // `coded` is modified in place by RS correction.
    //
    // `erasures` are byte positions the demodulator could not read (`DemodulateHeader`). They cost
    // half what a blind error costs, and the header is the measured bottleneck on hardware (F36:
    // H = 0.13-0.38), so passing them is the cheapest gain in the receive chain (F50). If decoding
    // WITH them fails, decoding without them is tried too: an erasure flag on a byte that was in
    // fact right spends budget for nothing, and the CRC guards both attempts against a
    // miscorrection, so the second try can only add successes.
    [[nodiscard]] Result<FrameHeader> Decode(std::span<std::uint8_t> coded,
                                             std::span<const std::size_t> erasures = {}) const;

  private:
    ReedSolomon rs_;
    std::size_t nsym_;
};

}  // namespace fileflow
