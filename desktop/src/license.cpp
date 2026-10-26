#include "license.h"
#include "license_config.h"
#include "secure.h"

#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>

namespace fileflow::desktop::license {
namespace {

template <std::size_t N>
std::optional<std::array<std::string_view, N>> Lines(std::string_view text) {
    std::array<std::string_view, N> out;
    for (auto& line : out) {
        const auto end = text.find('\n');
        if (end == std::string_view::npos) return std::nullopt;
        line = text.substr(0, end);
        text.remove_prefix(end + 1);
    }
    if (!text.empty()) return std::nullopt;
    return out;
}

bool IsUuid(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
        } else if (!(text[i] >= '0' && text[i] <= '9') && !(text[i] >= 'a' && text[i] <= 'f')) return false;
    }
    return true;
}

std::optional<secure::Bytes> Decode(std::string_view text) {
    const auto bytes = secure::UnB64Url(text);
    if (!bytes || secure::B64Url(*bytes) != text) return std::nullopt;
    return bytes;
}

struct PublicKey {
    mbedtls_pk_context key;
    PublicKey() { mbedtls_pk_init(&key); }
    ~PublicKey() { mbedtls_pk_free(&key); }
};

}  // namespace

std::optional<Grant> Verify(std::string_view text, std::string_view public_key_pem) {
    if (text.empty() || text.size() > kMaxLicenseBytes || public_key_pem.empty() || public_key_pem.size() > 4096) return std::nullopt;
    const auto lines = Lines<4>(text);
    if (!lines || (*lines)[0] != "FILEFLOW-PRO-1" || (*lines)[1].size() != 16) return std::nullopt;
    const auto payload = Decode((*lines)[2]);
    const auto signature = Decode((*lines)[3]);
    if (!payload || payload->empty() || payload->size() > 512 || !signature || signature->empty() || signature->size() > 80) return std::nullopt;

    PublicKey parsed;
    const std::string pem(public_key_pem);
    if (mbedtls_pk_parse_public_key(&parsed.key, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1) != 0 ||
        !mbedtls_pk_can_do(&parsed.key, MBEDTLS_PK_ECDSA)) return std::nullopt;
    if (mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(parsed.key)) != MBEDTLS_ECP_DP_SECP256R1) return std::nullopt;
    unsigned char der[256];
    const int der_length = mbedtls_pk_write_pubkey_der(&parsed.key, der, sizeof der);
    if (der_length <= 0) return std::nullopt;
    const secure::Bytes der_bytes(der + sizeof der - static_cast<std::size_t>(der_length), der + sizeof der);
    const auto digest = secure::Sha256(der_bytes);
    static constexpr char hex[] = "0123456789abcdef";
    std::string key_id;
    for (std::size_t i = 0; i < 8; ++i) {
        key_id += hex[digest[i] >> 4];
        key_id += hex[digest[i] & 15];
    }
    if ((*lines)[1] != key_id) return std::nullopt;
    const auto hash = secure::Sha256(*payload);
    if (mbedtls_pk_verify(&parsed.key, MBEDTLS_MD_SHA256, hash.data(), hash.size(), signature->data(), signature->size()) != 0) return std::nullopt;

    const std::string body(payload->begin(), payload->end());
    const auto claims = Lines<6>(body);
    if (!claims || (*claims)[0] != "fileflow-pro-v1" || !IsUuid((*claims)[1]) || !IsUuid((*claims)[2]) ||
        (*claims)[4] != "perpetual" || (*claims)[5] != "batch,bulk_manage") return std::nullopt;
    const auto time = (*claims)[3];
    if (time.empty() || time.size() > 19 || !std::all_of(time.begin(), time.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    std::int64_t issued_at = 0;
    const auto result = std::from_chars(time.data(), time.data() + time.size(), issued_at);
    if (result.ec != std::errc{} || result.ptr != time.data() + time.size() || issued_at <= 0) return std::nullopt;
    return Grant{std::string((*claims)[1]), std::string((*claims)[2]), issued_at};
}

std::optional<Grant> VerifyTrusted(std::string_view text) { return Verify(text, config::kPublicKey); }
bool StoreBuild() { return FILEFLOW_STORE_BUILD != 0; }
bool TrustConfigured() { return config::kPublicKey[0] != '\0'; }

std::string ReadFile(const std::string& path) {
    // Read at most the format limit plus one byte; never allocate based on untrusted file size.
    const std::filesystem::path source(std::u8string(path.begin(), path.end()));
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) return {};
    std::ifstream in(source, std::ios::binary);
    std::array<char, kMaxLicenseBytes + 1> buffer{};
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = in.gcount();
    if (in.bad() || count <= 0 || static_cast<std::size_t>(count) > kMaxLicenseBytes) return {};
    return {buffer.data(), static_cast<std::size_t>(count)};
}

}  // namespace fileflow::desktop::license
