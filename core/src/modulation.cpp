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
        case ModulationProfile::kM3FourColor: return 2;
        case ModulationProfile::kM5EightColor: return 3;
        default: return 0;
    }
}

std::size_t PayloadCapacityBytes(const FrameLayout& layout, ModulationProfile p) noexcept {
    return layout.payload_cells().size() * BitsPerCell(p) / 8;
}

ColorModulator::ColorModulator(const FrameLayout& layout, ModulationProfile profile)
    : layout_(&layout) {
    if (profile == ModulationProfile::kM3FourColor) {
        palette_ = {0b000, 0b011, 0b101, 0b110};  // black, cyan, magenta, yellow
        bits_ = 2;
    } else {
        palette_ = {0, 1, 2, 3, 4, 5, 6, 7};
        bits_ = 3;
    }
}

Status ColorModulator::Render(std::span<const std::uint8_t> header_coded,
                              std::span<const std::uint8_t> payload_coded,
                              CellMatrix* out) const {
    const auto& pay_cells = layout_->payload_cells();
    if (payload_coded.size() * 8 > pay_cells.size() * bits_) return Error::kValueOutOfRange;
    // Structure and header exactly as M0 draws them; payload filled below.
    FF_TRY(M0Modulator(*layout_).Render(header_coded, {}, out));

    const auto& pilots = layout_->color_pilot_cells();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        out->set_flat(pilots[i], ColorValue(palette_[PilotColor(i)]));
    }

    std::vector<std::uint8_t> bits;
    PackBits(payload_coded, &bits);
    for (std::size_t i = 0; i < pay_cells.size(); ++i) {
        unsigned v = 0;
        for (std::size_t k = 0; k < bits_; ++k) {
            const std::size_t j = i * bits_ + k;  // M5: first bit red, then green, then blue
            v = (v << 1U) | (j < bits.size() ? (bits[j] & 1U) : 0U);
        }
        out->set_flat(pay_cells[i], ColorValue(palette_[v]));
    }
    return Status::Ok();
}

