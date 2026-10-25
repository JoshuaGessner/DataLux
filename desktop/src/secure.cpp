#include "secure.h"

#include <fileflow/compress.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fileflow::desktop::secure {

namespace {

constexpr std::string_view kLabel = "FileFlow pairing v1";
constexpr std::size_t kPointBytes = 65;
constexpr std::size_t kAckBytes = 8;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kProofBytes = 8;
constexpr std::uint8_t kFlagEncrypted = 2;
constexpr std::uint8_t kFlagBatch = 4;
constexpr std::size_t kMaxNameBytes = 255;
constexpr std::uint64_t kMaxOriginal = 512ULL * 1024 * 1024;

// One random generator for the process, seeded from the system's entropy, behind a lock (Mbed TLS
// is built without its own threading).
struct Rng {
    std::mutex lock;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    Rng() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static constexpr char kPersonal[] = "FileFlow desktop";
        mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, reinterpret_cast<const unsigned char*>(kPersonal),
                              sizeof kPersonal - 1);
    }
    static Rng& Get() {
        static Rng r;
        return r;
    }
};

int DrbgRandom(void* /*unused*/, unsigned char* out, std::size_t n) {
    Rng& r = Rng::Get();
    std::lock_guard g(r.lock);
    return mbedtls_ctr_drbg_random(&r.drbg, out, n);
}

