// Session layer (C13a): the ONE transmit path and the ONE receive path between cells and files.
//
// Before this existed, the frame -> header -> FEC -> fountain chain was written out by hand in
// ffsim, again in ffreplay (which stopped at the header), again in the JNI transmitter and again in
// the end-to-end tests. Four copies of the same protocol is four chances to disagree about it, and
// FramePipeline's rule applies here for the same reason: if the simulator, the replay harness and
// the phone drive different code, a recorded dataset proves nothing about live behaviour.
//
// The receive path also carries the part none of the copies had: it learns everything it needs
// from the frames themselves. The intra-frame code rate is in every header and the manifest
// arrives in-band (ADR-0015), so a receiver with no out-of-band knowledge beyond the grid can
// complete and verify a transfer -- which is the only kind of receiver a real capture has.
#pragma once

#include <fileflow/frame.h>
#include <fileflow/grid.h>
#include <fileflow/intra_fec.h>
#include <fileflow/modulation.h>
#include <fileflow/result.h>
#include <fileflow/transfer.h>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fileflow {

// ---------------------------------------------------------------- transmit

struct TransmitConfig {
    // RS parity bytes per intra-frame codeword; 0 disables intra-frame FEC (the F18 A/B arm).
    std::size_t fec_nsym = 32;
    // 0 = fill the frame. With FEC on the symbol size is fixed by the code's message capacity, and
    // a different non-zero value is refused rather than silently overridden.
    std::uint32_t symbol_size = 0;
    std::uint32_t block_symbols = 0;      // 0 = AutoBlockSymbols
    std::uint32_t manifest_interval = 0;  // 0 = FileTransmitter::AutoManifestInterval
    // How payload cells carry bits: M0 (one, black or white) or M5 (three, eight colours). Named
    // in every header, so the receiver needs no configuration for it.
    ModulationProfile profile = ModulationProfile::kM0BinaryLuminance;
    // What the file is before compression, put in the manifest for the receiver's display.
    // original_size 0 = say nothing (an encrypted transfer).
    ContentHint content{};
};

// A transfer's session id, from the FILE: not a constant and not a random number.
//
// A constant meant two different files were the same session to a receiver: one that had started on
// the first kept going on frames of the second, and the fountain decoder mixed them into a file
// that could only fail verification. A random id per Send fixes that and breaks the opposite case --
// pressing Send again for the SAME file (the sender timed out, or the app was reopened) would strand
// a receiver half-way through, because every new frame would belong to a "different" transfer.
// Hashing the content and name gives both: same file, same session, the receiver carries on; any
// other file, a different session. Never 0.
[[nodiscard]] std::uint32_t SessionIdFor(std::span<const std::uint8_t> payload, std::string_view name);

class TransmitSession {
  public:
    // `layout` must outlive the session.
    static Result<TransmitSession> Create(const FrameLayout& layout,
                                          std::span<const std::uint8_t> file,
                                          std::string file_name, std::uint32_t session_id,
                                          TransmitConfig cfg = {});

    // Renders the next display state into `out` and returns the header it carries.
    [[nodiscard]] Result<FrameHeader> Next(CellMatrix* out);

    [[nodiscard]] const FileManifest& manifest() const noexcept { return tx_.manifest(); }
    [[nodiscard]] const FileTransmitter& transmitter() const noexcept { return tx_; }
    [[nodiscard]] const std::optional<IntraFec>& fec() const noexcept { return fec_; }

  private:
    TransmitSession(const FrameLayout& layout, std::optional<IntraFec> fec, FileTransmitter tx,
                    std::size_t nsym, ModulationProfile profile)
        : mod_(layout), color_(layout, profile), fec_(std::move(fec)), tx_(std::move(tx)),
          nsym_(nsym), profile_(profile) {}

    M0Modulator mod_;
    ColorModulator color_;  // the profile's own palette when it is a colour one
    HeaderCodec hdr_;
    std::optional<IntraFec> fec_;
    FileTransmitter tx_;
    std::size_t nsym_;
    ModulationProfile profile_;
};

// ---------------------------------------------------------------- receive