void ColorModulator::DemodulatePayload(std::span<const double> rgb, SoftSymbolBuffer* out,
                                       ColorDiagnostics* diag) const {
    using Vec = std::array<double, 3>;
    const auto& pay_cells = layout_->payload_cells();
    out->resize(pay_cells.size() * bits_);  // every bit an erasure until proven otherwise
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
    const auto n_colors = static_cast<unsigned>(palette_.size());
    for (unsigned c = 0; c < n_colors; ++c) {
        for (int k = 0; k < 3; ++k) {
            mu[c][static_cast<std::size_t>(k)] =
                ColorChannelOn(ColorValue(palette_[c]), k) ? 255.0 : 0.0;
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
    for (unsigned c = 0; c < n_colors; ++c) {
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

    auto nearest = [&mu, &dist2, n_colors](const Vec& v) {
        unsigned best = 0;
        double bd = dist2(v, mu[0]);
        for (unsigned c = 1; c < n_colors; ++c) {
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
        for (unsigned c = 0; c < n_colors; ++c) {
            if (n[c] < kMinMembers) continue;  // too few to move: keep the seed
            for (std::size_t k = 0; k < 3; ++k) mu[c][k] = sum[c][k] / static_cast<double>(n[c]);
        }
    }

    // ---- What a cell shows is partly its neighbours' ----
    //
    // The camera records colour at a quarter of luma's resolution and then demosaics and, with
    // digital zoom, resamples it, so every cell's colour carries a share of the cells around it.
    // On the Pixel 8 reading the S26 that cost a third of the colours' separation, and frame after
    // frame failed while the S26 read the Pixel fine (F65). It is not noise: it is a linear
    // function of colours this frame already shows. So it is LEARNED, like the colours, and
    // subtracted -- decision feedback, in two dimensions:
    //
    //     x - mu(own) = B4 (n4 - mu(own)) + B8 (n8 - mu(own))
    //
    // with n4 the mean colour of the four edge neighbours, n8 of the four corner ones, and B4, B8
    // 3x3 matrices fitted to this frame by least squares. Then each candidate colour is judged by
    // where it WOULD appear among those neighbours, not where it would appear alone.
    const GridGeometry g = layout_->geometry();
    std::vector<int> label(cells, -1);
    for (std::uint32_t idx = 0; idx < cells; ++idx) {
        Vec v{};
        if (read(idx, &v)) label[idx] = static_cast<int>(nearest(v));
    }
    // Neighbour means for one cell: [0] the edge four, [1] the corner four. False if it has none.
    auto neighbours = [&](std::uint32_t idx, std::array<Vec, 2>* n) {
        const auto c = static_cast<int>(idx % g.cols);
        const auto r = static_cast<int>(idx / g.cols);
        std::array<Vec, 2> sum{};
        std::array<int, 2> count{};
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int cc = c + dx;
                const int rr = r + dy;
                if (cc < 0 || rr < 0 || cc >= static_cast<int>(g.cols) || rr >= static_cast<int>(g.rows)) {
                    continue;
                }
                const int lab = label[static_cast<std::size_t>(rr) * g.cols + static_cast<std::size_t>(cc)];
                if (lab < 0) continue;
                const std::size_t group = (dx == 0 || dy == 0) ? 0 : 1;
                for (std::size_t k = 0; k < 3; ++k) sum[group][k] += mu[static_cast<std::size_t>(lab)][k];
                ++count[group];
            }
        }
        if (count[0] == 0 || count[1] == 0) return false;
        for (std::size_t gi = 0; gi < 2; ++gi) {
            for (std::size_t k = 0; k < 3; ++k) (*n)[gi][k] = sum[gi][k] / count[gi];
        }
        return true;
    };

    constexpr std::size_t kF = 6;  // features: edge-mean and corner-mean offsets, 3 channels each
    using Feat = std::array<double, kF>;
    auto features = [](const std::array<Vec, 2>& n, const Vec& own) {
        Feat f{};
        for (std::size_t k = 0; k < 3; ++k) {
            f[k] = n[0][k] - own[k];
            f[3 + k] = n[1][k] - own[k];
        }
        return f;
    };
    using Bleed = std::array<Feat, 3>;
    auto predict = [](const Bleed& b, const Vec& own, const Feat& f) {
        Vec p = own;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t k = 0; k < kF; ++k) p[i] += b[i][k] * f[k];
        }
        return p;
    };
    // Solves A X = B for X (A n x n, here 6x6 or 3x3), Gaussian elimination with partial
    // pivoting; false when A is singular -- a frame too uniform to fit, which then goes unfitted.
    auto solve = [](std::vector<std::vector<double>> a, std::vector<std::vector<double>> b,
                    std::vector<std::vector<double>>* sol) {
        const std::size_t n = a.size();
        const std::size_t m = b[0].size();
        for (std::size_t col = 0; col < n; ++col) {
            std::size_t piv = col;
            for (std::size_t r = col + 1; r < n; ++r) {
                if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
            }
            if (!(std::abs(a[piv][col]) > 1e-9)) return false;
            std::swap(a[piv], a[col]);
            std::swap(b[piv], b[col]);
            for (std::size_t r = 0; r < n; ++r) {
                if (r == col) continue;
                const double f = a[r][col] / a[col][col];
                for (std::size_t k = col; k < n; ++k) a[r][k] -= f * a[col][k];
                for (std::size_t k = 0; k < m; ++k) b[r][k] -= f * b[col][k];
            }
        }
        sol->assign(n, std::vector<double>(m));
        for (std::size_t r = 0; r < n; ++r) {
            for (std::size_t k = 0; k < m; ++k) (*sol)[r][k] = b[r][k] / a[r][r];
        }
        return true;
    };
    // The prediction mu_c + B4 (n4 - mu_c) + B8 (n8 - mu_c) splits into a part that depends only
    // on the candidate colour, (I - B4 - B8) mu_c, and one that depends only on the neighbours,
    // B4 n4 + B8 n8 -- so each cell costs one product, not one per candidate.
    auto own_part = [&mu, n_colors](const Bleed& b) {
        std::array<Vec, 8> q{};
        for (unsigned c = 0; c < n_colors; ++c) {
            for (std::size_t i = 0; i < 3; ++i) {
                double v = mu[c][i];
                for (std::size_t k = 0; k < 3; ++k) v -= (b[i][k] + b[i][3 + k]) * mu[c][k];
                q[c][i] = v;
            }
        }
        return q;
    };
    auto neighbour_part = [](const Bleed& b, const std::array<Vec, 2>& n) {
        Vec o{};
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t k = 0; k < 3; ++k) o[i] += b[i][k] * n[0][k] + b[i][3 + k] * n[1][k];
        }
        return o;
    };
    using Mat = std::array<std::array<double, 3>, 3>;
    // Squared distance under the weights w -- plain Euclidean, w the identity. Weighting by the
    // inverse residual covariance, so the sharp luma direction would count for more, was tried and
    // measured worse on real-camera-like frames: 11.9% of cells wrong against 3.5% under heavy
    // chroma blur (ffframe, F67). The pooled covariance is dominated by the cells already confused.
    auto metric = [](const Mat& w, const Vec& d) {
        double acc = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t k = 0; k < 3; ++k) acc += d[i] * w[i][k] * d[k];
        }
        return acc;
    };
    auto diff = [](const Vec& a, const Vec& b) { return Vec{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };

    std::vector<std::array<Vec, 2>> near(x.size());
    std::vector<bool> has_near(x.size(), false);
    Bleed bleed{};
    Mat w{};
    for (std::size_t i = 0; i < 3; ++i) w[i][i] = 1.0;
    double residual_var = 0.0;
    constexpr int kPasses = 2;
    for (int pass = 0; pass < kPasses; ++pass) {
        // Least squares for the bleed: (F^T F) B^T = F^T R.
        std::vector<std::vector<double>> ftf(kF, std::vector<double>(kF, 0.0));
        std::vector<std::vector<double>> ftr(kF, std::vector<double>(3, 0.0));
        std::size_t fitted = 0;
        for (std::size_t s = 0; s < x.size(); ++s) {
            has_near[s] = neighbours(pay_cells[slot[s]], &near[s]);
            if (!has_near[s]) continue;
            const Vec& own = mu[static_cast<std::size_t>(label[pay_cells[slot[s]]])];
            const Feat f = features(near[s], own);
            const Vec r = diff(x[s], own);
            for (std::size_t i = 0; i < kF; ++i) {
                for (std::size_t k = 0; k < kF; ++k) ftf[i][k] += f[i] * f[k];
                for (std::size_t k = 0; k < 3; ++k) ftr[i][k] += f[i] * r[k];
            }
            ++fitted;
        }
        bleed = Bleed{};
        std::vector<std::vector<double>> bt;
        if (fitted > 64 && solve(ftf, ftr, &bt)) {
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t k = 0; k < kF; ++k) bleed[i][k] = bt[k][i];
            }
        }
        // Residual spread once the bleed is accounted for: the noise the decisions face.
        Mat cov{};
        for (std::size_t s = 0; s < x.size(); ++s) {
            const Vec& own = mu[static_cast<std::size_t>(label[pay_cells[slot[s]]])];
            const Vec p = has_near[s] ? predict(bleed, own, features(near[s], own)) : own;
            const Vec e = diff(x[s], p);
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t k = 0; k < 3; ++k) cov[i][k] += e[i] * e[k];
            }
        }
        residual_var = 0.0;
        std::vector<std::vector<double>> cv(3, std::vector<double>(3));
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t k = 0; k < 3; ++k) cv[i][k] = cov[i][k] / static_cast<double>(x.size());
            cv[i][i] += 4.0;  // a floor: a perfectly clean frame must not divide by zero
            residual_var += cv[i][i];
        }
        // Re-decide every payload cell under the fitted model, for the next pass and the bits.
        const std::array<Vec, 8> q = own_part(bleed);
        for (std::size_t s = 0; s < x.size(); ++s) {
            const Vec o = has_near[s] ? neighbour_part(bleed, near[s]) : Vec{};
            unsigned best = 0;
            double bd = 1e300;
            for (unsigned c = 0; c < n_colors; ++c) {
                const Vec& base = has_near[s] ? q[c] : mu[c];
                const double d = metric(w, Vec{x[s][0] - base[0] - o[0], x[s][1] - base[1] - o[1],
                                               x[s][2] - base[2] - o[2]});
                if (d < bd) {
                    bd = d;
                    best = c;
                }
            }
            label[pay_cells[slot[s]]] = static_cast<int>(best);
        }
    }

    // Each bit's "clean" distance, in the same metric: how far flipping that bit moves a colour.
    std::array<double, 3> clean{};
    for (std::size_t j = 0; j < bits_; ++j) {
        const unsigned mask = 1U << (bits_ - 1 - j);
        double acc = 0.0;
        for (unsigned c = 0; c < n_colors; ++c) acc += metric(w, diff(mu[c], mu[c ^ mask]));
        clean[j] = std::max(acc / n_colors, 1e-6);
    }

    const std::array<Vec, 8> q = own_part(bleed);
    for (std::size_t s = 0; s < x.size(); ++s) {
        const Vec o = has_near[s] ? neighbour_part(bleed, near[s]) : Vec{};
        std::array<double, 8> d{};
        for (unsigned c = 0; c < n_colors; ++c) {
            const Vec& base = has_near[s] ? q[c] : mu[c];
            d[c] = metric(w, Vec{x[s][0] - base[0] - o[0], x[s][1] - base[1] - o[1],
                                 x[s][2] - base[2] - o[2]});
        }
        for (std::size_t j = 0; j < bits_; ++j) {
            const unsigned mask = 1U << (bits_ - 1 - j);
            double d0 = 1e300;
            double d1 = 1e300;
            for (unsigned c = 0; c < n_colors; ++c) {
                double& slot_d = (c & mask) != 0 ? d1 : d0;
                slot_d = std::min(slot_d, d[c]);
            }
            // Max-log LLR, scaled like M0's: 127 at a textbook cell, 0 on the boundary, positive
            // for bit 0.
            const double scaled = std::clamp(127.0 * (d1 - d0) / clean[j], -127.0, 127.0);
            auto v = static_cast<Llr>(std::lround(scaled));
            if (v == kLlrErasure) v = 1;
            out->symbols[slot[s] * bits_ + j] = SoftSymbol{v, false};
        }
    }

    if (diag != nullptr) {
        diag->cells = x.size();
        diag->pilots = pilots_read;
        // What remains once the bleed is subtracted: the noise the decisions actually face.
        diag->noise_sigma = std::sqrt(residual_var / 3.0);
        double spread = 1e300;
        for (unsigned a = 0; a < n_colors; ++a) {
            for (unsigned b = a + 1; b < n_colors; ++b) spread = std::min(spread, dist2(mu[a], mu[b]));
        }
        diag->centroid_spread = std::sqrt(spread);
        for (unsigned c = 0; c < n_colors; ++c) diag->centroids[c] = mu[c];
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
