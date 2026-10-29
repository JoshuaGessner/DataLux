// Offline Pro grants signed by FileFlow's server. The signing key never ships in the app.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fileflow::desktop::license {

inline constexpr std::size_t kMaxLicenseBytes = 4096;

struct Grant {
    std::string account_id;
    std::string license_id;
    std::int64_t issued_at_ms = 0;
};

// Strict format and ECDSA P-256/SHA-256 verification, with trust supplied by the build.
std::optional<Grant> Verify(std::string_view text, std::string_view public_key_pem);
std::optional<Grant> VerifyTrusted(std::string_view text);
std::string ReadFile(const std::string& path);
// Retained for build/API compatibility; desktop features are always free.
bool StoreBuild();
bool TrustConfigured();

}  // namespace fileflow::desktop::license
