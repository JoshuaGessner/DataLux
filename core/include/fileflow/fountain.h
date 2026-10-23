// Systematic random linear fountain code over GF(2) — the cross-frame erasure layer
// (ADR-0009, ADR-0015).
//
// Why rateless: frame loss is a NORMAL event on this channel, and there is no reverse
// channel (NG9) to request retransmission. A fountain code makes loss a non-event -- the
// transmitter emits repair symbols indefinitely and the receiver signals completion by
// having completed.
//
// Why not RaptorQ: RaptorQ (RFC 6330) is near-optimal, but Qualcomm's IETF IPR declaration
// #1958 offers non-assert only for devices that do NOT implement a wireless wide-area standard
// -- and we target smartphones, which do. That is unresolved (RISK-016, OQ-010, RT-07) and
// RaptorQ must not become a dependency before legal review.
//
// Why not LT any more (F49, ADR-0015): protocol v1 sent a systematic prefix followed by
// robust-soliton LT repair symbols, decoded by peeling. That combination is the worst of both.
// After the systematic prefix the receiver is missing only the few source symbols that were
// lost, and a low-degree repair symbol almost never touches one of them, so most repair symbols
// carry nothing new. Measured at k = 64: 0.20 reception overhead at 5% frame loss and 0.33 at
// 30% -- and Gaussian elimination in place of peeling recovers almost none of it, because the
// waste is in the code, not the decoder. A DENSE repair row (each source symbol included with
// probability 1/2) touches each missing symbol with probability 1/2, and Gaussian elimination
// over GF(2) then completes within ~1.6 symbols of k at any loss rate: 0.025 overhead at
// k = 64, 0.006 at k = 256. That is the textbook random linear fountain (MacKay, ITILA ch. 50),
// and it carries no patent question.
//
// The cost is O(k) symbol XORs per repair symbol on both sides and a k x k bit matrix in the
// decoder, which is why `kMaxSourceSymbols` is bounded far below the LT version's: blocks are
// meant to be small (64 by default) and many.
//
// SYSTEMATIC: source symbols are emitted first, so a clean channel decodes with no
// elimination work at all. Only a degraded channel pays for repair symbols.
#pragma once

#include <fileflow/result.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

// Deterministic PRNG. The decoder must regenerate each symbol's coefficient row from its
// encoding symbol id alone, so this must produce identical output on every platform --
// which rules out std::mt19937 seeded per-symbol (slow) and anything libstdc++/libc++
// might implement differently. splitmix64 is fixed by its constants.
class SplitMix64 {
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept : s_(seed) {}

    std::uint64_t Next() noexcept {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, n). Rejection-sampled to avoid modulo bias.
    std::uint32_t Below(std::uint32_t n) noexcept {
        if (n <= 1) return 0;
        const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
        std::uint64_t r;
        do {
            r = Next();
        } while (r >= limit);
        return static_cast<std::uint32_t>(r % n);
    }

    double NextDouble() noexcept {
        return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
    }

  private:
    std::uint64_t s_;
};

struct FountainParams {
    std::uint32_t source_symbols = 0;  // k
    std::uint32_t symbol_size = 0;     // bytes per symbol
    std::uint32_t session_seed = 0;    // mixes into every ESI so sessions differ

    // Hard bounds enforced before ANY allocation (docs/security/INPUT-VALIDATION.md).
    //
    // The source-symbol bound is a COST bound as well as a memory one: the decoder holds a
    // k x k bit matrix and spends up to k symbol XORs per repair symbol, so a hostile manifest
    // declaring a huge k must not be able to buy quadratic work. 1024 is 16x the default block.
    static constexpr std::uint32_t kMaxSourceSymbols = 1024;
    static constexpr std::uint32_t kMaxSymbolSize = 64u * 1024u;

    [[nodiscard]] Status Validate() const noexcept;
};

// The coefficient row of every encoding symbol, shared by encoder and decoder -- they MUST
// agree exactly or nothing decodes. Kept in one place for that reason.
//
// A row is a bitset over the k source symbols packed into 64-bit words, bit b of word w being
// source symbol 64*w + b. ESI < k is the unit row for that source symbol (systematic); every
// ESI >= k is a dense pseudorandom row regenerated from (session seed, ESI) alone.
class FountainRows {
  public:
    explicit FountainRows(const FountainParams& p) noexcept;

