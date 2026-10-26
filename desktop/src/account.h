#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fileflow::desktop::account {

inline constexpr std::size_t kMaxResponseBytes = 32 * 1024;
struct HttpResponse { int status = 0; std::string body; };
// Platform trust store, HTTPS only, no redirects, bounded timeout and response size.
HttpResponse Request(std::string_view path, std::string_view body = {}, std::string_view bearer = {});
bool ValidRequest(std::string_view path, std::string_view body, std::string_view bearer);
std::string Origin();

struct Session {
    std::string token;
    std::string user_id;
    std::string email;
    std::int64_t expires_at = 0;
    std::string license;
};
struct Device {
    std::string secret;
    std::string user_code;
    std::string verification_uri;
    int expires_in = 0;
    int interval = 5;
};
struct Reply {
    enum class Kind { kError, kDevice, kPending, kSession, kAccess, kLogout };
    Kind kind = Kind::kError;
    std::string message = "Account service is unavailable. Your offline access is unchanged.";
    Device device;
    Session session;
    bool pro = false;
    std::string license;
    int retry_after = 5;
    bool authentication_required = false;
};

// Parsing is separate from transport for deterministic, offline tests.
Reply ParseDevice(const HttpResponse& response);
Reply ParsePoll(const HttpResponse& response, std::string_view public_key_pem);
Reply ParseAccess(const HttpResponse& response, std::string_view user_id, std::string_view public_key_pem);
Reply Start();
Reply Poll(const Device& device);
Reply Refresh(const Session& session);
Reply Logout(const Session& session);
std::optional<Session> LoadSession(const std::string& path);
void SaveSession(const std::string& path, const Session& session);

}  // namespace fileflow::desktop::account
