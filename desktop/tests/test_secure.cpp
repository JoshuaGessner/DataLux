// The desktop's secure formats against the Android app's own vectors (SecureVectorsTest.kt): the
// same inputs, the same bytes -- or a phone and a computer could not open each other's files.
#include "secure.h"

#include <fileflow/envelope.h>

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace fileflow::desktop::secure {
namespace {

// The Kotlin test's Fixed(seed): "random" bytes seed, seed + 1, ...
Random Fixed(int seed) {
    return [seed](std::uint8_t* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(seed + static_cast<int>(i));
    };
}

Bytes Seq(std::size_t n, int (*f)(int)) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(f(static_cast<int>(i)));
    return b;
}

std::string Hex(const Bytes& b) {
    static constexpr char d[] = "0123456789abcdef";
    std::string s;
    for (auto v : b) {
        s += d[v >> 4];
        s += d[v & 15];
    }
    return s;
}

// The vectors, as SecureVectorsTest has them.
constexpr const char* kKey = "9e644ddfbe4679dc930bf73ea9c90216935b54ab9360d48b4908b27ba5a6fc24";
constexpr const char* kId = "99fd1bb27debc69f";
constexpr const char* kCode = "187 732";
constexpr const char* kEnvelope =
    "4646453202a0a1a2a3a4a5a6a7101112131415161718191a1bbe8cbdbf0eed6b97abc62b491373cb4310e9870d0138476365aae56d4879c0de083f18a7b3e262f5702eaa9f8653f713c199";
constexpr const char* kInvite =
    "FFTEAM2:ICEiIyQlJicoKSorQAkX_H8Suopkh3IaA9P-N0FJQIZy12MdrnAONeqx5dCSL7Eji_e5FYJZfPl3kT7Zv-5RHmT6Al9NFvm7r_9hnlTHcXa0-l-05HMrXTPAp2GbIving3GjS2w";
constexpr const char* kAck = "FFTEAMOK2:MDEyMzQ1Njc4OTo7SpcVy78k5blkYd2pCs0A1FG2Cit8acjFlTy3";

const Bytes kShared = Seq(32, [](int i) { return i + 1; });
const Bytes kMine = Seq(65, [](int i) { return i == 0 ? 4 : i; });
const Bytes kPeer = Seq(65, [](int i) { return i == 0 ? 4 : 65 - i; });

TEST(SecureVectors, PairingKeyDerivationMatchesThePhone) {
    const PairingKey k = Derive(kShared, kMine, kPeer, 0);
    EXPECT_EQ(Hex(k.key), kKey);
    EXPECT_EQ(Hex(k.id), kId);
    EXPECT_EQ(k.code, kCode);
    // Either side's view gives the same key.
    EXPECT_EQ(Hex(Derive(kShared, kPeer, kMine, 0).key), kKey);
}

TEST(SecureVectors, ASealedEnvelopeMatchesThePhoneAndOpens) {
    SealKey key{Seq(32, [](int i) { return i * 7 + 3; }), Seq(8, [](int i) { return 0xA0 + i; }), "paired phone"};
    const std::string text = "hello, team";
    auto plain = PackEnvelope(Bytes(text.begin(), text.end()), "note.txt", CompressionMode::kOff);
    ASSERT_TRUE(plain.ok());
    const Bytes sealed = Seal(plain.value().bytes, key, Fixed(0x10));
    EXPECT_EQ(Hex(sealed), kEnvelope);

    Opened o;
    ASSERT_EQ(Open(sealed, {key}, true, &o), OpenFailure::kNone);
    EXPECT_EQ(std::string(o.data.begin(), o.data.end()), text);
    EXPECT_EQ(o.name, "note.txt");
    EXPECT_EQ(o.sealed_to, "paired phone");
}

TEST(SecureVectors, ATeamInviteAndItsAckMatchThePhone) {
    const PairingKey pairing = Derive(kShared, kMine, kPeer, 0);
    Team team;
    team.id = Seq(8, [](int i) { return 0xB0 + i; });
    team.key = Seq(32, [](int i) { return 0xC0 + i; });
    team.name = "Crew";
    team.created_ms = 1'700'000'000'000;
    team.members = {{"Alex", 1'700'000'000'000, true, true, false}, {"Sam", 1'700'000'001'000}};
    EXPECT_EQ(invite::Encode(team, pairing, Fixed(0x20)), kInvite);

    auto joined = invite::Decode(kInvite, pairing, "Jo", 5);
    ASSERT_TRUE(joined.has_value());
    EXPECT_EQ(joined->name, "Crew");
    EXPECT_EQ(Hex(joined->key), Hex(team.key));
    ASSERT_EQ(joined->members.size(), 3U);
    EXPECT_EQ(joined->members[0].name, "Alex");
    EXPECT_TRUE(joined->members[0].leader);
    EXPECT_EQ(joined->members[1].name, "Sam");
    EXPECT_EQ(joined->members[2].name, "Jo");
    EXPECT_TRUE(joined->members[2].you);
    EXPECT_EQ(invite::Ack(*joined, pairing, "Jo", Fixed(0x30)), kAck);
    EXPECT_EQ(invite::ReadAck(kAck, team, pairing).value_or("?"), "Jo");
}

TEST(Secure, Base64UrlRoundTrips) {
    for (std::size_t n = 0; n < 40; ++n) {
        Bytes b(n);
        for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(i * 37 + 11);
        EXPECT_EQ(UnB64Url(B64Url(b)).value(), b) << n;
    }
    EXPECT_FALSE(UnB64Url("!!!").has_value());
}

TEST(Secure, TwoRealPairingsAgreeAndAStrangerOpensNothing) {
    auto a = Pairing::Start();
    auto b = Pairing::Start();
    EXPECT_EQ(b->ReadCode(a->QrText()), Pairing::Read::kPeerKey);
    EXPECT_EQ(a->ReadCode(b->QrText()), Pairing::Read::kPeerComplete);
    const auto ka = a->Key(0);
    const auto kb = b->Key(0);
    ASSERT_TRUE(ka && kb);
    EXPECT_EQ(Hex(ka->key), Hex(kb->key));
    EXPECT_EQ(ka->code, kb->code);
    EXPECT_EQ(a->ReadCode(a->QrText()), Pairing::Read::kOwn);
    // A point off the curve is not taken.
    Bytes bogus(65, 0);
    bogus[0] = 4;
    bogus[64] = 1;
    auto c = Pairing::Start();
    EXPECT_EQ(c->ReadCode(std::string(Pairing::kPrefix) + B64Url(bogus)), Pairing::Read::kNotOurs);

    auto plain = PackEnvelope(Bytes{1, 2, 3}, "x", CompressionMode::kOff);
    const Bytes sealed = Seal(plain.value().bytes, *ka);
    Opened o;
    EXPECT_EQ(Open(sealed, {*kb}, true, &o), OpenFailure::kNone);
    auto x = Pairing::Start();
    auto y = Pairing::Start();
    y->ReadCode(x->QrText());
    Opened none;
    EXPECT_EQ(Open(sealed, {*y->Key(0)}, true, &none), OpenFailure::kWrongKey);
    EXPECT_EQ(Open(sealed, {}, false, &none), OpenFailure::kNeedsPairing);
    EXPECT_EQ(Open(plain.value().bytes, {*kb}, true, &none), OpenFailure::kNotEncrypted);
}

TEST(Secure, AnAlteredEnvelopeIsRefused) {
    auto a = Pairing::Start();
    auto b = Pairing::Start();
    b->ReadCode(a->QrText());
    a->ReadCode(b->QrText());
    auto plain = PackEnvelope(Bytes(5000, 7), "x", CompressionMode::kAuto);
    const Bytes sealed = Seal(plain.value().bytes, *a->Key(0));
    for (std::size_t i = 0; i < sealed.size(); i += 97) {
        Bytes bad = sealed;
        bad[i] ^= 1;
        Opened o;
        EXPECT_NE(Open(bad, {*b->Key(0)}, true, &o), OpenFailure::kNone) << i;
    }
}

TEST(Secure, ACompressedBatchOpens) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files = {{"a.txt", Bytes(3000, 'a')}, {"b.txt", Bytes(10, 'b')}};
    auto plain = PackBatchEnvelope(files, "2 files", CompressionMode::kAuto);
    ASSERT_TRUE(plain.ok());
    Opened o;
    ASSERT_EQ(Open(plain.value().bytes, {}, false, &o), OpenFailure::kNone);
    ASSERT_TRUE(o.files.has_value());
    EXPECT_EQ(o.files->size(), 2U);
    EXPECT_EQ((*o.files)[0].first, "a.txt");
    EXPECT_EQ((*o.files)[0].second, Bytes(3000, 'a'));
}