Bytes Concat(std::initializer_list<const Bytes*> parts) {
    Bytes out;
    for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

Bytes Ascii(std::string_view s) { return {s.begin(), s.end()}; }

// HKDF-Expand (RFC 5869), as Pairing.expand.
Bytes Expand(const Bytes& prk, const Bytes& info, std::size_t length) {
    Bytes out;
    Bytes t;
    for (std::uint8_t i = 1; out.size() < length; ++i) {
        Bytes block = t;
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(i);
        t = HmacSha256(prk, block);
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

bool EqualCt(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint8_t d = 0;
    for (std::size_t i = 0; i < n; ++i) d |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return d == 0;
}

// AES-256-GCM, the tag appended -- as javax.crypto writes it.
Bytes GcmSeal(const Bytes& key, const std::uint8_t* nonce, const Bytes& aad, const Bytes& plain) {
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    Bytes out(plain.size() + kTagBytes);
    mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plain.size(), nonce, kNonceBytes, aad.data(), aad.size(),
                              plain.data(), out.data(), kTagBytes, out.data() + plain.size());
    mbedtls_gcm_free(&g);
    return out;
}

std::optional<Bytes> GcmOpen(const Bytes& key, const std::uint8_t* nonce, const Bytes& aad, const std::uint8_t* sealed,
                             std::size_t n) {
    if (n < kTagBytes) return std::nullopt;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    Bytes out(n - kTagBytes);
    const int rc = mbedtls_gcm_auth_decrypt(&g, out.size(), nonce, kNonceBytes, aad.data(), aad.size(), sealed + out.size(),
                                            kTagBytes, sealed, out.data());
    mbedtls_gcm_free(&g);
    if (rc != 0) return std::nullopt;
    return out;
}

struct Reader {
    const Bytes& b;
    std::size_t at = 0;
    bool ok = true;
    bool Has(std::size_t n) { return ok = ok && at + n <= b.size(); }
    std::uint8_t U8() { return Has(1) ? b[at++] : 0; }
    std::uint16_t U16() {
        if (!Has(2)) return 0;
        const auto v = static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
        at += 2;
        return v;
    }
    std::uint64_t U64() {
        if (!Has(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | b[at++];
        return v;
    }
    Bytes Take(std::size_t n) {
        if (!Has(n)) return {};
        Bytes v(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return v;
    }
    std::string Str() {  // a length byte, then UTF-8
        const Bytes s = Take(U8());
        return {s.begin(), s.end()};
    }
};

void PutU16(Bytes* b, std::size_t v) {
    b->push_back(static_cast<std::uint8_t>(v >> 8));
    b->push_back(static_cast<std::uint8_t>(v));
}
void PutU64(Bytes* b, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) b->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void PutStr(Bytes* b, std::string_view s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    b->push_back(static_cast<std::uint8_t>(n));
    b->insert(b->end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// Wire.members: count (u16) | count x [ name | joined ms (u64) | flags ].
void PutMembers(Bytes* b, const std::vector<Member>& list) {
    PutU16(b, list.size());
    for (const auto& m : list) {
        PutStr(b, m.name);
        PutU64(b, static_cast<std::uint64_t>(m.joined_ms));
        b->push_back(static_cast<std::uint8_t>((m.leader ? 1 : 0) | (m.you ? 2 : 0) | (m.needs_new_key ? 4 : 0)));
    }
}
std::vector<Member> ReadMembers(Reader* r) {
    std::vector<Member> out;
    const std::uint16_t n = r->U16();
    for (std::uint16_t i = 0; i < n && r->ok; ++i) {
        Member m;
        m.name = Team::CleanName(r->Str());
        m.joined_ms = static_cast<std::int64_t>(r->U64());
        const std::uint8_t f = r->U8();
        m.leader = (f & 1) != 0;
        m.you = (f & 2) != 0;
        m.needs_new_key = (f & 4) != 0;
        out.push_back(std::move(m));
    }
    return out;
}

// What a team code is sealed with: the code's own prefix and the pairing's id, bound in so a code
// can't be passed off as the other kind, or for another pairing.
Bytes CodeAad(std::string_view prefix, const PairingKey& pairing) {
    Bytes aad = Ascii(prefix);
    aad.insert(aad.end(), pairing.id.begin(), pairing.id.end());
    return aad;
}

Bytes SealWith(const Bytes& plain, const PairingKey& pairing, std::string_view prefix, const Random& random) {
    std::uint8_t nonce[kNonceBytes];
    random(nonce, sizeof nonce);
    const Bytes aad = CodeAad(prefix, pairing);
    Bytes out(nonce, nonce + kNonceBytes);
    const Bytes sealed = GcmSeal(pairing.key, nonce, aad, plain);
    out.insert(out.end(), sealed.begin(), sealed.end());
    return out;
}

std::optional<Bytes> OpenWith(std::string_view body, const PairingKey& pairing, std::string_view prefix) {
    const auto bytes = UnB64Url(body);
    if (!bytes || bytes->size() < kNonceBytes + kTagBytes) return std::nullopt;
    const Bytes aad = CodeAad(prefix, pairing);
    return GcmOpen(pairing.key, bytes->data(), aad, bytes->data() + kNonceBytes, bytes->size() - kNonceBytes);
}

Bytes Proof(const Team& team, const PairingKey& pairing) {
    Bytes msg = Ascii("FileFlow team joined");
    msg.insert(msg.end(), pairing.id.begin(), pairing.id.end());
    Bytes h = HmacSha256(team.key, msg);
    h.resize(kProofBytes);
    return h;
}

std::string Hex(const Bytes& b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    for (auto v : b) {
        s += kDigits[v >> 4];
        s += kDigits[v & 15];
    }
    return s;
}

std::optional<Bytes> UnHex(std::string_view s) {
    if (s.size() % 2 != 0) return std::nullopt;
    Bytes out;
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const auto d = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = d(s[i]);
        const int lo = d(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return out;
}

}  // namespace

void SystemRandom(std::uint8_t* out, std::size_t n) { DrbgRandom(nullptr, out, n); }

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string B64Url(const Bytes& b) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    std::size_t i = 0;
    for (; i + 3 <= b.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(b[i]) << 16) | (static_cast<std::uint32_t>(b[i + 1]) << 8) | b[i + 2];
        for (int k = 18; k >= 0; k -= 6) s += kAlphabet[(v >> k) & 63];
    }
    if (b.size() - i == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(b[i]) << 16;
        s += kAlphabet[(v >> 18) & 63];
        s += kAlphabet[(v >> 12) & 63];
    } else if (b.size() - i == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(b[i]) << 16) | (static_cast<std::uint32_t>(b[i + 1]) << 8);
        s += kAlphabet[(v >> 18) & 63];
        s += kAlphabet[(v >> 12) & 63];
        s += kAlphabet[(v >> 6) & 63];
    }
    return s;
}

std::optional<Bytes> UnB64Url(std::string_view s) {
    const auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    if (s.size() % 4 == 1) return std::nullopt;
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = val(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

Bytes Sha256(const Bytes& b) {
    Bytes out(32);
    mbedtls_sha256(b.data(), b.size(), out.data(), 0);
    return out;
}

Bytes HmacSha256(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key.data(), key.size(), data.data(), data.size(), out.data());
    return out;
}

PairingKey Derive(const Bytes& shared, const Bytes& mine, const Bytes& peer, std::int64_t now_ms) {
    const bool mine_first = std::lexicographical_compare(mine.begin(), mine.end(), peer.begin(), peer.end()) || mine == peer;
    const Bytes& lo = mine_first ? mine : peer;
    const Bytes& hi = mine_first ? peer : mine;
    const Bytes label = Ascii(kLabel);
    const Bytes salt = Sha256(Concat({&label, &lo, &hi}));
    const Bytes prk = HmacSha256(salt, shared);
    const Bytes okm = Expand(prk, Ascii("FileFlow v1 keys"), 32 + kIdBytes + 4);
    PairingKey k;
    k.key.assign(okm.begin(), okm.begin() + 32);
    k.id.assign(okm.begin() + 32, okm.begin() + 40);
    const std::uint32_t n = (static_cast<std::uint32_t>(okm[40]) << 24) | (static_cast<std::uint32_t>(okm[41]) << 16) |
                            (static_cast<std::uint32_t>(okm[42]) << 8) | okm[43];
    char code[8];
    std::snprintf(code, sizeof code, "%06u", static_cast<unsigned>(n % 1000000U));
    k.code = std::string(code, 3) + " " + std::string(code + 3, 3);
    k.label = "paired phone";
    k.created_ms = now_ms;
    return k;
}

// ------------------------------------------------------------------------------------- pairing

struct Pairing::Impl {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    Bytes point;
    std::optional<Bytes> peer;
    Impl() {
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&d);
        mbedtls_ecp_point_init(&q);
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
        mbedtls_ecp_gen_keypair(&grp, &d, &q, DrbgRandom, nullptr);
        point.resize(kPointBytes);
        std::size_t olen = 0;
        mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, point.data(), point.size());
    }
    ~Impl() {
        mbedtls_ecp_point_free(&q);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&grp);
    }
    // On the curve and not the identity: an invalid point is the classic attack on ECDH.
    bool Valid(const Bytes& p) {
        mbedtls_ecp_point pt;
        mbedtls_ecp_point_init(&pt);
        const bool ok = mbedtls_ecp_point_read_binary(&grp, &pt, p.data(), p.size()) == 0 && mbedtls_ecp_check_pubkey(&grp, &pt) == 0;
        mbedtls_ecp_point_free(&pt);
        return ok;
    }
};

Pairing::Pairing() : impl_(std::make_unique<Impl>()) {}
Pairing::~Pairing() = default;
std::unique_ptr<Pairing> Pairing::Start() { return std::unique_ptr<Pairing>(new Pairing()); }

std::string Pairing::QrText() const {
    std::string s = std::string(kPrefix) + B64Url(impl_->point);
    if (impl_->peer) {
        Bytes ack = Sha256(*impl_->peer);
        ack.resize(kAckBytes);
        s += ":" + B64Url(ack);
    }
    return s;
}

Pairing::Read Pairing::ReadCode(std::string_view text) {
    if (text.substr(0, kPrefix.size()) != kPrefix) return Read::kNotOurs;
    text.remove_prefix(kPrefix.size());
    const auto colon = text.find(':');
    const auto point = UnB64Url(text.substr(0, colon));
    if (!point || point->size() != kPointBytes || (*point)[0] != 4) return Read::kNotOurs;
    // Our own code reflected back -- a mirror, a window -- is not a peer.
    if (*point == impl_->point) return Read::kOwn;
    if (impl_->peer && *impl_->peer != *point) return Read::kNotOurs;  // a third phone
    if (!impl_->Valid(*point)) return Read::kNotOurs;
    impl_->peer = *point;
    bool acked = false;
    if (colon != std::string_view::npos) {
        const auto ack = UnB64Url(text.substr(colon + 1));
        Bytes mine = Sha256(impl_->point);
        mine.resize(kAckBytes);
        acked = ack && *ack == mine;
    }
    return acked ? Read::kPeerComplete : Read::kPeerKey;
}

std::optional<PairingKey> Pairing::Key(std::int64_t now_ms) const {
    if (!impl_->peer) return std::nullopt;
    mbedtls_ecp_point p;
    mbedtls_ecp_point_init(&p);
    mbedtls_mpi z;
    mbedtls_mpi_init(&z);
    std::optional<PairingKey> out;
    if (mbedtls_ecp_point_read_binary(&impl_->grp, &p, impl_->peer->data(), impl_->peer->size()) == 0 &&
        mbedtls_ecdh_compute_shared(&impl_->grp, &z, &p, &impl_->d, DrbgRandom, nullptr) == 0) {
        Bytes shared(32);
        mbedtls_mpi_write_binary(&z, shared.data(), shared.size());
        out = Derive(shared, impl_->point, *impl_->peer, now_ms);
        std::fill(shared.begin(), shared.end(), 0);
    }
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&p);
    return out;
}

// ------------------------------------------------------------------------------------ envelopes

Bytes Seal(const Bytes& plain, const SealKey& key, const Random& random) {
    // The plain envelope is "FFE2" | flags | inner; sealed, the header gains the key id and nonce,
    // and inner is encrypted with the whole header as associated data.
    Bytes header(plain.begin(), plain.begin() + 4);
    header.push_back(static_cast<std::uint8_t>(plain[4] | kFlagEncrypted));
    header.insert(header.end(), key.id.begin(), key.id.end());
    std::uint8_t nonce[kNonceBytes];
    random(nonce, sizeof nonce);
    header.insert(header.end(), nonce, nonce + kNonceBytes);
    const Bytes inner(plain.begin() + 5, plain.end());
    const Bytes sealed = GcmSeal(key.key, nonce, header, inner);
    Bytes out = header;
    out.insert(out.end(), sealed.begin(), sealed.end());
    return out;
}

std::string Describe(OpenFailure f) {
    switch (f) {
        case OpenFailure::kNone: return "";
        case OpenFailure::kNotAnEnvelope: return "This isn't a FileFlow file.";
        case OpenFailure::kNeedsPairing: return "This file is encrypted. Pair with the sender, or join their team, then have it sent again.";
        case OpenFailure::kWrongKey: return "This file is encrypted for a pairing or team this computer isn't in, or was altered. Nothing was saved.";
        case OpenFailure::kNotEncrypted: return "The sender isn't in encrypted mode, so the file was refused.";
        case OpenFailure::kUnknownCodec: return "This file was compressed in a way this version can't open. Update FileFlow.";
        case OpenFailure::kCorrupt: return "The file is damaged. Nothing was saved.";
    }
    return "";
}

OpenFailure Open(const Bytes& bytes, const std::vector<SealKey>& keys, bool require_encrypted, Opened* out) {
    if (bytes.size() < 5 || !(std::equal(bytes.begin(), bytes.begin() + 4, "FFE2") || std::equal(bytes.begin(), bytes.begin() + 4, "FFE1"))) {
        return OpenFailure::kNotAnEnvelope;
    }
    const bool v1 = bytes[3] == '1';
    const std::uint8_t flags = bytes[4];
    const std::uint8_t known = static_cast<std::uint8_t>(kFlagEncrypted | (v1 ? 1 : kFlagBatch));
    if ((flags & ~known) != 0) return OpenFailure::kCorrupt;
    const bool encrypted = (flags & kFlagEncrypted) != 0;
    if (require_encrypted && !encrypted) return OpenFailure::kNotEncrypted;
    Bytes inner;
    if (encrypted) {
        const std::size_t header = 5 + kIdBytes + kNonceBytes;
        if (bytes.size() < header + kTagBytes) return OpenFailure::kCorrupt;
        if (keys.empty()) return OpenFailure::kNeedsPairing;
        const SealKey* key = nullptr;
        for (const auto& k : keys) {
            if (k.id.size() == kIdBytes && EqualCt(k.id.data(), bytes.data() + 5, kIdBytes)) key = &k;
        }
        if (key == nullptr) return OpenFailure::kWrongKey;
        const Bytes aad(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header));
        auto opened = GcmOpen(key->key, bytes.data() + 5 + kIdBytes, aad, bytes.data() + header, bytes.size() - header);
        if (!opened) return OpenFailure::kWrongKey;
        inner = std::move(*opened);
        out->sealed_to = key->label;
    } else {
        inner.assign(bytes.begin() + 5, bytes.end());
    }
    Reader r{inner};
    const std::uint16_t name_len = r.U16();
    if (name_len > kMaxNameBytes) return OpenFailure::kCorrupt;
    const Bytes name = r.Take(name_len);
    const std::uint64_t original = r.U64();
    if (!r.ok || original > kMaxOriginal) return OpenFailure::kCorrupt;
    std::uint8_t codec = 0;
    std::uint32_t compress_ms = 0;
    if (v1) {
        codec = (flags & 1) != 0 ? 1 : 0;
    } else {
        codec = r.U8();
        for (int i = 0; i < 4; ++i) compress_ms = (compress_ms << 8) | r.U8();
    }
    if (!r.ok) return OpenFailure::kCorrupt;
    const std::span<const std::uint8_t> body(inner.data() + r.at, inner.size() - r.at);
    Bytes data;
    if (codec == 0) {
        data.assign(body.begin(), body.end());
    } else if (codec == 2) {
        auto d = Decompress(body, static_cast<std::size_t>(original));
        if (!d.ok()) return OpenFailure::kCorrupt;
        data = std::move(d).value();
    } else {
        return OpenFailure::kUnknownCodec;  // deflate: only the first release's phones sent it
    }
    if (data.size() != original) return OpenFailure::kCorrupt;
    out->name.assign(name.begin(), name.end());
    out->encrypted = encrypted;
    out->codec = codec;
    out->compress_ms = compress_ms;
    if ((flags & kFlagBatch) != 0 && !v1) {
        // "FFB1" | count | count x [ name length (u16) | name | size (u64) | bytes ], every length checked.
        Reader b{data};
        const Bytes magic = b.Take(4);
        if (magic != Ascii("FFB1")) return OpenFailure::kCorrupt;
        const std::uint16_t count = b.U16();
        if (count == 0 || count > 1000) return OpenFailure::kCorrupt;
        std::vector<std::pair<std::string, Bytes>> files;
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::uint16_t nl = b.U16();
            if (nl > kMaxNameBytes) return OpenFailure::kCorrupt;
            const Bytes n = b.Take(nl);
            const std::uint64_t size = b.U64();
            if (!b.ok || size > data.size() - b.at) return OpenFailure::kCorrupt;
            files.emplace_back(std::string(n.begin(), n.end()), b.Take(static_cast<std::size_t>(size)));
        }
        if (!b.ok || b.at != data.size()) return OpenFailure::kCorrupt;
        out->files = std::move(files);
    }
    out->data = std::move(data);
    return OpenFailure::kNone;
}