    [[nodiscard]] std::size_t words() const noexcept { return words_; }

    // Writes the row for `esi` into `out`, which must hold words() entries. Never all-zero.
    void Row(std::uint32_t esi, std::uint64_t* out) const noexcept;

  private:
    FountainParams p_;
    std::size_t words_;
    std::uint64_t last_mask_;  // valid bits of the final word
};

class FountainEncoder {
  public:
    // `source` is padded to k * symbol_size internally; the caller keeps ownership of
    // nothing -- symbols are copied into the encoder.
    static Result<FountainEncoder> Create(const FountainParams& p,
                                          std::span<const std::uint8_t> source);

    // Symbols 0..k-1 are the source symbols verbatim (systematic). Beyond that, repair.
    [[nodiscard]] std::vector<std::uint8_t> Symbol(std::uint32_t esi) const;

    [[nodiscard]] const FountainParams& params() const noexcept { return p_; }

  private:
    FountainEncoder(const FountainParams& p, std::vector<std::uint8_t> src)
        : p_(p), rows_(p), src_(std::move(src)) {}

    FountainParams p_;
    FountainRows rows_;
    std::vector<std::uint8_t> src_;  // k * symbol_size, zero-padded
};

// Incremental Gaussian elimination over GF(2). Each received symbol is reduced against the
// pivots held so far the moment it arrives, so the work is spread across the transfer and a
// symbol that adds nothing new is discarded at once rather than stored.
class FountainDecoder {
  public:
    static Result<FountainDecoder> Create(const FountainParams& p);

    // Ingest one received symbol. Duplicates and out-of-order arrivals are tolerated by
    // construction (PROTOCOL-SPEC). Returns true once the block is fully recovered.
    bool Ingest(std::uint32_t esi, std::span<const std::uint8_t> data);

    // Supply a source symbol whose content the receiver already KNOWS without receiving it --
    // the zero padding past the end of the file in the last block (ADR-0015). Identical to
    // ingesting systematic symbol `source_index`, except that it is not counted as received, so
    // it neither inflates the overhead figure nor pretends the channel delivered it.
    bool Prime(std::uint32_t source_index, std::span<const std::uint8_t> data);

    [[nodiscard]] bool complete() const noexcept { return rank_ == p_.source_symbols; }

    // Degrees of freedom held: the number of source symbols' worth of information recovered.
    // Monotone non-decreasing, so it is safe to drive a progress display from it.
    [[nodiscard]] std::uint32_t recovered() const noexcept { return rank_; }
    [[nodiscard]] std::uint32_t ingested() const noexcept { return ingested_; }
    [[nodiscard]] std::uint32_t primed() const noexcept { return primed_; }

    // Reception overhead: symbols consumed beyond those needed, as a fraction of those needed.
    // Primed symbols count on neither side. This is the Rfountain term in the goodput model,
    // and EXP-012 measures it.
    [[nodiscard]] double overhead() const noexcept;

    // Valid only when complete(). Returns k * symbol_size bytes (still padded).
    [[nodiscard]] Result<std::vector<std::uint8_t>> Take();

  private:
    explicit FountainDecoder(const FountainParams& p);

    bool Add(std::uint32_t esi, std::span<const std::uint8_t> data);
    void BackSubstitute();

    FountainParams p_;
    FountainRows rows_;
    std::size_t words_;
    // Row-echelon store indexed by PIVOT: the row held in slot p has its lowest set bit at p.
    std::vector<std::uint64_t> matrix_;  // k * words_
    std::vector<std::uint8_t> data_;     // k * symbol_size
    std::vector<bool> has_;              // slot p occupied
    std::vector<std::uint64_t> row_;     // scratch, words_
    std::vector<std::uint8_t> sym_;      // scratch, symbol_size
    std::uint32_t rank_ = 0;
    std::uint32_t primed_ = 0;
    std::uint32_t ingested_ = 0;
    bool solved_ = false;
};

}  // namespace fileflow