struct ReceiveConfig {
    // Payload cells whose |LLR| falls below this are erased rather than guessed (see HardDecide).
    Llr erase_below = 0;
    // Symbols decoded before the manifest are kept and replayed once it arrives, so a receiver
    // that misses the first manifest loses nothing. Bounded: 4096 frames is ~13 MB at 120x260.
    std::size_t max_buffered_frames = 4096;
    // Refuse an in-band manifest larger than this before FileReceiver allocates fountain state.
    // The protocol ceiling remains FileManifest::kMaxFileSize; platforms with tighter memory
    // budgets (notably wasm32/mobile browsers) should lower it.
    std::uint64_t max_file_bytes = FileManifest::kMaxFileSize;
};

enum class FrameOutcome : std::uint8_t {
    kUnreadable,            // no usable cell samples at all
    kHeaderFailed,          // header did not decode: the frame is lost
    kForeignSession,        // a readable frame from a different transmitter than the manifest's
    kPayloadUnsupported,    // header names a code rate this grid cannot carry
    kPayloadUncorrectable,  // intra-frame FEC exceeded its budget
    kPayloadCrcFailed,      // FEC "succeeded" but the bytes are wrong: a miscorrection caught
    kManifest,              // a manifest frame (the first one starts the transfer)
    kManifestRejected,      // a manifest frame whose contents failed validation
    kSymbol,                // a fountain symbol, ingested
    kBuffered,              // a fountain symbol, held until the manifest arrives
    kBufferFull,            // a fountain symbol dropped: no manifest and no room
};

[[nodiscard]] std::string_view FrameOutcomeName(FrameOutcome o) noexcept;

struct ReceiveStats {
    std::uint64_t frames = 0;
    std::uint64_t unreadable = 0;
    std::uint64_t header_ok = 0;
    std::uint64_t header_fail = 0;
    // Headers that decoded only because erasure positions were supplied (F50) -- the measured
    // value of that change, per capture.
    std::uint64_t header_rescued_by_erasures = 0;
    std::uint64_t foreign = 0;
    std::uint64_t payload_cells = 0;
    std::uint64_t erased_cells = 0;
    std::uint64_t fec_ok = 0;
    std::uint64_t fec_fail = 0;
    // Of fec_ok: frames that failed alone and decoded once combined with an earlier capture of
    // the same display state -- what keeping failed frames is worth.
    std::uint64_t fec_ok_combined = 0;
    std::uint64_t crc_fail = 0;
    std::uint64_t manifests = 0;
    std::uint64_t manifests_rejected = 0;
    std::uint64_t symbols = 0;
    std::uint64_t buffered = 0;
    std::uint64_t buffer_dropped = 0;
    std::size_t worst_codeword_erasures = 0;

    // ---- display-state accounting (EXP-006) ----
    //
    // The sequence number in every header counts states the transmitter SUBMITTED. The receiver
    // sees the subset that was presented and then captured. Three different rates fall out, and
    // they must not be conflated (ADR-0012):
    //   submission rate  = sequence span / time span   -- what the transmitter attempted
    //   distinct rate    = distinct sequences / time   -- what reached this receiver, <= Fd
    //   repeats          = captures of a state already seen: camera oversampling
    // With the camera at >= 2x the state rate, a sequence number never seen is evidence the
    // display never presented it, which is the dropped-state term Fd exists to capture.
    std::uint64_t distinct_sequences = 0;
    std::uint64_t repeated_sequences = 0;
    std::uint32_t first_sequence = 0;
    std::uint32_t last_sequence = 0;
    std::int64_t first_timestamp_ns = 0;
    std::int64_t last_timestamp_ns = 0;

    [[nodiscard]] double header_success() const noexcept {
        const auto n = header_ok + header_fail;
        return n ? static_cast<double>(header_ok) / static_cast<double>(n) : 0.0;
    }
    [[nodiscard]] double cell_erasure_rate() const noexcept {
        return payload_cells ? static_cast<double>(erased_cells) / static_cast<double>(payload_cells)
                             : 0.0;
    }
    [[nodiscard]] double seconds_observed() const noexcept {
        return static_cast<double>(last_timestamp_ns - first_timestamp_ns) * 1e-9;
    }
    [[nodiscard]] double submission_rate() const noexcept;  // states/s, 0 if not measurable
    [[nodiscard]] double distinct_rate() const noexcept;    // states/s, 0 if not measurable
};

