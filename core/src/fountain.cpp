#include <fileflow/fountain.h>

#include <fileflow/bytes.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace fileflow {
namespace {

// The inner loop of both encoder and decoder. Written as a plain byte loop on purpose: clang and
// gcc vectorise it at -O2, and anything cleverer would be harder to audit for the same speed.
void XorInto(std::uint8_t* dst, const std::uint8_t* src, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) dst[i] ^= src[i];
}

}  // namespace

Status FountainParams::Validate() const noexcept {
    if (source_symbols == 0 || source_symbols > kMaxSourceSymbols) return Error::kValueOutOfRange;
    if (symbol_size == 0 || symbol_size > kMaxSymbolSize) return Error::kValueOutOfRange;

    // Checked before any allocation derived from these values (INPUT-VALIDATION SR-1).
    std::size_t total = 0;
    if (!CheckedMul(source_symbols, symbol_size, &total)) return Error::kSizeOverflow;
    if (total > (std::size_t{1} << 31)) return Error::kPayloadTooLarge;
    return Status::Ok();
}

// ---------------------------------------------------------------- rows

FountainRows::FountainRows(const FountainParams& p) noexcept
    : p_(p),
      words_((static_cast<std::size_t>(p.source_symbols) + 63) / 64),
      last_mask_(p.source_symbols % 64 == 0 ? ~std::uint64_t{0}
                                            : (std::uint64_t{1} << (p.source_symbols % 64)) - 1) {}

void FountainRows::Row(std::uint32_t esi, std::uint64_t* out) const noexcept {
    std::fill(out, out + words_, std::uint64_t{0});
    const std::uint32_t k = p_.source_symbols;
    if (k == 0) return;

    // Systematic prefix: symbol i IS source symbol i.
    if (esi < k) {
        out[esi / 64] = std::uint64_t{1} << (esi % 64);
        return;
    }

    // Mixing the session seed keeps two concurrent sessions from generating identical
    // symbol structure (THREAT-MODEL T8 session confusion).
    SplitMix64 rng((static_cast<std::uint64_t>(p_.session_seed) << 32) ^
                   (static_cast<std::uint64_t>(esi) * 0x9E3779B97F4A7C15ULL));
    bool any = false;
    for (std::size_t w = 0; w < words_; ++w) {
        out[w] = rng.Next();
        if (w + 1 == words_) out[w] &= last_mask_;
        any = any || out[w] != 0;
    }
    // An all-zero row carries nothing. Only reachable for tiny k (probability 2^-k), but a row
    // that can never help is a symbol slot wasted, so give it one deterministic neighbour.
    if (!any) {
        const std::uint32_t i = esi % k;
        out[i / 64] = std::uint64_t{1} << (i % 64);
    }
}

// ---------------------------------------------------------------- encoder

Result<FountainEncoder> FountainEncoder::Create(const FountainParams& p,
                                                std::span<const std::uint8_t> source) {
    FF_TRY(p.Validate());

    std::size_t total = 0;
    if (!CheckedMul(p.source_symbols, p.symbol_size, &total)) return Error::kSizeOverflow;
    if (source.size() > total) return Error::kLengthMismatch;

    std::vector<std::uint8_t> src(total, 0);
    std::copy(source.begin(), source.end(), src.begin());
    return FountainEncoder(p, std::move(src));
}

std::vector<std::uint8_t> FountainEncoder::Symbol(std::uint32_t esi) const {
    const std::size_t s = p_.symbol_size;
    std::vector<std::uint8_t> out(s, 0);
    if (esi < p_.source_symbols) {
        std::memcpy(out.data(), src_.data() + static_cast<std::size_t>(esi) * s, s);
        return out;
    }

    std::vector<std::uint64_t> row(rows_.words());
    rows_.Row(esi, row.data());
    for (std::size_t w = 0; w < row.size(); ++w) {
        std::uint64_t bits = row[w];
        while (bits != 0) {
            const auto idx = w * 64 + static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            XorInto(out.data(), src_.data() + idx * s, s);
        }
    }
    return out;
}

