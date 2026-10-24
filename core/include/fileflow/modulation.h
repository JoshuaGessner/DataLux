// M0 binary luminance modulation and SOFT demodulation.
//
// The defining property of this layer (docs/architecture/DATA-FLOW.md): the demodulator
// emits LOG-LIKELIHOOD RATIOS AND ERASURE FLAGS, never hard bits. Any stage that
// thresholds to a hard decision destroys information permanently, and most codes correct
// roughly twice as many erasures as errors -- so a cell we KNOW is untrustworthy is worth
// far more marked as an erasure than guessed at.
#pragma once

#include <fileflow/frame.h>
#include <fileflow/grid.h>
#include <fileflow/result.h>

#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

inline constexpr std::uint8_t kLevelDark = 0;
inline constexpr std::uint8_t kLevelBright = 255;

// Quantised LLR. Positive => bit is more likely 0 (dark); negative => more likely 1.
// 8-bit quantisation is provisional -- whether it costs measurable coding gain is a
// secondary question of EXP-011 (OQ-025).
using Llr = std::int8_t;
inline constexpr Llr kLlrErasure = 0;
inline constexpr Llr kLlrMax = 127;

struct SoftSymbol {
    Llr llr = kLlrErasure;
    bool erased = true;
};

// The convergence point of the CPU and GPU capture paths (ADR-0005). Both samplers MUST
// produce identical contents here -- an equivalence test between them is mandatory in CI,
// because it is the only thing preventing the two paths from silently diverging.
struct SoftSymbolBuffer {
    std::vector<SoftSymbol> symbols;  // indexed by payload-cell order

    [[nodiscard]] std::size_t size() const noexcept { return symbols.size(); }
    void resize(std::size_t n) { symbols.assign(n, SoftSymbol{}); }
};

// Photometric reference for one region, derived from pilot cells.
struct PhotometricRef {
    double dark = 0.0;
    double bright = 255.0;
    double noise_sigma = 8.0;  // per-region noise estimate

    [[nodiscard]] double threshold() const noexcept { return 0.5 * (dark + bright); }
    [[nodiscard]] double separation() const noexcept { return bright - dark; }
};

class M0Modulator {
  public:
    explicit M0Modulator(const FrameLayout& layout) : layout_(&layout) {}

    // Writes markers, pilots, timing tracks, guards, header and payload into `out`.
    // `header_bits` and `payload_bits` are bit-packed MSB-first.
    [[nodiscard]] Status Render(std::span<const std::uint8_t> header_coded,
                                std::span<const std::uint8_t> payload_coded,
                                CellMatrix* out) const;

    [[nodiscard]] std::size_t payload_capacity_bytes() const noexcept {
        return layout_->payload_cells().size() / 8;
    }
    [[nodiscard]] std::size_t header_capacity_bytes() const noexcept {
        return layout_->header_cells().size() / 8;
    }

    // Demodulation of the header from sampled cell values.
    // `coded_bytes` is HeaderCodec::coded_size() -- known to both sides, so the replicated
    // copies in the header band can be combined before RS correction.
    //
    // Copies are combined by CONFIDENCE, not by count: each readable copy votes its distance from
    // the threshold in half-separations, clipped to +/-1, so a cell sitting on the threshold
    // barely moves the tally and a clean one moves it fully. Erased (NaN) copies abstain (F46).
    //
    // If `erased_bytes` is given, it receives the byte positions holding a bit whose combined
    // vote is too weak to call -- including bits for which every copy was erased, which a vote
    // count would silently have called DARK. Those positions go to `HeaderCodec::Decode` as
    // erasures, at half the RS budget of the errors they would otherwise be (F50).
    [[nodiscard]] Result<std::vector<std::uint8_t>> DemodulateHeader(
        std::span<const double> samples, const PhotometricRef& ref, std::size_t coded_bytes,
        std::vector<std::size_t>* erased_bytes = nullptr) const;

    // SOFT demodulation of the payload. `samples` is one value per grid cell, in flat
    // row-major order, already photometrically normalised.
    void DemodulatePayload(std::span<const double> samples, const PhotometricRef& ref,
                           SoftSymbolBuffer* out) const;

    // Estimate dark/bright references and noise from the pilot lattice. This is the
    // interpolation advantage of layout Candidate B -- pilots are distributed through the
    // payload area, so the illumination field is interpolated, not extrapolated.
    [[nodiscard]] PhotometricRef EstimateReference(std::span<const double> samples) const;

  private:
    const FrameLayout* layout_;
};

// Payload bits per cell under a profile: 1 for M0, 3 for M5. 0 = not one this build carries.
[[nodiscard]] std::size_t BitsPerCell(ModulationProfile p) noexcept;

// Payload bytes one frame carries under a profile, before intra-frame FEC.
[[nodiscard]] std::size_t PayloadCapacityBytes(const FrameLayout& layout,
                                               ModulationProfile p) noexcept;

// The display value of the colour whose red, green and blue are each fully on or off.
//
// The LOW THREE BITS are red, green and blue, and a renderer lights a channel when its bit is set.
// Under that one rule M0's two levels keep their meaning -- 0 has no bit set (black) and 255 has
// all of them (white) -- so a single renderer draws either profile.
[[nodiscard]] constexpr std::uint8_t ColorValue(unsigned rgb3) noexcept {
    return static_cast<std::uint8_t>(rgb3 & 7U);
}
[[nodiscard]] constexpr bool ColorChannelOn(std::uint8_t value, int channel) noexcept {
    // Channel 0 red, 1 green, 2 blue.
    return ((static_cast<unsigned>(value) >> static_cast<unsigned>(2 - channel)) & 1U) != 0;
}

