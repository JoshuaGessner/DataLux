#include <fileflow/transfer.h>

#include <fileflow/bytes.h>

#include <algorithm>
#include <cctype>

namespace fileflow {

Status FileManifest::Validate() const noexcept {
    if (file_size == 0 || file_size > kMaxFileSize) return Error::kValueOutOfRange;
    if (block_count == 0 || block_count > kMaxBlockCount) return Error::kValueOutOfRange;
    if (block_symbols == 0 || block_symbols > FountainParams::kMaxSourceSymbols)
        return Error::kValueOutOfRange;
    if (symbol_size == 0 || symbol_size > FountainParams::kMaxSymbolSize)
        return Error::kValueOutOfRange;
    if (file_name.size() > kMaxNameLength) return Error::kValueOutOfRange;

    // Checked arithmetic: block_count * block_symbols * symbol_size must not overflow, and
    // must plausibly cover the declared file size (INPUT-VALIDATION SR-2).
    std::size_t per_block = 0;
    if (!CheckedMul(block_symbols, symbol_size, &per_block)) return Error::kSizeOverflow;
    std::size_t total = 0;
    if (!CheckedMul(block_count, per_block, &total)) return Error::kSizeOverflow;
    if (total < file_size) return Error::kLengthMismatch;
    if (total > kMaxFileSize + per_block) return Error::kPayloadTooLarge;
    return Status::Ok();
}

std::vector<std::uint8_t> FileManifest::Encode() const {
    ByteWriter w;
    w.U8(kProtocolVersion);
    w.U32(session_id);
    w.U64(file_size);
    w.Bytes(sha256);
    w.U32(block_count);
    w.U32(block_symbols);
    w.U32(symbol_size);
    w.U8(static_cast<std::uint8_t>(std::min(file_name.size(), kMaxNameLength)));
    w.Bytes({reinterpret_cast<const std::uint8_t*>(file_name.data()),
             std::min(file_name.size(), kMaxNameLength)});
    w.U16(0);  // extension length: none. Readers SKIP unknown extensions by length.
    const std::uint32_t crc = Crc32(w.data());
    w.U32(crc);
    return std::move(w).take();
}

Result<FileManifest> FileManifest::Decode(std::span<const std::uint8_t> in) {
    // Minimum: version+session+size+hash+counts+namelen+extlen+crc
    constexpr std::size_t kMinSize = 1 + 4 + 8 + 32 + 4 + 4 + 4 + 1 + 2 + 4;
    if (in.size() < kMinSize) return Error::kTruncated;

    // Verify CRC over everything but the trailing 4 bytes before trusting any field.
    const std::span<const std::uint8_t> body = in.subspan(0, in.size() - 4);
    ByteReader crc_r(in.subspan(in.size() - 4, 4));
    auto got = crc_r.U32();
    if (!got.ok()) return got.error();
    if (got.value() != Crc32(body)) return Error::kHeaderCrcMismatch;

    ByteReader r(body);
    FileManifest m;

    auto ver = r.U8();
    if (!ver.ok()) return ver.error();
    if (ver.value() > kProtocolVersion) return Error::kUnsupportedVersion;

    auto sid = r.U32();
    if (!sid.ok()) return sid.error();
    m.session_id = sid.value();

    auto fs = r.U64();
    if (!fs.ok()) return fs.error();
    if (fs.value() > kMaxFileSize) return Error::kPayloadTooLarge;
    m.file_size = fs.value();

    auto digest = r.Bytes(Sha256::kDigestSize);
    if (!digest.ok()) return digest.error();
    std::copy(digest.value().begin(), digest.value().end(), m.sha256.begin());

    auto bc = r.U32();
    if (!bc.ok()) return bc.error();
    m.block_count = bc.value();

    auto bs = r.U32();
    if (!bs.ok()) return bs.error();
    m.block_symbols = bs.value();

    auto ss = r.U32();
    if (!ss.ok()) return ss.error();
    m.symbol_size = ss.value();

    auto nlen = r.U8();
    if (!nlen.ok()) return nlen.error();
    auto name = r.Bytes(nlen.value());
    if (!name.ok()) return name.error();
    m.file_name = SanitiseFileName(
        std::string_view(reinterpret_cast<const char*>(name.value().data()), name.value().size()));

    auto extlen = r.U16();
    if (!extlen.ok()) return extlen.error();
    if (extlen.value() > 4096) return Error::kTooManyExtensions;
    // Forward compatibility: unknown extensions are SKIPPED BY LENGTH, never rejected.
    // A protocol that rejects unknown fields cannot be extended without breaking every
    // deployed receiver (PROTOCOL-SPEC).
    FF_TRY(r.Skip(extlen.value()));

    FF_TRY(m.Validate());
    return m;
}

std::string SanitiseFileName(std::string_view raw) {
    // 1. basename only -- discard everything up to and including the last separator
    std::size_t start = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '/' || raw[i] == '\\') start = i + 1;
    }
    std::string_view base = raw.substr(start);

    // 2. strip separators, nulls, control characters
    std::string out;
    out.reserve(base.size());
    for (char ch : base) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc < 0x20 || uc == 0x7F) continue;
        if (ch == '/' || ch == '\\' || ch == '\0') continue;
        out.push_back(ch);
    }

    // 3. reject names that are entirely dots ("." / ".." / "...")
    if (out.find_first_not_of('.') == std::string::npos) out.clear();

    // 4. bound the length
    if (out.size() > FileManifest::kMaxNameLength) {
        out.resize(FileManifest::kMaxNameLength);
    }

    // 5. safe fallback if nothing usable remains
    if (out.empty()) out = "received.bin";
    return out;
}

