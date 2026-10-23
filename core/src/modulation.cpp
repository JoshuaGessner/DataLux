#include <fileflow/modulation.h>

#include <algorithm>
#include <cmath>

namespace fileflow {
namespace {

// Confidence from distance to the decision threshold, normalised by the local noise
// estimate. A cell sampled near the boundary and one far from it carry very different
// information, and a hard-decision decoder throws that difference away.
Llr QuantiseLlr(double sample, const PhotometricRef& ref) {
    const double sigma = std::max(ref.noise_sigma, 1e-6);
    const double d = (ref.threshold() - sample) / sigma;  // >0 => closer to dark => bit 0

    // Scale ADAPTIVELY, against the distance a clean cell would sit at.
    //
    // A fixed scale of 32 mapped only d in [0,4] onto the whole int8 range, so on any decent
    // channel every cell clamped to +/-127 and the magnitude carried no information at all --
    // measured at 100% of cells in the top band across noise amplitudes from 0 to 120
    // (finding F19). Saturation at high SNR is not itself wrong (a certain cell IS certain),
    // but collapsing every distinguishable confidence into one value throws away the gradation
    // the FEC layer is supposed to exploit.
    //
    // Normalising by the clean-cell distance makes |llr| mean "fraction of the way to an
    // unambiguous decision": 127 for a textbook cell, ~64 halfway, ~0 at the threshold. That
    // is relative to THIS frame's measured separation and noise, so it stays meaningful on
    // good and bad channels alike -- and it is what an erasure-marking threshold needs.
    const double clean_distance = std::max(ref.separation() / (2.0 * sigma), 1.0);
    const double scaled = 127.0 * d / clean_distance;

    const double clamped = std::clamp(scaled, -127.0, 127.0);
    auto v = static_cast<Llr>(std::lround(clamped));
    if (v == kLlrErasure) v = 1;  // reserve exactly 0 for "erased"
    return v;
}

}  // namespace

void PackBits(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* bits_out) {
    bits_out->clear();
    bits_out->reserve(bytes.size() * 8);
    for (std::uint8_t b : bytes) {
        // `(b >> i) & 1` is int-typed after promotion; mixing in an unsigned literal made it a
        // signed-to-unsigned conversion. Kept signed and narrowed explicitly instead.
        for (int i = 7; i >= 0; --i) {
            bits_out->push_back(static_cast<std::uint8_t>((b >> i) & 1));
        }
    }
}

void UnpackBits(std::span<const std::uint8_t> bits, std::vector<std::uint8_t>* bytes_out) {
    bytes_out->assign(bits.size() / 8, 0);
    for (std::size_t i = 0; i < bytes_out->size(); ++i) {
        std::uint8_t v = 0;
        for (std::size_t j = 0; j < 8; ++j) {
            v = static_cast<std::uint8_t>((static_cast<unsigned>(v) << 1U) |
                                      (static_cast<unsigned>(bits[i * 8 + j]) & 1U));
        }
        (*bytes_out)[i] = v;
    }
}

Status M0Modulator::Render(std::span<const std::uint8_t> header_coded,
                           std::span<const std::uint8_t> payload_coded,
                           CellMatrix* out) const {
    const GridGeometry& g = layout_->geometry();
    if (out->cols() != g.cols || out->rows() != g.rows) return Error::kGridMismatch;

    const auto& hdr_cells = layout_->header_cells();
    const auto& pay_cells = layout_->payload_cells();

    if (header_coded.size() * 8 > hdr_cells.size()) return Error::kValueOutOfRange;
    if (payload_coded.size() * 8 > pay_cells.size()) return Error::kValueOutOfRange;

    // --- Structural cells ---
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        for (std::uint32_t c = 0; c < g.cols; ++c) {
            switch (layout_->role(c, r)) {
                case CellRole::kMarker:
                    out->set(c, r, layout_->MarkerValue(c, r));
                    break;
                case CellRole::kPilot:
                case CellRole::kColorPilot:
                    out->set(c, r, layout_->PilotValue(c, r));
                    break;
                case CellRole::kTimingTrack:
                    // Alternating track: gives the receiver a known spatial frequency for
                    // grid registration and a phase reference.
                    out->set(c, r, (c % 2 == 0) ? kLevelBright : kLevelDark);
                    break;
                case CellRole::kBoundary:
                    // Always bright, every frame, regardless of payload. "Persistent" is the
                    // point: the detector can track the screen across frames without ever
                    // re-acquiring, and a frame whose payload decode fails still contributes
                    // a geometry update.
                    out->set(c, r, kLevelBright);
                    break;
                case CellRole::kGuard:
                    out->set(c, r, kLevelDark);
                    break;
                default:
                    break;
            }
        }
    }