// What one frame says on its own: its header, its demodulated payload and the FEC attempt on
// it. Everything here depends on the frame alone, so it is computed outside the session's lock
// (FrameDecoder) and the session only records it (ReceiveSession::Commit).
struct DecodedFrame {
    bool readable = false;      // any usable cell at all
    bool header_ok = false;
    bool header_rescued_by_erasures = false;
    FrameHeader header{};
    std::size_t bits = 0;       // bits per cell the header names
    bool supported = false;     // a profile and code rate this grid can carry
    bool has_color = false;
    ColorDiagnostics color{};
    std::size_t payload_cells = 0;
    std::size_t erased_cells = 0;
    bool reached_fec = false;
    bool fec_ok = false;
    IntraFec::Stats fec{};
    std::size_t coded_len = 0;
    SoftSymbolBuffer soft;              // kept only when FEC failed: the session may combine it
    std::vector<std::uint8_t> message;  // corrected payload (or the raw bytes without FEC)
    // Set by a caller that already tried combining this frame with the earlier captures of its
    // state, outside the session's lock (TryCombine): Commit then only records the outcome.
    bool combine_tried = false;
    bool combined = false;  // ...and it worked: fec_ok and message come from the combination
};

// Soft combining (F68): `soft` summed with each of `earlier` -- captures of the same display
// state that failed FEC alone -- then with all of them, and decoded. The corrected payload on the
// first that succeeds. Pure: safe on any lane, outside any lock.
[[nodiscard]] Result<std::vector<std::uint8_t>> TryCombine(std::span<const SoftSymbolBuffer> earlier,
                                                           const SoftSymbolBuffer& soft,
                                                           const IntraFec& fec, std::size_t coded_len,
                                                           Llr erase_below, IntraFec::Stats* stats);

// The frame-local half of receiving: header, demodulation, FEC. One per decode lane, so several
// lanes decode at once -- the session's lock then covers only the bookkeeping.
//
// That split is the difference on a phone with slower cores. Demodulating eight colours (k-means
// plus the bleed model) and running FEC took ~17 ms of a Pixel 8 core per frame, all of it under
// the one lock four lanes share: lanes queued for it, each frame took 85-105 ms end to end, and
// the receiver decoded 23-33 frames a second -- 15-20 of the sender's 30 states (F69).
class FrameDecoder {
  public:
    FrameDecoder(const FrameLayout& layout, ReceiveConfig cfg = {})
        : mod_(layout), color8_(layout, ModulationProfile::kM5EightColor),
          color4_(layout, ModulationProfile::kM3FourColor), cfg_(cfg) {}

    [[nodiscard]] DecodedFrame Decode(std::span<const double> cell_samples,
                                      std::span<const double> cell_rgb);

    // The FEC for a code rate and profile; nullptr when this grid cannot carry it.
    const IntraFec* FecFor(std::size_t nsym, std::size_t bits_per_cell);

  private:
    M0Modulator mod_;
    ColorModulator color8_;
    ColorModulator color4_;
    HeaderCodec hdr_;
    ReceiveConfig cfg_;
    // By (nsym, bits per cell); nullopt = unsupported. Built lazily, per decoder.
    std::map<std::pair<std::size_t, std::size_t>, std::optional<IntraFec>> fec_;
};

class ReceiveSession {
  public:
    // `layout` must outlive the session. The grid is the ONE thing the receiver must be told:
    // it cannot read a header without already knowing where the header cells are.
    static Result<ReceiveSession> Create(const FrameLayout& layout, ReceiveConfig cfg = {});

    // One captured frame's photometrically normalised cell samples (FramePipeline output, NaN =
    // erased). `timestamp_ns` is the capture time, used only for rate accounting.
    //
    // `cell_rgb` is the frame's colour samples (CapturedFrame::cell_rgb), needed only for a frame
    // whose header names a colour profile (M3, M5); without them such a frame is
    // kPayloadUnsupported.
    FrameOutcome Ingest(std::span<const double> cell_samples, std::int64_t timestamp_ns) {
        return Ingest(cell_samples, {}, timestamp_ns);
    }
    FrameOutcome Ingest(std::span<const double> cell_samples, std::span<const double> cell_rgb,
                        std::int64_t timestamp_ns);

