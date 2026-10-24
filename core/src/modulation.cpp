#include <fileflow/modulation.h>

#include <algorithm>
#include <array>
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

std::size_t BitsPerCell(ModulationProfile p) noexcept {
    switch (p) {
        case ModulationProfile::kM0BinaryLuminance: return 1;
        case ModulationProfile::kM5EightColor: return 3;
        default: return 0;
    }
}

std::size_t PayloadCapacityBytes(const FrameLayout& layout, ModulationProfile p) noexcept {
    return layout.payload_cells().size() * BitsPerCell(p) / 8;
}

Status ColorModulator::Render(std::span<const std::uint8_t> header_coded,
                              std::span<const std::uint8_t> payload_coded,
                              CellMatrix* out) const {
    const auto& pay_cells = layout_->payload_cells();
    if (payload_coded.size() * 8 > pay_cells.size() * 3) return Error::kValueOutOfRange;
    // Structure and header exactly as M0 draws them; payload filled below.
    FF_TRY(M0Modulator(*layout_).Render(header_coded, {}, out));

    const auto& pilots = layout_->color_pilot_cells();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        out->set_flat(pilots[i], ColorValue(PilotColor(i)));
    }

    std::vector<std::uint8_t> bits;
    PackBits(payload_coded, &bits);
    for (std::size_t i = 0; i < pay_cells.size(); ++i) {
        unsigned v = 0;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t j = i * 3 + k;  // first bit red, then green, then blue
            v = (v << 1U) | (j < bits.size() ? (bits[j] & 1U) : 0U);
        }
        out->set_flat(pay_cells[i], ColorValue(v));
    }
    return Status::Ok();
}

void ColorModulator::DemodulatePayload(std::span<const double> rgb, SoftSymbolBuffer* out,
                                       ColorDiagnostics* diag) const {
    using Vec = std::array<double, 3>;
    const auto& pay_cells = layout_->payload_cells();
    out->resize(pay_cells.size() * 3);  // every bit an erasure until proven otherwise
    const auto cells = static_cast<std::size_t>(layout_->geometry().cells());
    if (rgb.size() != cells * 3) return;

    auto read = [&rgb](std::uint32_t idx, Vec* v) {
        for (std::size_t k = 0; k < 3; ++k) {
            (*v)[k] = rgb[static_cast<std::size_t>(idx) * 3 + k];
            if (std::isnan((*v)[k])) return false;
        }
        return true;
    };
    auto dist2 = [](const Vec& a, const Vec& b) {
        double d = 0.0;
        for (std::size_t k = 0; k < 3; ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
        return d;
    };

    // Seeds: the ideal corners of the colour cube, replaced by what the pilots showed wherever a
    // pilot of that colour was read.
    std::array<Vec, 8> mu{};
    for (unsigned c = 0; c < 8; ++c) {
        for (int k = 0; k < 3; ++k) {
            mu[c][static_cast<std::size_t>(k)] = ColorChannelOn(ColorValue(c), k) ? 255.0 : 0.0;
        }
    }
    std::array<Vec, 8> pilot_sum{};
    std::array<std::size_t, 8> pilot_n{};
    const auto& pilots = layout_->color_pilot_cells();
    std::size_t pilots_read = 0;
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        Vec v{};
        if (!read(pilots[i], &v)) continue;
        const unsigned c = PilotColor(i);
        for (std::size_t k = 0; k < 3; ++k) pilot_sum[c][k] += v[k];
        ++pilot_n[c];
        ++pilots_read;
    }
    for (unsigned c = 0; c < 8; ++c) {
        if (pilot_n[c] == 0) continue;
        for (std::size_t k = 0; k < 3; ++k) {
            mu[c][k] = pilot_sum[c][k] / static_cast<double>(pilot_n[c]);
        }
    }

    std::vector<Vec> x;
    std::vector<std::size_t> slot;  // payload-cell index of each sample
    x.reserve(pay_cells.size());
    slot.reserve(pay_cells.size());
    for (std::size_t i = 0; i < pay_cells.size(); ++i) {
        Vec v{};
        if (pay_cells[i] >= cells || !read(pay_cells[i], &v)) continue;
        x.push_back(v);
        slot.push_back(i);
    }
    if (x.empty()) return;

    auto nearest = [&mu, &dist2](const Vec& v) {
        unsigned best = 0;
        double bd = dist2(v, mu[0]);
        for (unsigned c = 1; c < 8; ++c) {
            const double d = dist2(v, mu[c]);
            if (d < bd) {
                bd = d;
                best = c;
            }
        }
        return best;
    };

    // Settle the centroids where this frame's cells actually are. The pilots stay in every mean
    // with their KNOWN colour, which anchors each cluster's identity: k-means alone could swap two
    // colours and decode every bit of both wrong with complete confidence.
    constexpr int kRounds = 3;
    constexpr std::size_t kMinMembers = 16;
    for (int round = 0; round < kRounds; ++round) {
        std::array<Vec, 8> sum = pilot_sum;
        std::array<std::size_t, 8> n = pilot_n;
        for (const Vec& v : x) {
            const unsigned c = nearest(v);
            for (std::size_t k = 0; k < 3; ++k) sum[c][k] += v[k];
            ++n[c];
        }
        for (unsigned c = 0; c < 8; ++c) {
            if (n[c] < kMinMembers) continue;  // too few to move: keep the seed
            for (std::size_t k = 0; k < 3; ++k) mu[c][k] = sum[c][k] / static_cast<double>(n[c]);
        }
    }

    // Each bit's "clean" distance: how far, on average, flipping that bit moves a colour.
    std::array<double, 3> clean{};
    for (std::size_t j = 0; j < 3; ++j) {
        const unsigned mask = 4U >> j;
        double acc = 0.0;
        for (unsigned c = 0; c < 8; ++c) acc += dist2(mu[c], mu[c ^ mask]);
        clean[j] = std::max(acc / 8.0, 1.0);
    }

    double noise = 0.0;
    for (std::size_t s = 0; s < x.size(); ++s) {
        std::array<double, 8> d{};
        for (unsigned c = 0; c < 8; ++c) d[c] = dist2(x[s], mu[c]);
        noise += *std::min_element(d.begin(), d.end());
        for (std::size_t j = 0; j < 3; ++j) {
            const unsigned mask = 4U >> j;
            double d0 = 1e300;
            double d1 = 1e300;
            for (unsigned c = 0; c < 8; ++c) {
                double& slot_d = (c & mask) != 0 ? d1 : d0;
                slot_d = std::min(slot_d, d[c]);
            }
            // Max-log LLR, scaled like M0's: 127 at a textbook cell, 0 on the boundary, positive
            // for bit 0.
            const double scaled = std::clamp(127.0 * (d1 - d0) / clean[j], -127.0, 127.0);
            auto v = static_cast<Llr>(std::lround(scaled));
            if (v == kLlrErasure) v = 1;
            out->symbols[slot[s] * 3 + j] = SoftSymbol{v, false};
        }
    }

    if (diag != nullptr) {
        diag->cells = x.size();
        diag->pilots = pilots_read;
        diag->noise_sigma = std::sqrt(noise / (3.0 * static_cast<double>(x.size())));
        double spread = 1e300;
        for (unsigned a = 0; a < 8; ++a) {
            for (unsigned b = a + 1; b < 8; ++b) spread = std::min(spread, dist2(mu[a], mu[b]));
        }
        diag->centroid_spread = std::sqrt(spread);
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