// What the colour demodulator saw, per frame: the evidence for whether eight colours are
// separable on a given pair of phones, which is the question M5 lives or dies on.
struct ColorDiagnostics {
    std::size_t cells = 0;          // payload cells with a colour sample
    std::size_t pilots = 0;         // colour pilots with a sample
    double centroid_spread = 0.0;   // smallest distance between two colour centroids, 0..255 units
    double noise_sigma = 0.0;       // per-channel spread of cells round their centroid
};

// M5: three bits a cell, one each on red, green and blue (kM5EightColor). And M3: two bits a
// cell in four colours (kM3FourColor) -- black, cyan, magenta and yellow, any two of which differ
// in TWO channels where the nearest of the eight differ in one. Two-thirds the data, for cameras
// that cannot tell eight apart at a practical distance (F66).
//
// The header and every structural cell are drawn exactly as M0 draws them -- the header must be
// readable before the receiver knows the profile, and markers, pilots and the boundary are what it
// finds and normalises the screen by. Only payload cells and the reserved colour pilots change.
//
// Demodulation does not trust any colour model. Camera and panel disagree about every primary:
// the display's red leaks into the camera's green, white balance is whatever the locked camera
// left it at, and the chroma planes have a quarter the resolution. So the eight colours are
// LEARNED per frame: the colour pilots seed one centroid each, and a few rounds of k-means over the
// frame's own payload cells settle them where this camera actually sees them. Each bit's
// confidence is then the max-log likelihood ratio between the nearest colour with that bit set and
// the nearest without -- a soft symbol, like M0's, for the same erasure discipline downstream.
class ColorModulator {
  public:
    explicit ColorModulator(const FrameLayout& layout,
                            ModulationProfile profile = ModulationProfile::kM5EightColor);

    [[nodiscard]] Status Render(std::span<const std::uint8_t> header_coded,
                                std::span<const std::uint8_t> payload_coded,
                                CellMatrix* out) const;

    [[nodiscard]] std::size_t payload_capacity_bytes() const noexcept {
        return layout_->payload_cells().size() * bits_ / 8;
    }
    [[nodiscard]] std::size_t bits_per_cell() const noexcept { return bits_; }

    // `rgb` is three values per grid cell, interleaved r,g,b, flat row-major, each normalised so the
    // white pilots read 255 and the black ones 0 in that channel (FramePipeline). NaN = erased.
    // `out` receives bits_per_cell() soft bits per payload cell, in payload-bit order.
    void DemodulatePayload(std::span<const double> rgb, SoftSymbolBuffer* out,
                           ColorDiagnostics* diag = nullptr) const;

    // The palette index colour-pilot `i` (raster order among them) shows: each colour in turn.
    [[nodiscard]] unsigned PilotColor(std::size_t i) const noexcept {
        return static_cast<unsigned>(i % palette_.size());
    }

  private:
    const FrameLayout* layout_;
    std::vector<unsigned> palette_;  // palette index -> the colour's red/green/blue bits
    std::size_t bits_;
};

// Pack/unpack helpers. Bit order is MSB-first within each byte, fixed by the wire format.
void PackBits(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* bits_out);
void UnpackBits(std::span<const std::uint8_t> bits, std::vector<std::uint8_t>* bytes_out);

// Hard-decide a soft buffer into bytes, reporting how many cells were erased.
// Used until the payload FEC decoder lands (EXP-011 selects it); the soft information is
// preserved in SoftSymbolBuffer so that decoder can consume it directly.
struct HardDecision {
    std::vector<std::uint8_t> bytes;
    std::size_t erasures = 0;  // erased CELLS

    // Indices into `bytes` containing at least one erased cell.
    //
    // Reed-Solomon operates on bytes, so a single unreadable cell condemns its whole byte --
    // eight cells' worth of budget spent on one. That promotion is lossy and it is the price
    // of a byte-oriented code; a bit-level or non-binary code would not pay it (OQ-025,
    // EXP-011). The positions are nonetheless worth far more than the count: erasures with
    // known positions cost HALF what blind errors cost (see ReedSolomon::Decode).
    std::vector<std::size_t> erased_bytes;
};
// `erase_below` converts LOW-CONFIDENCE decisions into erasures.
//
// This is how an algebraic code exploits soft information. A cell with |llr| = 3 is barely
// distinguishable from noise; taken as a hard decision it is a coin flip, and a wrong one costs
// the Reed-Solomon layer TWO units of budget (an error must be located as well as corrected).
// Erasing it instead costs ONE. So marking marginal cells as erasures trades a cheap, certain
// cost for an expensive, probable one.
//
// There is an optimum and it is not at either extreme: erase too few and likely-wrong bits slip
// through at double price, erase too many and the erasure budget is exhausted by cells that
// were probably fine. 0 disables the mechanism, which is the A/B baseline.
//
// This is the first thing in the system that uses LLR MAGNITUDE rather than just its sign, so
// it is also the test of whether preserving soft confidence was worth anything at all.
[[nodiscard]] HardDecision HardDecide(const SoftSymbolBuffer& soft, std::size_t want_bytes,
                                      Llr erase_below = 0);

}  // namespace fileflow
