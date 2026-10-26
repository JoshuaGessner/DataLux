#include "account.h"
#include "license.h"
#include "license_fixture.h"
#include "secure.h"
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace fileflow::desktop::account {
namespace {
using Json = nlohmann::json;
constexpr auto kUser = "19444c5e-5a94-490a-a6c3-352a461cf677";
std::string Token() { return std::string(43, 'a'); }
}

TEST(Account, DeviceFlowRejectsUnsafeUrlsCodesAndPollIntervals) {
    Json value{{"deviceCode", Token()}, {"userCode", "ABCD-2345"}, {"verificationUri", Origin() + "/account.html#device"},
               {"expiresIn", 600}, {"interval", 5}};
    EXPECT_EQ(ParseDevice({200, value.dump()}).kind, Reply::Kind::kDevice);
    value["verificationUri"] = "https://attacker.invalid/account.html#device";
    EXPECT_EQ(ParseDevice({200, value.dump()}).kind, Reply::Kind::kError);
    value["verificationUri"] = Origin() + "/account.html#device"; value["interval"] = 1;
    EXPECT_EQ(ParseDevice({200, value.dump()}).kind, Reply::Kind::kError);
    value["interval"] = 5; value["deviceCode"] = "short";
    EXPECT_EQ(ParseDevice({200, value.dump()}).kind, Reply::Kind::kError);
    EXPECT_EQ(ParseDevice({200, std::string(kMaxResponseBytes + 1, 'x')}).kind, Reply::Kind::kError);
}

TEST(Account, ProviderFailuresCannotBecomeAFreeRevocation) {
    for (const auto status : {0, 301, 401, 403, 404, 500, 503}) {
        auto reply = ParseAccess({status, R"({"pro":false,"license":null})"}, kUser, "");
        EXPECT_EQ(reply.kind, Reply::Kind::kError);
    }
    EXPECT_EQ(ParseAccess({200, R"({"pro":false,"license":null})"}, kUser, "").kind, Reply::Kind::kAccess);
    EXPECT_EQ(ParseAccess({200, R"({"pro":"false","license":null})"}, kUser, "").kind, Reply::Kind::kError);
    EXPECT_EQ(ParseAccess({200, "<html>fallback</html>"}, kUser, "").kind, Reply::Kind::kError);
    EXPECT_TRUE(ParseAccess({401, "{}"}, kUser, "").authentication_required);
    EXPECT_FALSE(ParseAccess({503, "{}"}, kUser, "").authentication_required);
}

TEST(Account, TransportRejectsUnexpectedPathsAndHeaderInjection) {
    EXPECT_TRUE(ValidRequest("/api/native/entitlements", "", Token()));
    EXPECT_FALSE(ValidRequest("/api/admin/login", "{}", Token()));
    EXPECT_FALSE(ValidRequest("/api/native/entitlements", "", "a\r\nInjected: value"));
    EXPECT_FALSE(ValidRequest("/api/native/device/start", std::string(kMaxResponseBytes + 1, 'x'), ""));
}

TEST(Account, SignInValidatesSessionAndNeverTreatsAPendingPollAsSignedIn) {
    EXPECT_EQ(ParsePoll({202, "{}"}, "").kind, Reply::Kind::kPending);
    EXPECT_EQ(ParsePoll({429, "{}"}, "").retry_after, 10);
    EXPECT_EQ(ParsePoll({410, "{}"}, "").kind, Reply::Kind::kError);
    Json value{{"token", Token()}, {"user", {{"id", kUser}, {"email", "person@example.invalid"}}},
               {"expiresAt", 1792890000000LL}, {"entitlements", {{"pro", false}, {"license", nullptr}}}};
    EXPECT_EQ(ParsePoll({200, value.dump()}, "").kind, Reply::Kind::kSession);
    value["token"] = "untrusted-short-session";
    EXPECT_EQ(ParsePoll({200, value.dump()}, "").kind, Reply::Kind::kError);
    value["token"] = Token(); value["entitlements"]["pro"] = true;
    EXPECT_EQ(ParsePoll({200, value.dump()}, "").kind, Reply::Kind::kError);
}

TEST(Account, ProRefreshRequiresASignedGrantBoundToTheSignedInAccount) {
    const auto id = "11111111-1111-4111-8111-111111111111";
    Json value{{"pro", true}, {"license", license_fixture::kValid}, {"emailVerified", true}};
    EXPECT_EQ(ParseAccess({200, value.dump()}, id, license_fixture::kPublicKey).kind, Reply::Kind::kAccess);
    EXPECT_EQ(ParseAccess({200, value.dump()}, kUser, license_fixture::kPublicKey).kind, Reply::Kind::kError);
    EXPECT_EQ(ParseAccess({200, value.dump()}, id, license_fixture::kWrongKey).kind, Reply::Kind::kError);
    value["emailVerified"] = false;
    EXPECT_EQ(ParseAccess({200, value.dump()}, id, license_fixture::kPublicKey).kind, Reply::Kind::kError);
}

TEST(Account, CachedSessionHasBoundedPrivateAtomicStorage) {
    secure::Bytes random(16); secure::SystemRandom(random.data(), random.size());
    const auto dir = std::filesystem::temp_directory_path() / ("fileflow-account-" + secure::B64Url(random));
    std::filesystem::create_directory(dir);
    const auto file = dir / "account.json";
    Session session{Token(), kUser, "person@example.invalid", 1792890000000LL, ""};
    EXPECT_THROW(SaveSession("", session), std::runtime_error);
    SaveSession(file.string(), session);
    const auto loaded = LoadSession(file.string()); ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->user_id, session.user_id); EXPECT_EQ(loaded->token, session.token);
#if !defined(_WIN32)
    EXPECT_EQ(std::filesystem::status(file).permissions() & std::filesystem::perms::group_all, std::filesystem::perms::none);
    EXPECT_EQ(std::filesystem::status(file).permissions() & std::filesystem::perms::others_all, std::filesystem::perms::none);
#endif
    Json other{{"origin", "https://other.example.invalid"}, {"token", session.token}, {"userId", session.user_id},
               {"email", session.email}, {"expiresAt", session.expires_at}, {"license", ""}};
    secure::WritePrivateFile(file.string(), other.dump()); EXPECT_FALSE(LoadSession(file.string()));
    { std::ofstream out(file); out << std::string(20 * 1024, 'x'); }
    EXPECT_FALSE(LoadSession(file.string()));
    secure::WritePrivateFile(file.string(), ""); EXPECT_FALSE(LoadSession(file.string()));
    std::filesystem::remove_all(dir);
}
}  // namespace fileflow::desktop::account
