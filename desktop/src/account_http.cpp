#include "account.h"
#include <algorithm>
#include <chrono>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace fileflow::desktop::account {
#if defined(_WIN32)
namespace {
struct Handle {
    HINTERNET value;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
std::wstring Wide(const std::string& text) { return {text.begin(), text.end()}; }
}
HttpResponse Request(std::string_view path, std::string_view body, std::string_view bearer) {
    if (!ValidRequest(path, body, bearer)) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto url = Wide(Origin() + std::string(path));
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1); parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return {};
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength), target(parts.lpszUrlPath, parts.dwUrlPathLength);
    Handle session{WinHttpOpen(L"FileFlow Account/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000)) return {};
    Handle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) return {};
    Handle request{WinHttpOpenRequest(connection.value, body.empty() ? L"GET" : L"POST", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) return {};
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled)) return {};
    std::wstring headers = L"Accept: application/json\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
    if (!bearer.empty()) headers += Wide("Authorization: Bearer " + std::string(bearer) + "\r\n");
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return {};
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return {};
    HttpResponse response; response.status = static_cast<int>(status);
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) return {};
        char buffer[4096]; DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof buffer, &read)) return {};
        if (!read) break;
        if (response.body.size() + read > kMaxResponseBytes) return {};
        response.body.append(buffer, read);
    }
    return response;
}
#else
namespace {
std::size_t Receive(char* bytes, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    if (size && count > kMaxResponseBytes / size) return 0;
    const auto length = size * count;
    if (body->size() + length > kMaxResponseBytes) return 0;
    body->append(bytes, length); return length;
}
struct CurlInit {
    bool valid = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    ~CurlInit() { if (valid) curl_global_cleanup(); }
};
}
HttpResponse Request(std::string_view path, std::string_view body, std::string_view bearer) {
    static CurlInit init;
    if (!init.valid || !ValidRequest(path, body, bearer)) return {};
    CURL* handle = curl_easy_init(); if (!handle) return {};
    const std::string url = Origin() + std::string(path), payload(body), auth = "Authorization: Bearer " + std::string(bearer);
    struct curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!bearer.empty()) headers = curl_slist_append(headers, auth.c_str());
    HttpResponse response;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    if (!body.empty()) { curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload.c_str()); curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size())); }
    const auto result = curl_easy_perform(handle);
    long status = 0; curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers); curl_easy_cleanup(handle);
    if (result != CURLE_OK) return {};
    response.status = static_cast<int>(status); return response;
}
#endif
}  // namespace fileflow::desktop::account
