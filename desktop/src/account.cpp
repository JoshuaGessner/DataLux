#include "account.h"
#include "account_config.h"
#include "license.h"
#include "license_config.h"
#include "secure.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

namespace fileflow::desktop::account {
namespace {
using Json = nlohmann::json;
bool Opaque(std::string_view value) {
    return value.size() == 43 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
bool Uuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!(value[i] >= '0' && value[i] <= '9') && !(value[i] >= 'a' && value[i] <= 'f')) return false;
    }
    return true;
}
Json Read(std::string_view body) {
    if (body.empty() || body.size() > kMaxResponseBytes) throw std::runtime_error("Invalid account response");
    return Json::parse(body, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 16) throw std::runtime_error("Invalid account response");
        return true;
    });
}
bool Access(const Json& data, std::string_view user, std::string_view key, Reply* result) {
    if (!data.is_object() || !data.contains("pro") || !data["pro"].is_boolean() || !data.contains("license")) return false;
    result->pro = data["pro"].get<bool>();
    if (!result->pro) return data["license"].is_null();
    if (!data["license"].is_string() || !data.value("emailVerified", false)) return false;
    result->license = data["license"].get<std::string>();
    auto grant = license::Verify(result->license, key);
    return grant && grant->account_id == user;
}
Reply Failure(int status) {
    Reply out;
    if (status == 401) { out.message = "Sign in again to refresh your account. Offline Pro access is unchanged."; out.authentication_required = true; }
    if (status == 410) out.message = "This sign-in code expired. Choose Sign in to get a new code.";
    if (status == 403) out.message = "Verify your email on the website before linking this computer.";
    return out;
}
}  // namespace

std::string Origin() { return config::kAccountOrigin; }
bool ValidRequest(std::string_view path, std::string_view body, std::string_view bearer) {
    return (path == "/api/native/device/start" || path == "/api/native/device/poll" || path == "/api/native/entitlements" || path == "/api/native/logout") &&
        body.size() <= kMaxResponseBytes && (bearer.empty() || Opaque(bearer));
}

Reply ParseDevice(const HttpResponse& response) {
    if (response.status != 200) return Failure(response.status);
    try {
        const auto data = Read(response.body);
        Reply out; out.kind = Reply::Kind::kDevice; out.message.clear();
        out.device = {data.at("deviceCode").get<std::string>(), data.at("userCode").get<std::string>(),
            data.at("verificationUri").get<std::string>(), data.at("expiresIn").get<int>(), data.at("interval").get<int>()};
        const auto& code = out.device.user_code;
        if (!Opaque(out.device.secret) || code.size() != 9 || code[4] != '-' ||
            !std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }) ||
            out.device.verification_uri != Origin() + "/account.html#device" || out.device.expires_in < 1 || out.device.expires_in > 600 ||
            out.device.interval < 5 || out.device.interval > 60) return Failure(0);
        return out;
    } catch (...) { return Failure(0); }
}

Reply ParsePoll(const HttpResponse& response, std::string_view public_key_pem) {
    if (response.status == 202 || response.status == 429) {
        Reply out; out.kind = Reply::Kind::kPending; out.message.clear();
        out.retry_after = response.status == 429 ? 10 : 5;
        return out;
    }
    if (response.status != 200) return Failure(response.status);
    try {
        const auto data = Read(response.body);
        Reply out; out.kind = Reply::Kind::kSession;
        const auto& user = data.at("user");
        out.session = {data.at("token").get<std::string>(), user.at("id").get<std::string>(), user.at("email").get<std::string>(),
                       data.at("expiresAt").get<std::int64_t>(), {}};
        if (!Opaque(out.session.token) || !Uuid(out.session.user_id) || out.session.email.empty() || out.session.email.size() > 254 ||
            out.session.expires_at <= 0 || !Access(data.at("entitlements"), out.session.user_id, public_key_pem, &out)) return Failure(0);
        out.session.license = out.license;
        out.message = "Signed in. Your account is linked to this computer.";
        return out;
    } catch (...) { return Failure(0); }
}

Reply ParseAccess(const HttpResponse& response, std::string_view user_id, std::string_view public_key_pem) {
    if (response.status != 200) return Failure(response.status);
    try {
        Reply out; out.kind = Reply::Kind::kAccess;
        if (!Uuid(user_id) || !Access(Read(response.body), user_id, public_key_pem, &out)) return Failure(0);
        out.message = out.pro ? "Pro refreshed. It remains available offline." : "Your account is on the Free plan.";
        return out;
    } catch (...) { return Failure(0); }
}

Reply Start() { return ParseDevice(Request("/api/native/device/start", R"({"label":"DataLux PC"})")); }
Reply Poll(const Device& device) { return ParsePoll(Request("/api/native/device/poll", Json{{"deviceCode", device.secret}}.dump()), license::config::kPublicKey); }
Reply Refresh(const Session& session) { return ParseAccess(Request("/api/native/entitlements", {}, session.token), session.user_id, license::config::kPublicKey); }
Reply Logout(const Session& session) {
    const auto response = Request("/api/native/logout", "{}", session.token);
    Reply out; out.kind = Reply::Kind::kLogout;
    out.message = response.status == 200 || response.status == 401 ? "Signed out on this computer." :
        "Signed out locally. The server session will expire automatically.";
    return out;
}

std::optional<Session> LoadSession(const std::string& path) {
    try {
        const std::filesystem::path source(std::u8string(path.begin(), path.end()));
        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error) || error) return std::nullopt;
        std::ifstream stream(source, std::ios::binary);
        std::array<char, 16 * 1024 + 1> buffer{};
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = stream.gcount();
        if (stream.bad() || n <= 0 || static_cast<std::size_t>(n) == buffer.size()) return std::nullopt;
        const auto data = Read(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
        if (data.at("origin").get<std::string>() != Origin()) return std::nullopt;
        Session session{data.at("token").get<std::string>(), data.at("userId").get<std::string>(), data.at("email").get<std::string>(),
                        data.at("expiresAt").get<std::int64_t>(), data.at("license").get<std::string>()};
        if (!Opaque(session.token) || !Uuid(session.user_id) || session.email.empty() || session.email.size() > 254 || session.expires_at <= 0 ||
            session.license.size() > license::kMaxLicenseBytes) return std::nullopt;
        return session;
    } catch (...) { return std::nullopt; }
}
void SaveSession(const std::string& path, const Session& session) {
    if (path.empty() || !std::filesystem::path(std::u8string(path.begin(), path.end())).is_absolute()) throw std::runtime_error("Private account path unavailable");
    secure::WritePrivateFile(path, Json{{"origin", Origin()}, {"token", session.token}, {"userId", session.user_id}, {"email", session.email},
                                       {"expiresAt", session.expires_at}, {"license", session.license}}.dump());
}
}  // namespace fileflow::desktop::account