// ---------------------------------------------------------------------------------------- teams

std::string Team::CleanName(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) out += c;
    }
    const auto a = out.find_first_not_of(' ');
    const auto b = out.find_last_not_of(' ');
    out = a == std::string::npos ? "" : out.substr(a, b - a + 1);
    // At most 40 characters (code points, as Kotlin's take counts UTF-16 units -- the same for the
    // names people type), cut on a character boundary.
    std::size_t chars = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if ((static_cast<unsigned char>(out[i]) & 0xC0) != 0x80 && ++chars > kMaxNameChars) return out.substr(0, i);
    }
    return out;
}

Team Team::Create(std::string_view name, std::string_view my_name, std::int64_t now_ms, const Random& random) {
    Team t;
    t.id.resize(kIdBytes);
    random(t.id.data(), t.id.size());
    t.key.resize(32);
    random(t.key.data(), t.key.size());
    t.name = CleanName(name);
    t.created_ms = now_ms;
    t.is_leader = true;
    t.members.push_back({CleanName(my_name), now_ms, true, true, false});
    return t;
}

std::string Team::IdHex() const { return Hex(id); }

SealKey Team::AsSealKey() const { return {key, id, "team " + name}; }

const Member* Team::Leader() const {
    for (const auto& m : members) {
        if (m.leader) return &m;
    }
    return nullptr;
}