// Two sides of the pairing screen, as PairingFlowTest plays them: reads in any order, some missed.
TEST(Secure, ATeamChangesHandsInAnyOrder) {
    for (int seed = 0; seed < 60; ++seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        const Team team = Team::Create("Field crew", "Laptop", 1);
        PairingFlow leader(PairingFlow::Mode::kInvite, "Laptop", team);
        PairingFlow member(PairingFlow::Mode::kJoin, "Pixel");
        int steps = 0;
        for (; steps < 400 && !(leader.stage() == PairingFlow::Stage::kDone && member.stage() == PairingFlow::Stage::kDone); ++steps) {
            if (rng() % 10 < 3) continue;
            if (rng() % 2 == 0) leader.Read(member.qr()); else member.Read(leader.qr());
        }
        ASSERT_LT(steps, 400) << seed;
        ASSERT_TRUE(member.joined().has_value());
        EXPECT_EQ(Hex(member.joined()->key), Hex(team.key));
        EXPECT_EQ(leader.member_name().value_or(""), "Pixel");
        EXPECT_EQ(leader.key()->code, member.key()->code);
    }
}

TEST(Secure, TeamsAreKeptInTheStore) {
    const std::string path = ::testing::TempDir() + "teams-test.txt";
    std::remove(path.c_str());
    Team a = Team::Create("Crew", "Me", 5);
    a = a.WithMember({"Sam", 6});
    {
        TeamStore s(path);
        s.Add(a);
        s.Add(Team::Create("Medics", "Me", 7));
    }
    TeamStore again(path);
    auto all = again.All();
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all[0].name, "Crew");
    EXPECT_EQ(Hex(all[0].key), Hex(a.key));
    ASSERT_EQ(all[0].members.size(), 2U);
    EXPECT_EQ(all[0].members[1].name, "Sam");
    again.Remove(a.IdHex());
    EXPECT_EQ(TeamStore(path).All().size(), 1U);
    std::remove(path.c_str());
}

}  // namespace
}  // namespace fileflow::desktop::secure
