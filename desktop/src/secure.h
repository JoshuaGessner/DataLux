// Encrypted mode on the desktop: the Android app's secure/ package, in C++, byte for byte -- pairing
// (ECDH P-256, HKDF-SHA256, the six-digit code), sealed envelopes (AES-256-GCM), teams and their
// invites -- so a computer and a phone pair, join each other's teams and open each other's files.
// Held to the app's own vectors (desktop/tests/test_secure.cpp, SecureVectorsTest.kt). Mbed TLS
// does the cryptography.
#pragma once

#include <fileflow/result.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fileflow::desktop::secure {

using Bytes = std::vector<std::uint8_t>;
// Fills a buffer with random bytes: the system's, or -- in a test -- a fixed pattern.
using Random = std::function<void(std::uint8_t*, std::size_t)>;
void SystemRandom(std::uint8_t* out, std::size_t n);

std::string B64Url(const Bytes& b);
std::optional<Bytes> UnB64Url(std::string_view s);
Bytes Sha256(const Bytes& b);
Bytes HmacSha256(const Bytes& key, const Bytes& data);
std::int64_t NowMs();

inline constexpr std::size_t kIdBytes = 8;

// A key an envelope can be sealed to: a pairing's, or a team's.
struct SealKey {
    Bytes key;  // 32 bytes, AES-256
    Bytes id;   // 8 bytes, in the clear in every envelope sealed to it
    std::string label;  // "paired phone", "team Crew"
};

struct PairingKey : SealKey {
    std::string code;  // "482 913": the same on both sides, or someone else's key was read
    std::int64_t created_ms = 0;
};

// Pairing.derive: the key, id and code from an ECDH secret and both public points.
PairingKey Derive(const Bytes& shared, const Bytes& mine, const Bytes& peer, std::int64_t now_ms);

// One side of a pairing (Pairing.kt): a fresh P-256 key, shown as a QR code, and the peer's read.
class Pairing {
  public:
    static std::unique_ptr<Pairing> Start();
    ~Pairing();
    Pairing(const Pairing&) = delete;
    Pairing& operator=(const Pairing&) = delete;

    static constexpr std::string_view kPrefix = "FFPAIR1:";
    enum class Read { kNotOurs, kOwn, kPeerKey, kPeerComplete };

    [[nodiscard]] std::string QrText() const;
    Read ReadCode(std::string_view text);
    [[nodiscard]] std::optional<PairingKey> Key(std::int64_t now_ms) const;

  private:
    Pairing();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Envelopes (Envelope.kt): sealing a plain envelope from core's PackEnvelope, and opening any.
// "FFE2" | flags (2 sealed, 4 batch) | [key id | nonce] | body, the header bound in as GCM's AAD.
Bytes Seal(const Bytes& plain_envelope, const SealKey& key, const Random& random = SystemRandom);

struct Opened {
    std::string name;
    Bytes data;
    bool encrypted = false;
    std::uint8_t codec = 0;
    std::uint32_t compress_ms = 0;
    std::optional<std::vector<std::pair<std::string, Bytes>>> files;  // a batch's
    std::string sealed_to;  // the key's label, when it was sealed
};
enum class OpenFailure { kNone, kNotAnEnvelope, kNeedsPairing, kWrongKey, kNotEncrypted, kUnknownCodec, kCorrupt };
std::string Describe(OpenFailure f);
OpenFailure Open(const Bytes& bytes, const std::vector<SealKey>& keys, bool require_encrypted, Opened* out);

// Teams (Team.kt).
struct Member {
    std::string name;
    std::int64_t joined_ms = 0;
    bool leader = false;
    bool you = false;
    bool needs_new_key = false;
};

struct Team {
    Bytes id;   // 8
    std::string name;
    Bytes key;  // 32
    std::int64_t created_ms = 0;
    bool is_leader = true;
    std::vector<Member> members;

    static constexpr std::size_t kMaxNameChars = 40;
    static std::string CleanName(std::string_view s);
    static Team Create(std::string_view name, std::string_view my_name, std::int64_t now_ms, const Random& random = SystemRandom);
    [[nodiscard]] std::string IdHex() const;
    [[nodiscard]] SealKey AsSealKey() const;
    [[nodiscard]] const Member* Leader() const;
    [[nodiscard]] Team WithMember(Member m) const;
};

namespace invite {
inline constexpr std::string_view kPrefix = "FFTEAM2:";
inline constexpr std::string_view kAckPrefix = "FFTEAMOK2:";
inline constexpr std::size_t kMaxListed = 12;
bool IsTeamCode(std::string_view text);
std::string Encode(const Team& team, const PairingKey& pairing, const Random& random = SystemRandom);
std::optional<Team> Decode(std::string_view text, const PairingKey& pairing, std::string_view my_name, std::int64_t now_ms);
std::string Ack(const Team& team, const PairingKey& pairing, std::string_view my_name, const Random& random = SystemRandom);
std::optional<std::string> ReadAck(std::string_view text, const Team& team, const PairingKey& pairing);
}  // namespace invite

// The teams this computer is in, in a file in the app's preferences folder, readable only by this
// user. (Not wrapped by a hardware key as on the phone -- see SECURITY.md.)
class TeamStore {
  public:
    explicit TeamStore(std::string path) : path_(std::move(path)) {}
    std::vector<Team> All();
    void Add(const Team& t);
    void Remove(const std::string& id_hex);

  private:
    void Save(const std::vector<Team>& teams);
    std::string path_;
    std::optional<std::vector<Team>> cache_;
};

// What one side does on the pairing screen (PairingFlow.kt), played the same way.
class PairingFlow {
  public:
    enum class Mode { kPair, kInvite, kJoin };
    enum class Stage { kScanning, kGotKey, kHandingOver, kWaitingForTeam, kDone };

    PairingFlow(Mode mode, std::string my_name, std::optional<Team> team = std::nullopt);
    bool Read(std::string_view text);  // true when anything changed

    [[nodiscard]] Stage stage() const { return stage_; }
    [[nodiscard]] const std::string& qr() const { return qr_; }
    [[nodiscard]] const std::optional<PairingKey>& key() const { return key_; }
    [[nodiscard]] const std::optional<Team>& joined() const { return joined_; }
    [[nodiscard]] const std::optional<std::string>& member_name() const { return member_name_; }
    [[nodiscard]] bool foreign_invite() const { return foreign_invite_; }

  private:
    bool GotKey(bool confirmed);
    bool ReadTeamCode(std::string_view text);

    Mode mode_;
    std::string my_name_;
    std::optional<Team> team_;
    std::unique_ptr<Pairing> pairing_;
    Stage stage_ = Stage::kScanning;
    std::string qr_;
    std::optional<PairingKey> key_;
    std::optional<Team> joined_;
    std::optional<std::string> member_name_;
    bool foreign_invite_ = false;
};

}  // namespace fileflow::desktop::secure