// ---------------------------------------------------------------- block geometry

std::uint32_t FirstPaddingSymbol(const FileManifest& m, std::uint32_t block) noexcept {
    const std::uint64_t per_block =
        static_cast<std::uint64_t>(m.block_symbols) * static_cast<std::uint64_t>(m.symbol_size);
    const std::uint64_t off = static_cast<std::uint64_t>(block) * per_block;
    if (m.symbol_size == 0 || off >= m.file_size) return 0;
    const std::uint64_t len = std::min(per_block, m.file_size - off);
    const std::uint64_t data_symbols = (len + m.symbol_size - 1) / m.symbol_size;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(data_symbols, m.block_symbols));
}

std::uint32_t AutoBlockSymbols(std::uint64_t total_symbols) noexcept {
    if (total_symbols == 0) return 1;
    const std::uint64_t kmax = FountainParams::kMaxSourceSymbols;
    const std::uint64_t blocks = (total_symbols + kmax - 1) / kmax;
    return static_cast<std::uint32_t>((total_symbols + blocks - 1) / blocks);
}

// ---------------------------------------------------------------- transmitter

std::uint32_t FileTransmitter::AutoManifestInterval(std::uint64_t symbols_needed) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(symbols_needed / 2, 16, 128));
}

FileTransmitter::FileTransmitter(FileManifest m, std::vector<FountainEncoder> enc,
                                 std::vector<std::uint32_t> first_padding,
                                 std::uint32_t manifest_interval)
    : manifest_(std::move(m)),
      manifest_bytes_(manifest_.Encode()),
      encoders_(std::move(enc)),
      first_padding_(std::move(first_padding)),
      next_esi_(encoders_.size(), 0),
      manifest_interval_(manifest_interval) {}

