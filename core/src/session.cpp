#include <fileflow/session.h>

#include <fileflow/hash.h>

#include <algorithm>
#include <cmath>

namespace fileflow {

// ---------------------------------------------------------------- transmit

Result<TransmitSession> TransmitSession::Create(const FrameLayout& layout,
                                                std::span<const std::uint8_t> file,
                                                std::string file_name, std::uint32_t session_id,
                                                TransmitConfig cfg) {
    const M0Modulator mod(layout);
    const HeaderCodec hdr;
    // FrameLayout validates geometry but cannot know how large the CODED header is, so a grid
    // too small for its own header would build and then fail at render with a generic error.
    if (mod.header_capacity_bytes() < hdr.coded_size()) return Error::kValueOutOfRange;
    if (cfg.fec_nsym >= 255) return Error::kValueOutOfRange;
    const std::size_t capacity = PayloadCapacityBytes(layout, cfg.profile);
    if (capacity == 0) return Error::kUnknownProfile;

    std::optional<IntraFec> fec;
    std::uint32_t symbol_size = cfg.symbol_size;
    if (cfg.fec_nsym > 0) {
        IntraFecParams fp;
        fp.nsym = cfg.fec_nsym;
        FF_ASSIGN_OR_RETURN(auto f, IntraFec::Create(capacity, fp));
        // The fountain symbol must fit the FEC layer's MESSAGE capacity, not the raw frame
        // capacity -- parity occupies the difference.
        const auto required = static_cast<std::uint32_t>(f.message_bytes());
        if (symbol_size != 0 && symbol_size != required) return Error::kLengthMismatch;
        symbol_size = required;
        fec.emplace(std::move(f));
    } else if (symbol_size == 0) {
        symbol_size = static_cast<std::uint32_t>(capacity);
    }
    if (symbol_size == 0 || symbol_size > FountainParams::kMaxSymbolSize ||
        symbol_size > capacity) {
        return Error::kValueOutOfRange;
    }

    FF_ASSIGN_OR_RETURN(auto tx, FileTransmitter::Create(file, std::move(file_name), session_id,
                                                         symbol_size, cfg.block_symbols,
                                                         cfg.manifest_interval));
    return TransmitSession(layout, std::move(fec), std::move(tx), cfg.fec_nsym, cfg.profile);
}

Result<FrameHeader> TransmitSession::Next(CellMatrix* out) {
    auto fp = tx_.NextFrame();
    fp.header.fec_nsym = static_cast<std::uint8_t>(nsym_);
    fp.header.profile = profile_;
    FF_ASSIGN_OR_RETURN(auto hdr_coded, hdr_.Encode(fp.header));

    std::vector<std::uint8_t> payload_coded;
    if (fec_.has_value()) {
        FF_ASSIGN_OR_RETURN(payload_coded, fec_->Encode(fp.data));
    } else {
        payload_coded = std::move(fp.data);
    }
    if (profile_ == ModulationProfile::kM5EightColor || profile_ == ModulationProfile::kM3FourColor) {
        FF_TRY(color_.Render(hdr_coded, payload_coded, out));
    } else {
        FF_TRY(mod_.Render(hdr_coded, payload_coded, out));
    }
    return fp.header;
}

// ---------------------------------------------------------------- receive

std::string_view FrameOutcomeName(FrameOutcome o) noexcept {
    switch (o) {
        case FrameOutcome::kUnreadable: return "unreadable";
        case FrameOutcome::kHeaderFailed: return "header_failed";
        case FrameOutcome::kForeignSession: return "foreign_session";
        case FrameOutcome::kPayloadUnsupported: return "payload_unsupported";
        case FrameOutcome::kPayloadUncorrectable: return "payload_uncorrectable";
        case FrameOutcome::kPayloadCrcFailed: return "payload_crc_failed";
        case FrameOutcome::kManifest: return "manifest";
        case FrameOutcome::kManifestRejected: return "manifest_rejected";
        case FrameOutcome::kSymbol: return "symbol";
        case FrameOutcome::kBuffered: return "buffered";
        case FrameOutcome::kBufferFull: return "buffer_full";
    }
    return "unknown";
}

double ReceiveStats::submission_rate() const noexcept {
    const double s = seconds_observed();
    if (!(s > 0.0) || last_sequence <= first_sequence) return 0.0;
    return static_cast<double>(last_sequence - first_sequence) / s;
}

double ReceiveStats::distinct_rate() const noexcept {
    const double s = seconds_observed();
    // n distinct states over the span between the first and last of them is n-1 intervals.
    if (!(s > 0.0) || distinct_sequences < 2) return 0.0;
    return static_cast<double>(distinct_sequences - 1) / s;
}

Result<ReceiveSession> ReceiveSession::Create(const FrameLayout& layout, ReceiveConfig cfg) {
    const M0Modulator mod(layout);
    if (mod.header_capacity_bytes() < HeaderCodec{}.coded_size()) return Error::kValueOutOfRange;
    return ReceiveSession(layout, cfg);
}

const IntraFec* ReceiveSession::FecFor(std::size_t nsym, std::size_t bits_per_cell) {
    // Built lazily per code rate and profile seen, from the header, so the receiver needs no
    // configuration for either -- and a transmitter that changes them between sessions just works.
    const auto key = std::make_pair(nsym, bits_per_cell);
    auto it = fec_.find(key);
    if (it == fec_.end()) {
        IntraFecParams fp;
        fp.nsym = nsym;
        const std::size_t capacity = bits_per_cell == 3   ? color8_.payload_capacity_bytes()
                                     : bits_per_cell == 2 ? color4_.payload_capacity_bytes()
                                                          : mod_.payload_capacity_bytes();
        auto f = IntraFec::Create(capacity, fp);
        std::optional<IntraFec> entry;
        if (f.ok()) entry.emplace(std::move(f).value());
        it = fec_.emplace(key, std::move(entry)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void ReceiveSession::NoteSequence(std::uint32_t seq, std::int64_t ts) {
    if (!any_sequence_) {
        any_sequence_ = true;
        stats_.first_sequence = seq;
        stats_.first_timestamp_ns = ts;
    }
    stats_.last_sequence = std::max(stats_.last_sequence, seq);
    stats_.last_timestamp_ns = ts;
    // Bounded: a hostile stream of distinct sequence numbers must not grow this without limit.
    if (seen_.count(seq) != 0) {
        ++stats_.repeated_sequences;
    } else if (seen_.size() < (std::size_t{1} << 20)) {
        seen_.insert(seq);
        ++stats_.distinct_sequences;
    }
}

FrameOutcome ReceiveSession::AcceptManifest(const FrameHeader& h,
                                            std::span<const std::uint8_t> bytes) {
    ++stats_.manifests;
    if (rx_.has_value()) return FrameOutcome::kManifest;  // already running; repeats are free

    // Attacker-controlled bytes: Decode bounds and CRCs everything, Validate runs inside it, and
    // the receiver allocates only what a validated manifest declares (INPUT-VALIDATION SR-1/2).
    auto m = FileManifest::Decode(bytes);
    if (!m.ok() || m.value().session_id != h.session_id) {
        ++stats_.manifests_rejected;
        return FrameOutcome::kManifestRejected;
    }
    auto rx = FileReceiver::Create(m.value());
    if (!rx.ok()) {
        ++stats_.manifests_rejected;
        return FrameOutcome::kManifestRejected;
    }
    rx_.emplace(std::move(rx).value());

    // Everything read before the manifest is used now. Frames from any other session are dropped
    // by FileReceiver's own session check, never mixed in (THREAT-MODEL T8).
    // Replay stops at completion: symbols that arrived while the receiver waited for this frame
    // were never consumed, and counting them would charge the manifest's latency to the code.
    for (const Held& held : held_) {
        if (rx_->complete()) break;
        if (held.header.session_id != rx_->manifest().session_id) continue;
        ++stats_.symbols;
        rx_->Ingest(held.header, held.data);
    }
    held_.clear();
    held_.shrink_to_fit();
    return FrameOutcome::kManifest;
}

FrameOutcome ReceiveSession::Ingest(std::span<const double> cell_samples,
                                    std::span<const double> cell_rgb, std::int64_t timestamp_ns) {
    ++stats_.frames;
    last_reached_fec_ = false;

    if (std::none_of(cell_samples.begin(), cell_samples.end(),
                     [](double v) { return !std::isnan(v); })) {
        ++stats_.unreadable;
        return FrameOutcome::kUnreadable;
    }

    // --- Header ---
    const PhotometricRef ref = mod_.EstimateReference(cell_samples);
    std::vector<std::size_t> erased;
    auto hbytes = mod_.DemodulateHeader(cell_samples, ref, hdr_.coded_size(), &erased);
    if (!hbytes.ok()) {
        ++stats_.header_fail;
        return FrameOutcome::kHeaderFailed;
    }
    std::vector<std::uint8_t> hbuf = std::move(hbytes).value();
    auto hdr_r = hdr_.Decode(hbuf, erased);
    if (!hdr_r.ok()) {
        ++stats_.header_fail;
        return FrameOutcome::kHeaderFailed;
    }
    ++stats_.header_ok;
    if (!erased.empty()) {
        // Would it have decoded blind? Only asked on success, so the cost lands on the frames
        // that are worth counting.
        std::vector<std::uint8_t> blind = mod_.DemodulateHeader(cell_samples, ref,
                                                                hdr_.coded_size()).value();
        if (!hdr_.Decode(blind).ok()) ++stats_.header_rescued_by_erasures;
    }
    const FrameHeader h = hdr_r.value();
    NoteSequence(h.sequence, timestamp_ns);

    if (rx_.has_value() && h.session_id != rx_->manifest().session_id) {
        ++stats_.foreign;
        return FrameOutcome::kForeignSession;
    }

    // --- Payload ---
    const std::size_t bits = BitsPerCell(h.profile);
    last_bits_ = bits;
    const ColorModulator* color = h.profile == ModulationProfile::kM5EightColor   ? &color8_
                                  : h.profile == ModulationProfile::kM3FourColor ? &color4_
                                                                                 : nullptr;
    if (bits == 0 || (color != nullptr && cell_rgb.size() != cell_samples.size() * 3)) {
        return FrameOutcome::kPayloadUnsupported;
    }
    const IntraFec* fec = nullptr;
    if (h.fec_nsym > 0) {
        fec = FecFor(h.fec_nsym, bits);
        if (fec == nullptr) return FrameOutcome::kPayloadUnsupported;
    }

    SoftSymbolBuffer soft;
    if (color != nullptr) {
        color->DemodulatePayload(cell_rgb, &soft, &last_color_);
    } else {
        mod_.DemodulatePayload(cell_samples, ref, &soft);
    }
    const std::size_t capacity =
        color != nullptr ? color->payload_capacity_bytes() : mod_.payload_capacity_bytes();
    const std::size_t coded_len = fec ? fec->coded_bytes() : capacity;
    HardDecision hd = HardDecide(soft, coded_len, cfg_.erase_below);
    stats_.payload_cells += soft.size();
    stats_.erased_cells += hd.erasures;

    std::vector<std::uint8_t> message;
    if (fec != nullptr) {
        last_reached_fec_ = true;
        auto dec = fec->Decode(hd.bytes, hd.erased_bytes, &last_fec_);
        stats_.worst_codeword_erasures =
            std::max(stats_.worst_codeword_erasures, last_fec_.worst_erasures_in_codeword);
        if (!dec.ok()) {
            // Never ingest partially-corrected data: the fountain layer absorbs a lost frame and
            // would be poisoned by a half-corrected one.
            ++stats_.fec_fail;
            return FrameOutcome::kPayloadUncorrectable;
        }
        ++stats_.fec_ok;
        message = std::move(dec).value();
    } else {
        message = std::move(hd.bytes);
    }

    if (h.payload_bytes > message.size()) {
        ++stats_.crc_fail;
        return FrameOutcome::kPayloadCrcFailed;
    }
    const std::span<const std::uint8_t> data(message.data(), h.payload_bytes);
    if (Crc32(data) != h.payload_crc) {
        ++stats_.crc_fail;
        return FrameOutcome::kPayloadCrcFailed;
    }

    if (h.is_manifest()) return AcceptManifest(h, data);

    if (rx_.has_value()) {
        ++stats_.symbols;
        rx_->Ingest(h, data);
        return FrameOutcome::kSymbol;
    }
    if (held_.size() >= cfg_.max_buffered_frames) {
        ++stats_.buffer_dropped;
        return FrameOutcome::kBufferFull;
    }
    ++stats_.buffered;
    held_.push_back(Held{h, std::vector<std::uint8_t>(data.begin(), data.end())});
    return FrameOutcome::kBuffered;
}

Result<std::vector<std::uint8_t>> ReceiveSession::Finish() {
    if (!rx_.has_value()) return Error::kFountainIncomplete;
    return rx_->Finish();
}

}  // namespace fileflow