    // --- Header bits (replicated to fill the band, improving H) ---
    std::vector<std::uint8_t> hbits;
    PackBits(header_coded, &hbits);
    if (!hbits.empty()) {
        for (std::size_t i = 0; i < hdr_cells.size(); ++i) {
            const std::uint8_t bit = hbits[i % hbits.size()];
            out->set_flat(hdr_cells[i], bit ? kLevelBright : kLevelDark);
        }
    }

    // --- Payload bits ---
    std::vector<std::uint8_t> pbits;
    PackBits(payload_coded, &pbits);
    for (std::size_t i = 0; i < pay_cells.size(); ++i) {
        const std::uint8_t bit = (i < pbits.size()) ? pbits[i] : 0U;
        out->set_flat(pay_cells[i], bit ? kLevelBright : kLevelDark);
    }

    return Status::Ok();
}

PhotometricRef M0Modulator::EstimateReference(std::span<const double> samples) const {
    const GridGeometry& g = layout_->geometry();
    PhotometricRef ref;

    std::vector<double> dark_vals;
    std::vector<double> bright_vals;

    auto consider = [&](std::uint32_t idx) {
        if (idx >= samples.size()) return;
        const std::uint32_t c = idx % g.cols;
        const std::uint32_t r = idx / g.cols;
        if (layout_->PilotValue(c, r) == kLevelBright) {
            bright_vals.push_back(samples[idx]);
        } else {
            dark_vals.push_back(samples[idx]);
        }
    };
    for (std::uint32_t idx : layout_->pilot_cells()) consider(idx);

    auto median = [](std::vector<double>& v) {
        if (v.empty()) return 0.0;
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
        return v[mid];
    };
    auto stdev = [](const std::vector<double>& v, double mean) {
        if (v.size() < 2) return 8.0;
        double acc = 0.0;
        for (double x : v) acc += (x - mean) * (x - mean);
        return std::sqrt(acc / static_cast<double>(v.size() - 1));
    };

    if (!dark_vals.empty()) ref.dark = median(dark_vals);
    if (!bright_vals.empty()) ref.bright = median(bright_vals);

    const double sd = stdev(dark_vals, ref.dark);
    const double sb = stdev(bright_vals, ref.bright);
    ref.noise_sigma = std::max(1.0, 0.5 * (sd + sb));

    // Degenerate separation means the region is saturated or washed out. Rather than
    // "correcting" it, the caller should treat the frame as unusable -- see the erasure
    // discipline in DATA-FLOW.md.
    if (ref.separation() < 1.0) {
        ref.dark = 0.0;
        ref.bright = 255.0;
        ref.noise_sigma = 1e6;  // drives every LLR to an erasure
    }
    return ref;
}

Result<std::vector<std::uint8_t>> M0Modulator::DemodulateHeader(
    std::span<const double> samples, const PhotometricRef& ref, std::size_t coded_bytes,
    std::vector<std::size_t>* erased_bytes) const {
    const auto& hdr_cells = layout_->header_cells();
    if (hdr_cells.empty() || coded_bytes == 0) return Error::kInternal;

    // The header codeword is replicated across the band; combine the copies before RS
    // correction. Cheap, and it materially improves H on a noisy channel.
    const std::size_t period = coded_bytes * 8;
    if (period > hdr_cells.size()) return Error::kValueOutOfRange;

    // One readable copy at full confidence is worth 1.0; a tally below this is not a decision.
    // Half a clean copy: a lone marginal cell, or two clean copies that disagree, erase rather
    // than guess -- an erasure costs the RS layer one unit, a wrong guess two.
    constexpr double kMinConfidence = 0.5;
    const double half_sep = std::max(0.5 * ref.separation(), 1e-6);

    std::vector<double> votes(period, 0.0);

    for (std::size_t i = 0; i < hdr_cells.size(); ++i) {
        const std::uint32_t idx = hdr_cells[i];
        if (idx >= samples.size()) continue;
        // An ERASED cell abstains. It must not vote.
        //
        // NaN is the erasure convention, and `NaN > threshold` is false, so without this test
        // every unreadable cell cast a confident DARK vote. That is the worst possible failure
        // mode for a majority vote: erasures are not neutral noise, they all land on the same
        // side, so the tally is biased by exactly the cells that carry no information. On the
        // first above-cliff capture 76% of cells were erased, which buried the 24% that were
        // real and held H at 0.026 while the header band itself was measurably healthy (F46).
        if (std::isnan(samples[idx])) continue;
        votes[i % period] += std::clamp((samples[idx] - ref.threshold()) / half_sep, -1.0, 1.0);
    }

    std::vector<std::uint8_t> bits(period, 0);
    for (std::size_t i = 0; i < period; ++i) bits[i] = votes[i] > 0.0 ? 1U : 0U;

    if (erased_bytes != nullptr) {
        erased_bytes->clear();
        for (std::size_t byte = 0; byte < coded_bytes; ++byte) {
            for (std::size_t b = 0; b < 8; ++b) {
                if (std::abs(votes[byte * 8 + b]) < kMinConfidence) {
                    erased_bytes->push_back(byte);
                    break;
                }
            }
        }
    }

    std::vector<std::uint8_t> bytes;
    UnpackBits(bits, &bytes);
    return bytes;
}