    // Records a frame some FrameDecoder has already decoded -- the same as Ingest, split so the
    // expensive half can run outside whatever lock guards this session.
    FrameOutcome Commit(DecodedFrame frame, std::int64_t timestamp_ns);

    // Copies of the failed captures held for this frame's display state, for TryCombine outside
    // the lock. Empty when there are none.
    [[nodiscard]] std::vector<SoftSymbolBuffer> EarlierCaptures(const FrameHeader& h) const;

    [[nodiscard]] bool has_manifest() const noexcept { return rx_.has_value(); }
    [[nodiscard]] const FileManifest* manifest() const noexcept {
        return rx_ ? &rx_->manifest() : nullptr;
    }
    [[nodiscard]] const FileReceiver* receiver() const noexcept { return rx_ ? &*rx_ : nullptr; }
    [[nodiscard]] bool complete() const noexcept { return rx_ && rx_->complete(); }
    [[nodiscard]] double progress() const noexcept { return rx_ ? rx_->progress() : 0.0; }
    [[nodiscard]] const ReceiveStats& stats() const noexcept { return stats_; }

    // The last frame that reached the intra-frame decoder, for the link controller (C14). False
    // when the last frame never got that far.
    [[nodiscard]] bool last_reached_fec() const noexcept { return last_reached_fec_; }
    [[nodiscard]] const IntraFec::Stats& last_fec_stats() const noexcept { return last_fec_; }
    // The last M5 frame's colour evidence: how far apart the eight colours sat, and how noisy.
    [[nodiscard]] const ColorDiagnostics& last_color() const noexcept { return last_color_; }
    // Bits per cell the last decoded header named: 1 black and white, 2 four colours, 3 eight.
    [[nodiscard]] std::size_t last_bits_per_cell() const noexcept { return last_bits_; }

    // Reassembles and VERIFIES (G3). kFountainIncomplete without a manifest as well.
    [[nodiscard]] Result<std::vector<std::uint8_t>> Finish();

  private:
    ReceiveSession(const FrameLayout& layout, ReceiveConfig cfg)
        : decoder_(layout, cfg), cfg_(cfg) {}

    // A frame whose payload failed FEC alone: tried again summed with the earlier failed captures
    // of the same display state, and kept for later ones if that fails too.
    Result<std::vector<std::uint8_t>> CombineWithEarlier(const FrameHeader& h,
                                                         const SoftSymbolBuffer& soft,
                                                         const IntraFec& fec, std::size_t coded_len);
    // Keeps a failed capture for later ones of its state to combine with; Forget drops a state
    // once one of its captures has decoded.
    void Remember(const FrameHeader& h, const SoftSymbolBuffer& soft);
    void Forget(const FrameHeader& h);
    FrameOutcome AcceptManifest(const FrameHeader& h, std::span<const std::uint8_t> bytes);
    void NoteSequence(std::uint32_t seq, std::int64_t ts);

    struct Held {
        FrameHeader header;
        std::vector<std::uint8_t> data;
    };
    // Failed captures of one display state, newest last.
    struct Failed {
        std::uint32_t session_id = 0;
        std::uint32_t sequence = 0;
        ModulationProfile profile{};
        std::uint8_t nsym = 0;
        std::vector<SoftSymbolBuffer> captures;
    };
    // The camera runs at about twice the state rate, so a state's captures arrive within a few
    // frames of each other -- out of order across decode lanes, but never far apart. A handful of
    // recent states is all that can still be matched.
    static constexpr std::size_t kFailedStates = 8;
    static constexpr std::size_t kCapturesPerState = 3;

    FrameDecoder decoder_;  // for Ingest, and the FEC that combining decodes with
    ReceiveConfig cfg_;
    ColorDiagnostics last_color_{};
    std::size_t last_bits_ = 0;
    std::optional<FileReceiver> rx_;
    std::vector<Held> held_;
    std::deque<Failed> failed_;
    std::set<std::uint32_t> seen_;
    bool any_sequence_ = false;
    ReceiveStats stats_;
    IntraFec::Stats last_fec_{};
    bool last_reached_fec_ = false;
};

}  // namespace fileflow
