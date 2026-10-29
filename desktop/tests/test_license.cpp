#include "license.h"
#include "license_fixture.h"
#include "secure.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace license = fileflow::desktop::license;
namespace secure = fileflow::desktop::secure;

TEST(License, DesktopNeverRequiresPurchase) {
    EXPECT_FALSE(license::StoreBuild());
}

TEST(License, VerifiesServerCompatiblePermanentGrant) {
    const auto grant = license::Verify(license_fixture::kValid, license_fixture::kPublicKey);
    ASSERT_TRUE(grant);
    EXPECT_EQ(grant->account_id, "11111111-1111-4111-8111-111111111111");
    EXPECT_EQ(grant->license_id, "22222222-2222-4222-8222-222222222222");
    EXPECT_EQ(grant->issued_at_ms, 1792886400000LL);
}

TEST(License, RejectsUntrustedOrMissingKeys) {
    EXPECT_FALSE(license::Verify(license_fixture::kValid, license_fixture::kWrongKey));
    EXPECT_FALSE(license::Verify(license_fixture::kValid, ""));
    EXPECT_FALSE(license::Verify(license_fixture::kValid, "not a public key"));
}

TEST(License, RejectsTamperedEnvelopeAndPayload) {
    const std::string original = license_fixture::kValid;
    for (const std::size_t pos : {std::size_t{0}, std::size_t{17}, original.find('\n', 32) + 3, original.size() - 5}) {
        auto changed = original;
        changed[pos] = changed[pos] == 'A' ? 'B' : 'A';
        EXPECT_FALSE(license::Verify(changed, license_fixture::kPublicKey));
    }
    EXPECT_FALSE(license::Verify(original.substr(0, original.size() - 1), license_fixture::kPublicKey));
    EXPECT_FALSE(license::Verify(original + "\n", license_fixture::kPublicKey));
    EXPECT_FALSE(license::Verify(std::string(license::kMaxLicenseBytes + 1, 'A'), license_fixture::kPublicKey));
}

TEST(License, RejectsSignedUnsupportedOrMalformedClaims) {
    EXPECT_FALSE(license::Verify(license_fixture::kWrongFeatures, license_fixture::kPublicKey));
    EXPECT_FALSE(license::Verify(license_fixture::kBadAccount, license_fixture::kPublicKey));
    EXPECT_FALSE(license::Verify(license_fixture::kBadTime, license_fixture::kPublicKey));
    EXPECT_FALSE(license::Verify(license_fixture::kWrongVersion, license_fixture::kPublicKey));
}

TEST(License, FileReadIsBoundedAndPrivateReplacementPreservesOtherData) {
    const auto directory = std::filesystem::temp_directory_path() / ("fileflow-license-test-" + std::to_string(secure::NowMs()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const auto target = directory / "pro.fileflow-license";
    const auto unrelated = directory / "received.txt";
    { std::ofstream out(unrelated); out << "keep this file"; }
    secure::WritePrivateFile(target.string(), license_fixture::kValid);
    EXPECT_EQ(license::ReadFile(target.string()), license_fixture::kValid);
    EXPECT_EQ(license::ReadFile(unrelated.string()), "keep this file");
#if !defined(_WIN32)
    EXPECT_EQ(std::filesystem::status(target).permissions() &
        (std::filesystem::perms::group_all | std::filesystem::perms::others_all), std::filesystem::perms::none);
#endif
    secure::WritePrivateFile(target.string(), std::string(license::kMaxLicenseBytes + 1, 'A'));
    EXPECT_TRUE(license::ReadFile(target.string()).empty());
    EXPECT_TRUE(license::ReadFile((directory / "missing").string()).empty());
    std::filesystem::remove_all(directory);
}