Result<FileTransmitter> FileTransmitter::Create(std::span<const std::uint8_t> payload,
                                                std::string file_name,
                                                std::uint32_t session_id,
                                                std::uint32_t symbol_size,
                                                std::uint32_t block_symbols,
                                                std::uint32_t manifest_interval) {
    if (payload.empty()) return Error::kValueOutOfRange;
    if (symbol_size == 0) return Error::kValueOutOfRange;
    if (block_symbols == 0) {
        block_symbols = AutoBlockSymbols((payload.size() + symbol_size - 1) / symbol_size);
    }

    std::size_t per_block = 0;
    if (!CheckedMul(block_symbols, symbol_size, &per_block)) return Error::kSizeOverflow;

    const auto block_count =
        static_cast<std::uint32_t>((payload.size() + per_block - 1) / per_block);

    FileManifest m;
    m.file_size = payload.size();
    m.sha256 = Sha256::Of(payload);
    m.file_name = SanitiseFileName(file_name);
    m.block_count = block_count;
    m.block_symbols = block_symbols;
    m.symbol_size = symbol_size;
    m.session_id = session_id;
    FF_TRY(m.Validate());

    // The manifest travels in ONE frame, so it must fit one symbol. At a real grid a symbol is
    // kilobytes and a manifest at most ~320 bytes; only a deliberately tiny symbol can trip this.
    if (m.Encode().size() > symbol_size) return Error::kValueOutOfRange;

    std::vector<FountainEncoder> encoders;
    std::vector<std::uint32_t> first_padding;
    encoders.reserve(block_count);
    first_padding.reserve(block_count);
    std::uint64_t needed = 0;
    for (std::uint32_t b = 0; b < block_count; ++b) {
        const std::size_t off = static_cast<std::size_t>(b) * per_block;
        const std::size_t len = std::min(per_block, payload.size() - off);

        FountainParams p;
        p.source_symbols = block_symbols;
        p.symbol_size = symbol_size;
        p.session_seed = session_id ^ b;

        auto enc = FountainEncoder::Create(p, payload.subspan(off, len));
        if (!enc.ok()) return enc.error();
        encoders.push_back(std::move(enc).value());
        first_padding.push_back(FirstPaddingSymbol(m, b));
        needed += first_padding.back();
    }

    if (manifest_interval == 0) manifest_interval = AutoManifestInterval(needed);
    return FileTransmitter(std::move(m), std::move(encoders), std::move(first_padding),
                           manifest_interval);
}

FileTransmitter::FramePayload FileTransmitter::NextFrame() {
    FramePayload fp;
    fp.header.session_id = manifest_.session_id;
    fp.header.sequence = static_cast<std::uint32_t>(counter_);
    fp.header.profile = ModulationProfile::kM0BinaryLuminance;
    fp.header.phase = static_cast<std::uint8_t>(counter_ % 16);

    if (counter_ % manifest_interval_ == 0) {
        // Manifest frame. Filler after the manifest is keyed to the sequence number so successive
        // manifest frames do not render identically -- a static pattern would hide a frozen
        // display from any receiver-side duplicate detector.
        fp.data.resize(manifest_.symbol_size);
        std::copy(manifest_bytes_.begin(), manifest_bytes_.end(), fp.data.begin());
        SplitMix64 filler((static_cast<std::uint64_t>(manifest_.session_id) << 32) ^
                          (counter_ * 0xD1B54A32D192ED03ULL));
        for (std::size_t i = manifest_bytes_.size(); i < fp.data.size(); ++i) {
            fp.data[i] = static_cast<std::uint8_t>(filler.Next() & 0xFF);
        }
        fp.header.flags |= FrameHeader::kFlagManifest;
        fp.header.payload_bytes = static_cast<std::uint32_t>(manifest_bytes_.size());
        fp.header.payload_crc = Crc32(manifest_bytes_);
        ++counter_;
        return fp;
    }

    // Round-robin across blocks: a receiver that starts late still makes progress on every
    // block instead of waiting a full cycle.
    const auto bc = static_cast<std::uint64_t>(manifest_.block_count);
    const auto block = static_cast<std::uint32_t>(data_frames_ % bc);

    // Systematic symbols that are pure padding are never sent: the receiver already knows them.
    std::uint32_t esi = next_esi_[block];
    if (esi >= first_padding_[block] && esi < manifest_.block_symbols) {
        esi = manifest_.block_symbols;
    }
    next_esi_[block] = esi + 1;

    fp.data = encoders_[block].Symbol(esi);
    fp.header.block_id = block;
    fp.header.esi = esi;
    fp.header.payload_bytes = static_cast<std::uint32_t>(fp.data.size());
    fp.header.payload_crc = Crc32(fp.data);
    if (block + 1 == manifest_.block_count) fp.header.flags |= FrameHeader::kFlagLastBlock;

    ++data_frames_;
    ++counter_;
    return fp;
}