void M0Modulator::DemodulatePayload(std::span<const double> samples, const PhotometricRef& ref,
                                    SoftSymbolBuffer* out) const {
    const auto& pay_cells = layout_->payload_cells();
    out->resize(pay_cells.size());

    for (std::size_t i = 0; i < pay_cells.size(); ++i) {
        const std::uint32_t idx = pay_cells[i];
        if (idx >= samples.size()) {
            out->symbols[i] = SoftSymbol{kLlrErasure, true};
            continue;
        }
        const double v = samples[idx];
        // A NaN sample means the cell fell outside the rectified image, was occluded, or
        // sat in a rolling-shutter transition band. Erasure, NOT a guess.
        if (std::isnan(v)) {
            out->symbols[i] = SoftSymbol{kLlrErasure, true};
            continue;
        }
        out->symbols[i] = SoftSymbol{QuantiseLlr(v, ref), false};
    }
}

HardDecision HardDecide(const SoftSymbolBuffer& soft, std::size_t want_bytes, Llr erase_below) {
    HardDecision hd;
    const std::size_t want_bits = want_bytes * 8;
    std::vector<std::uint8_t> bits(want_bits, 0);

    // Tracks which BYTES are contaminated, not just how many cells were lost. Reed-Solomon
    // needs positions: an erasure whose location is known costs half what a blind error costs
    // (ReedSolomon::Decode). Everything upstream -- NaN samples, the erased flag, the
    // photometric residual check -- exists to produce exactly this information, and returning
    // only a count would throw it away at the last step.
    std::vector<bool> byte_erased(want_bytes, false);

    for (std::size_t i = 0; i < want_bits; ++i) {
        // Symbols we never received are ERASURES, not zeros.
        //
        // The loop used to stop at soft.symbols.size(), leaving the remaining bits at 0 with no
        // erasure flag -- handing the FEC decoder fabricated data that looks perfectly valid.
        // That is exactly the failure the NaN/erasure convention exists to prevent, and it
        // would surface only when a frame arrived short, which is when it hurts most.
        if (i >= soft.symbols.size()) {
            ++hd.erasures;
            bits[i] = 0;
            byte_erased[i / 8] = true;
            continue;
        }
        const SoftSymbol& s = soft.symbols[i];
        // Confidence is |llr|. Anything at or below the threshold is a decision we do not
        // trust, and an untrusted decision is cheaper declared than guessed.
        const int confidence = s.llr < 0 ? -static_cast<int>(s.llr) : static_cast<int>(s.llr);
        if (s.erased || s.llr == kLlrErasure || confidence < static_cast<int>(erase_below)) {
            ++hd.erasures;
            bits[i] = 0;
            byte_erased[i / 8] = true;
        } else {
            // llr > 0 => closer to dark => bit 0
            bits[i] = s.llr > 0 ? 0U : 1U;
        }
    }
    UnpackBits(bits, &hd.bytes);

    hd.erased_bytes.reserve(hd.erasures / 8 + 1);
    for (std::size_t b = 0; b < want_bytes; ++b) {
        if (byte_erased[b]) hd.erased_bytes.push_back(b);
    }
    return hd;
}

}  // namespace fileflow