// ---------------------------------------------------------------- decoder

FountainDecoder::FountainDecoder(const FountainParams& p)
    : p_(p),
      rows_(p),
      words_(rows_.words()),
      matrix_(static_cast<std::size_t>(p.source_symbols) * rows_.words(), 0),
      data_(static_cast<std::size_t>(p.source_symbols) * p.symbol_size, 0),
      has_(p.source_symbols, false),
      row_(rows_.words(), 0),
      sym_(p.symbol_size, 0) {}

Result<FountainDecoder> FountainDecoder::Create(const FountainParams& p) {
    FF_TRY(p.Validate());
    return FountainDecoder(p);
}

bool FountainDecoder::Add(std::uint32_t esi, std::span<const std::uint8_t> data) {
    const std::size_t s = p_.symbol_size;
    rows_.Row(esi, row_.data());
    std::memcpy(sym_.data(), data.data(), s);

    // Reduce against the pivots held. The pivot in slot p has no bits below p, so XORing it in
    // clears bit p and can only disturb bits ABOVE it: one ascending pass over the words reduces
    // the row completely, and the first set bit with no pivot becomes a new one.
    for (std::size_t w = 0; w < words_; ++w) {
        while (row_[w] != 0) {
            const std::size_t p = w * 64 + static_cast<std::size_t>(std::countr_zero(row_[w]));
            if (!has_[p]) {
                std::copy(row_.begin(), row_.end(),
                          matrix_.begin() + static_cast<std::ptrdiff_t>(p * words_));
                std::memcpy(data_.data() + p * s, sym_.data(), s);
                has_[p] = true;
                ++rank_;
                if (complete()) BackSubstitute();
                return true;
            }
            const std::uint64_t* pr = matrix_.data() + p * words_;
            for (std::size_t j = w; j < words_; ++j) row_[j] ^= pr[j];
            XorInto(sym_.data(), data_.data() + p * s, s);
        }
    }
    return false;  // in the span of what is held: carried nothing new
}

void FountainDecoder::BackSubstitute() {
    // Upper-triangular solve, highest pivot first, so every symbol XORed in is already final.
    const std::size_t s = p_.symbol_size;
    for (std::size_t p = p_.source_symbols; p-- > 0;) {
        std::uint64_t* pr = matrix_.data() + p * words_;
        for (std::size_t w = p / 64; w < words_; ++w) {
            std::uint64_t bits = pr[w];
            if (w == p / 64) bits &= ~((std::uint64_t{2} << (p % 64)) - 1);  // strictly above p
            while (bits != 0) {
                const auto j = w * 64 + static_cast<std::size_t>(std::countr_zero(bits));
                bits &= bits - 1;
                XorInto(data_.data() + p * s, data_.data() + j * s, s);
            }
            pr[w] = (w == p / 64) ? (std::uint64_t{1} << (p % 64)) : 0;
        }
    }
    solved_ = true;
}

bool FountainDecoder::Ingest(std::uint32_t esi, std::span<const std::uint8_t> data) {
    if (data.size() != p_.symbol_size) return complete();
    if (complete()) return true;
    ++ingested_;
    Add(esi, data);
    return complete();
}

bool FountainDecoder::Prime(std::uint32_t source_index, std::span<const std::uint8_t> data) {
    if (data.size() != p_.symbol_size || source_index >= p_.source_symbols) return complete();
    if (complete()) return true;
    if (Add(source_index, data)) ++primed_;
    return complete();
}

double FountainDecoder::overhead() const noexcept {
    const auto needed = static_cast<double>(p_.source_symbols - primed_);
    if (needed <= 0.0) return 0.0;
    return (static_cast<double>(ingested_) - needed) / needed;
}

Result<std::vector<std::uint8_t>> FountainDecoder::Take() {
    if (!complete() || !solved_) return Error::kFountainIncomplete;
    return data_;
}

}  // namespace fileflow