// ---------------------------------------------------------------- receiver

Result<FileReceiver> FileReceiver::Create(const FileManifest& m) {
    FF_TRY(m.Validate());

    FileReceiver rx(m);
    rx.decoders_.reserve(m.block_count);
    const std::vector<std::uint8_t> zero(m.symbol_size, 0);
    for (std::uint32_t b = 0; b < m.block_count; ++b) {
        FountainParams p;
        p.source_symbols = m.block_symbols;
        p.symbol_size = m.symbol_size;
        p.session_seed = m.session_id ^ b;

        auto dec = FountainDecoder::Create(p);
        if (!dec.ok()) return dec.error();
        rx.decoders_.push_back(std::move(dec).value());

        // The padding past the end of the file is zero by definition, so it is known before a
        // single frame arrives. Priming it is what lets the transmitter never send it.
        const std::uint32_t first_pad = FirstPaddingSymbol(m, b);
        for (std::uint32_t i = first_pad; i < m.block_symbols; ++i) {
            rx.decoders_.back().Prime(i, zero);
        }
        rx.needed_ += first_pad;
    }
    return rx;
}

bool FileReceiver::Ingest(const FrameHeader& h, std::span<const std::uint8_t> data) {
    // A manifest frame's payload is not a symbol, and block 0 / ESI 0 in its header mean nothing.
    // Ingesting it would poison block 0 with manifest bytes that pass the frame CRC.
    if (h.is_manifest()) return complete();

    // Session isolation: packets from another transmitter are discarded, not mixed in
    // (THREAT-MODEL T8).
    if (h.session_id != manifest_.session_id) return complete();
    if (h.block_id >= decoders_.size()) return complete();
    if (data.size() != manifest_.symbol_size) return complete();

    // A damaged symbol must NEVER reach the fountain decoder. An erasure code cannot tell
    // corrupt data from good, so a single bad symbol silently poisons the whole block and
    // surfaces only as an unattributable hash mismatch at the very end. Failing this check
    // makes the frame an erasure -- exactly what the fountain layer is designed to absorb.
    if (Crc32(data) != h.payload_crc) return complete();

    ++ingested_;
    decoders_[h.block_id].Ingest(h.esi, data);
    return complete();
}

bool FileReceiver::complete() const noexcept {
    return std::all_of(decoders_.begin(), decoders_.end(),
                       [](const FountainDecoder& d) { return d.complete(); });
}

std::uint32_t FileReceiver::blocks_complete() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(
        decoders_.begin(), decoders_.end(), [](const FountainDecoder& d) { return d.complete(); }));
}

double FileReceiver::progress() const noexcept {
    // Primed padding is excluded on both sides: progress measures what the CHANNEL has delivered,
    // so a fresh receiver reads exactly zero however much padding it already knows.
    if (needed_ == 0) return complete() ? 1.0 : 0.0;
    std::uint64_t recovered = 0;
    for (const auto& d : decoders_) recovered += d.recovered() - d.primed();
    return static_cast<double>(recovered) / static_cast<double>(needed_);
}

double FileReceiver::overhead() const noexcept {
    if (needed_ == 0) return 0.0;
    const auto k = static_cast<double>(needed_);
    return (static_cast<double>(ingested_) - k) / k;
}

Result<std::vector<std::uint8_t>> FileReceiver::Finish() {
    if (!complete()) return Error::kFountainIncomplete;

    std::vector<std::uint8_t> out;
    out.reserve(manifest_.file_size);

    for (auto& d : decoders_) {
        auto blk = d.Take();
        if (!blk.ok()) return blk.error();
        const auto& b = blk.value();
        const std::size_t take = std::min<std::size_t>(b.size(), manifest_.file_size - out.size());
        out.insert(out.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(take));
        if (out.size() >= manifest_.file_size) break;
    }

    if (out.size() != manifest_.file_size) return Error::kLengthMismatch;

    // THE GATE. Nothing is delivered without this passing (G3, THREAT-MODEL T5).
    if (Sha256::Of(out) != manifest_.sha256) return Error::kHashMismatch;

    return out;
}

}  // namespace fileflow
