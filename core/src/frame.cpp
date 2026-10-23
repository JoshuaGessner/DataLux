#include <fileflow/frame.h>

#include <fileflow/bytes.h>
#include <fileflow/hash.h>

namespace fileflow {

std::vector<std::uint8_t> FrameHeader::EncodePlain() const {
    ByteWriter w(kPlainSize);
    w.U8(kFrameMagic);
    w.U8(version);
    w.U32(session_id);
    w.U32(sequence);
    w.U8(static_cast<std::uint8_t>(profile));
    w.U8(phase);
    w.U32(block_id);
    w.U32(esi);
    w.U32(payload_bytes);
    w.U8(flags);
    w.U8(fec_nsym);
    w.U32(payload_crc);
    const std::uint32_t crc = Crc32(w.data());
    w.U32(crc);
    return std::move(w).take();
}

Result<FrameHeader> FrameHeader::DecodePlain(std::span<const std::uint8_t> in) {
    if (in.size() < kPlainSize) return Error::kTruncated;

    // CRC first: nothing in the header is trusted until it verifies. This also catches
    // RS MISCORRECTION, where the code "successfully" decodes to a wrong codeword
    // (docs/security/THREAT-MODEL.md T5).
    const std::span<const std::uint8_t> body = in.subspan(0, kPlainSize - 4);
    const std::uint32_t want = Crc32(body);
    ByteReader crc_r(in.subspan(kPlainSize - 4, 4));
    auto got = crc_r.U32();
    if (!got.ok()) return got.error();
    if (got.value() != want) return Error::kHeaderCrcMismatch;

    ByteReader r(body);
    FrameHeader h;

    auto magic = r.U8();
    if (!magic.ok()) return magic.error();
    if (magic.value() != kFrameMagic) return Error::kBadMagic;

    auto ver = r.U8();
    if (!ver.ok()) return ver.error();
    // Forward compatibility: a HIGHER major version is refused cleanly rather than
    // misparsed (PROTOCOL-SPEC). Never guess at an unknown layout.
    if (ver.value() > kProtocolVersion) return Error::kUnsupportedVersion;
    // v1 had a different layout and fountain code (ADR-0015). Its CRC would not verify against
    // this layout anyway, but say what it is rather than leaving it to a checksum to imply.
    if (ver.value() < kProtocolVersion) return Error::kUnsupportedVersion;
    h.version = ver.value();

    auto sid = r.U32();
    if (!sid.ok()) return sid.error();
    h.session_id = sid.value();

    auto seq = r.U32();
    if (!seq.ok()) return seq.error();
    h.sequence = seq.value();

    auto prof = r.U8();
    if (!prof.ok()) return prof.error();
    if (prof.value() > static_cast<std::uint8_t>(ModulationProfile::kM4MixedFrame)) {
        // Unknown profile: skip the FRAME, do not abort the session (PROTOCOL-SPEC).
        return Error::kUnknownProfile;
    }
    h.profile = static_cast<ModulationProfile>(prof.value());

    auto ph = r.U8();
    if (!ph.ok()) return ph.error();
    h.phase = ph.value();

    auto bid = r.U32();
    if (!bid.ok()) return bid.error();
    h.block_id = bid.value();

    auto esi = r.U32();
    if (!esi.ok()) return esi.error();
    h.esi = esi.value();

    auto plen = r.U32();
    if (!plen.ok()) return plen.error();
    // Bound BEFORE this value is used to size anything (INPUT-VALIDATION SR-1).
    if (plen.value() > kMaxPayloadBytes) return Error::kValueOutOfRange;
    h.payload_bytes = plen.value();

    auto fl = r.U8();
    if (!fl.ok()) return fl.error();
    h.flags = fl.value();

    auto nsym = r.U8();
    if (!nsym.ok()) return nsym.error();
    // An intra-frame code with nsym >= 255 has no message bytes at all.
    if (nsym.value() >= 255) return Error::kValueOutOfRange;
    h.fec_nsym = nsym.value();

    auto pcrc = r.U32();
    if (!pcrc.ok()) return pcrc.error();
    h.payload_crc = pcrc.value();

    return h;
}

Result<std::vector<std::uint8_t>> HeaderCodec::Encode(const FrameHeader& h) const {
    const std::vector<std::uint8_t> plain = h.EncodePlain();
    return rs_.Encode(plain);
}

Result<FrameHeader> HeaderCodec::Decode(std::span<std::uint8_t> coded,
                                        std::span<const std::size_t> erasures) const {
    if (coded.size() != coded_size()) return Error::kLengthMismatch;
    for (const std::size_t pos : erasures) {
        if (pos >= coded.size()) return Error::kValueOutOfRange;
    }

    // RS correction may fail; that is a normal outcome on a bad channel, not an error
    // condition to log loudly. The frame simply becomes an erasure.
    auto attempt = [&](std::span<std::uint8_t> cw,
                       std::span<const std::size_t> er) -> Result<FrameHeader> {
        auto corrected = rs_.Decode(cw, er);
        if (!corrected.ok()) return corrected.error();
        return FrameHeader::DecodePlain(cw.subspan(0, FrameHeader::kPlainSize));
    };

    if (erasures.empty() || erasures.size() > nsym_) return attempt(coded, {});

    const std::vector<std::uint8_t> original(coded.begin(), coded.end());
    auto with = attempt(coded, erasures);
    if (with.ok()) return with;
    std::copy(original.begin(), original.end(), coded.begin());
    auto without = attempt(coded, {});
    return without.ok() ? without : with;
}

}  // namespace fileflow
