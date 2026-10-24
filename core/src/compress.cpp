#include <fileflow/compress.h>

#include <zstd.h>

#include <memory>

namespace fileflow {
namespace {

// Long-distance matching window: 2^27 = 128 MB. The decoder must be told to accept it.
constexpr int kLongWindowLog = 27;

struct CCtxFree {
    void operator()(ZSTD_CCtx* c) const noexcept { ZSTD_freeCCtx(c); }
};
struct DCtxFree {
    void operator()(ZSTD_DCtx* d) const noexcept { ZSTD_freeDCtx(d); }
};

}  // namespace

int MaxCompressionLevel() noexcept { return ZSTD_maxCLevel(); }

Result<std::vector<std::uint8_t>> Compress(std::span<const std::uint8_t> in,
                                           CompressOptions options) {
    const std::unique_ptr<ZSTD_CCtx, CCtxFree> ctx(ZSTD_createCCtx());
    if (!ctx) return Error::kInternal;
    const int level = options.level < 1                       ? 1
                      : options.level > ZSTD_maxCLevel()      ? ZSTD_maxCLevel()
                                                              : options.level;
    ZSTD_CCtx* c = ctx.get();
    if (ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 0)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 1))) {
        return Error::kInternal;
    }
    if (options.long_window &&
        (ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, kLongWindowLog)))) {
        return Error::kInternal;
    }
    std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
    const std::size_t n = ZSTD_compress2(c, out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n)) return Error::kInternal;
    out.resize(n);
    return out;
}

Result<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> in,
                                             std::size_t expected) {
    // The stream's own claim first: it must agree with what the sender declared.
    const unsigned long long claimed = ZSTD_getFrameContentSize(in.data(), in.size());
    if (claimed == ZSTD_CONTENTSIZE_ERROR) return Error::kUncorrectable;
    if (claimed != ZSTD_CONTENTSIZE_UNKNOWN && claimed != expected) return Error::kLengthMismatch;

    const std::unique_ptr<ZSTD_DCtx, DCtxFree> ctx(ZSTD_createDCtx());
    if (!ctx) return Error::kInternal;
    if (ZSTD_isError(ZSTD_DCtx_setParameter(ctx.get(), ZSTD_d_windowLogMax, kLongWindowLog))) {
        return Error::kInternal;
    }
    // Exactly `expected` bytes of room: a stream that would produce more fails here instead.
    std::vector<std::uint8_t> out(expected);
    const std::size_t n = ZSTD_decompressDCtx(ctx.get(), out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n)) {
        return ZSTD_getErrorCode(n) == ZSTD_error_dstSize_tooSmall ? Error::kLengthMismatch
                                                                   : Error::kUncorrectable;
    }
    if (n != expected) return Error::kLengthMismatch;
    return out;
}

}  // namespace fileflow