Team Team::WithMember(Member m) const {
    Team t = *this;
    std::erase_if(t.members, [&](const Member& o) { return !o.leader && !o.you && o.name == m.name && o.needs_new_key; });
    t.members.push_back(std::move(m));
    return t;
}

namespace invite {

bool IsTeamCode(std::string_view text) { return text.substr(0, 6) == "FFTEAM"; }

std::string Encode(const Team& team, const PairingKey& pairing, const Random& random) {
    Bytes plain = team.id;
    plain.insert(plain.end(), team.key.begin(), team.key.end());
    PutU64(&plain, static_cast<std::uint64_t>(team.created_ms));
    PutStr(&plain, team.name);
    const Member* leader = team.Leader();
    PutStr(&plain, leader != nullptr ? leader->name : "");
    std::vector<Member> listed;
    for (const auto& m : team.members) {
        if (!m.leader && !m.needs_new_key) listed.push_back({m.name, m.joined_ms});
    }
    if (listed.size() > kMaxListed) listed.erase(listed.begin(), listed.end() - static_cast<std::ptrdiff_t>(kMaxListed));
    PutMembers(&plain, listed);
    return std::string(kPrefix) + B64Url(SealWith(plain, pairing, kPrefix, random));
}

std::optional<Team> Decode(std::string_view text, const PairingKey& pairing, std::string_view my_name, std::int64_t now_ms) {
    if (text.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    const auto plain = OpenWith(text.substr(kPrefix.size()), pairing, kPrefix);
    if (!plain) return std::nullopt;
    Reader r{*plain};
    Team t;
    t.id = r.Take(kIdBytes);
    t.key = r.Take(32);
    t.created_ms = static_cast<std::int64_t>(r.U64());
    t.name = Team::CleanName(r.Str());
    const std::string leader = Team::CleanName(r.Str());
    auto others = ReadMembers(&r);
    if (!r.ok || r.at != plain->size() || t.name.empty()) return std::nullopt;
    t.is_leader = false;
    t.members.push_back({leader, t.created_ms, true, false, false});
    for (auto& m : others) t.members.push_back({m.name, m.joined_ms});
    t.members.push_back({Team::CleanName(my_name), now_ms, false, true, false});
    return t;
}

std::string Ack(const Team& team, const PairingKey& pairing, std::string_view my_name, const Random& random) {
    Bytes plain = Proof(team, pairing);
    PutStr(&plain, Team::CleanName(my_name));
    return std::string(kAckPrefix) + B64Url(SealWith(plain, pairing, kAckPrefix, random));
}

std::optional<std::string> ReadAck(std::string_view text, const Team& team, const PairingKey& pairing) {
    if (text.substr(0, kAckPrefix.size()) != kAckPrefix) return std::nullopt;
    const auto plain = OpenWith(text.substr(kAckPrefix.size()), pairing, kAckPrefix);
    if (!plain || plain->size() < kProofBytes + 1) return std::nullopt;
    const Bytes proof = Proof(team, pairing);
    if (!EqualCt(plain->data(), proof.data(), kProofBytes)) return std::nullopt;
    Reader r{*plain};
    r.at = kProofBytes;
    std::string name = Team::CleanName(r.Str());
    if (!r.ok || r.at != plain->size()) return std::nullopt;
    return name;
}

}  // namespace invite

// One line per team: id, name, key, created ms, "L"/"M", members -- hex and base64url so a line
// holds no tab or newline of its own.
std::vector<Team> TeamStore::All() {
    if (cache_) return *cache_;
    std::vector<Team> out;
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::size_t a = 0;
        for (std::size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
        f.push_back(line.substr(a));
        if (f.size() != 6) continue;
        Team t;
        auto id = UnHex(f[0]);
        auto name = UnB64Url(f[1]);
        auto key = UnHex(f[2]);
        auto members = UnB64Url(f[5]);
        if (!id || !name || !key || !members || id->size() != kIdBytes || key->size() != 32) continue;
        t.id = *id;
        t.name = std::string(name->begin(), name->end());
        t.key = *key;
        t.created_ms = std::strtoll(f[3].c_str(), nullptr, 10);
        t.is_leader = f[4] == "L";
        Reader r{*members};
        t.members = ReadMembers(&r);
        out.push_back(std::move(t));
    }
    cache_ = out;
    return out;
}

void TeamStore::Add(const Team& team) {
    auto all = All();
    std::erase_if(all, [&](const Team& t) { return t.id == team.id; });
    all.push_back(team);
    Save(all);
}

void TeamStore::Remove(const std::string& id_hex) {
    auto all = All();
    std::erase_if(all, [&](const Team& t) { return t.IdHex() == id_hex; });
    Save(all);
}

void TeamStore::Save(const std::vector<Team>& teams) {
    {
        std::ofstream out(path_, std::ios::trunc);
        for (const auto& t : teams) {
            Bytes members;
            PutMembers(&members, t.members);
            out << Hex(t.id) << '\t' << B64Url(Ascii(t.name)) << '\t' << Hex(t.key) << '\t' << t.created_ms << '\t'
                << (t.is_leader ? "L" : "M") << '\t' << B64Url(members) << '\n';
        }
    }
#if !defined(_WIN32)
    chmod(path_.c_str(), 0600);  // this user's alone
#endif
    cache_ = teams;
}

// --------------------------------------------------------------------------------- pairing flow

PairingFlow::PairingFlow(Mode mode, std::string my_name, std::optional<Team> team)
    : mode_(mode), my_name_(std::move(my_name)), team_(std::move(team)), pairing_(Pairing::Start()) {
    qr_ = pairing_->QrText();
}

bool PairingFlow::Read(std::string_view text) {
    if (stage_ == Stage::kDone) return false;
    if (invite::IsTeamCode(text)) return ReadTeamCode(text);
    switch (pairing_->ReadCode(text)) {
        case Pairing::Read::kNotOurs:
        case Pairing::Read::kOwn: return false;
        case Pairing::Read::kPeerKey: return GotKey(false);
        case Pairing::Read::kPeerComplete: return GotKey(true);
    }
    return false;
}

bool PairingFlow::GotKey(bool confirmed) {
    const bool first = !key_.has_value();
    if (first) key_ = pairing_->Key(NowMs());
    if (!key_) return false;
    const Stage before = stage_;
    // Our code now acknowledges theirs -- what completes THEIR side -- unless it has moved on to
    // the team key, which must stay up until the member reads it.
    if (stage_ != Stage::kHandingOver) qr_ = pairing_->QrText();
    if (!confirmed) {
        if (stage_ == Stage::kScanning) stage_ = Stage::kGotKey;
    } else if (mode_ == Mode::kPair) {
        stage_ = Stage::kDone;
    } else if (mode_ == Mode::kInvite) {
        if (stage_ != Stage::kHandingOver) qr_ = invite::Encode(*team_, *key_);
        stage_ = Stage::kHandingOver;
    } else if (stage_ == Stage::kScanning || stage_ == Stage::kGotKey) {
        stage_ = Stage::kWaitingForTeam;
    }
    return first || stage_ != before;
}

bool PairingFlow::ReadTeamCode(std::string_view text) {
    if (!key_) return false;
    if (mode_ == Mode::kInvite) {
        auto name = invite::ReadAck(text, *team_, *key_);
        if (!name) return false;
        member_name_ = std::move(name);
        stage_ = Stage::kDone;
        return true;
    }
    if (mode_ != Mode::kJoin || text.substr(0, invite::kPrefix.size()) != invite::kPrefix) return false;
    // The leader shows the team only once it has OUR acknowledgement of ITS key; opening the team
    // proves the leader holds the same pairing key.
    auto team = invite::Decode(text, *key_, my_name_, NowMs());
    if (!team) {
        if (foreign_invite_) return false;
        foreign_invite_ = true;
        return true;
    }
    joined_ = std::move(team);
    qr_ = invite::Ack(*joined_, *key_, my_name_);
    stage_ = Stage::kDone;
    return true;
}

}  // namespace fileflow::desktop::secure
