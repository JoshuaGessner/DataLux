#include <fileflow/envelope.h>

#include <fileflow/compress.h>

#include <algorithm>
#include <chrono>
#include <span>

namespace fileflow {
namespace {

constexpr std::size_t kMaxNameBytes = 255;
constexpr std::size_t kMinCompressBytes = 512;
constexpr std::size_t kProbeBytes = 256 * 1024;
// A probe that saves less than this means the data is already compressed.
constexpr double kProbeMaxRatio = 0.92;
// The whole result must save at least this much, or the original is sent.
constexpr double kKeepMaxRatio = 0.97;
constexpr std::uint8_t kCodecNone = 0;
constexpr std::uint8_t kCodecZstd = 2;
constexpr std::uint8_t kFlagBatch = 4;

void PutU16(std::vector<std::uint8_t>* out, std::size_t v) {
    out->push_back(static_cast<std::uint8_t>(v >> 8));
    out->push_back(static_cast<std::uint8_t>(v));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// The name's first 255 bytes, as the app cuts it (which can split a UTF-8 sequence, exactly as
// the app's would: the receiver decodes leniently).
std::string Truncated(const std::string& name) {
    return name.size() <= kMaxNameBytes ? name : name.substr(0, kMaxNameBytes);
}

// Envelope.compressIfWorthIt, with zstd: the compressed body, or empty to send the data as is.
std::vector<std::uint8_t> CompressIfWorthIt(const std::vector<std::uint8_t>& data,
                                            CompressionMode mode) {
    if (mode == CompressionMode::kOff || data.size() < kMinCompressBytes) return {};
    const std::span<const std::uint8_t> sample(data.data(), std::min(data.size(), kProbeBytes));
    auto probe = Compress(sample, {.level = 1, .long_window = false});
    if (!probe.ok() || static_cast<double>(probe.value().size()) >
                           static_cast<double>(sample.size()) * kProbeMaxRatio) {
        return {};
    }
    const int max = MaxCompressionLevel();
    CompressOptions o;
    if (mode == CompressionMode::kMaximum) {
        // The strongest level's tables grow with the input; past 16 MB, 19 is within a few percent
        // in a tenth of the memory.
        o.level = data.size() <= 16U * 1024 * 1024 ? max : std::min(19, max);
        o.long_window = true;
    } else {
        o.level = std::min(data.size() <= 32U * 1024 * 1024 ? 19 : 15, max);
        o.long_window = data.size() > 8U * 1024 * 1024;
    }
    auto whole = Compress(data, o);
    if (!whole.ok() || static_cast<double>(whole.value().size()) >=
                           static_cast<double>(data.size()) * kKeepMaxRatio) {
        return {};
    }
    return std::move(whole).value();
}

Result<PackedEnvelope> Pack(const std::vector<std::uint8_t>& data, const std::string& name,
                            CompressionMode mode, bool batch) {
    if (data.size() > kEnvelopeMaxOriginalBytes) return Error::kPayloadTooLarge;
    const auto started = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> compressed = CompressIfWorthIt(data, mode);
    const auto ms = mode == CompressionMode::kOff
                        ? 0
                        : std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started).count();
    const bool use = !compressed.empty();
    const std::vector<std::uint8_t>& body = use ? compressed : data;
    const std::string n = Truncated(name);

    PackedEnvelope p;
    p.original_size = data.size();
    p.codec = use ? kCodecZstd : kCodecNone;
    p.compress_ms = static_cast<std::uint32_t>(std::clamp<long long>(ms, 0, 0x7FFFFFFF));
    p.bytes.reserve(4 + 1 + 2 + n.size() + 8 + 1 + 4 + body.size());
    p.bytes = {'F', 'F', 'E', '2', batch ? kFlagBatch : std::uint8_t{0}};
    PutU16(&p.bytes, n.size());
    p.bytes.insert(p.bytes.end(), n.begin(), n.end());
    PutU64(&p.bytes, p.original_size);
    p.bytes.push_back(p.codec);
    PutU32(&p.bytes, p.compress_ms);
    p.bytes.insert(p.bytes.end(), body.begin(), body.end());
    return p;
}

}  // namespace

Result<PackedEnvelope> PackEnvelope(const std::vector<std::uint8_t>& data, const std::string& name,
                                    CompressionMode mode) {
    return Pack(data, name, mode, /*batch=*/false);
}

Result<PackedEnvelope> PackBatchEnvelope(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files,
    const std::string& name, CompressionMode mode) {
    if (files.empty() || files.size() > kEnvelopeMaxBatchFiles) return Error::kValueOutOfRange;
    std::uint64_t size = 4 + 2;
    for (const auto& [n, bytes] : files) size += 2 + Truncated(n).size() + 8 + bytes.size();
    if (size > kEnvelopeMaxOriginalBytes) return Error::kPayloadTooLarge;
    std::vector<std::uint8_t> data;
    data.reserve(static_cast<std::size_t>(size));
    data = {'F', 'F', 'B', '1'};
    PutU16(&data, files.size());
    for (const auto& [n, bytes] : files) {
        const std::string t = Truncated(n);
        PutU16(&data, t.size());
        data.insert(data.end(), t.begin(), t.end());
        PutU64(&data, bytes.size());
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    return Pack(data, name, mode, /*batch=*/true);
}

}  // namespace fileflow
