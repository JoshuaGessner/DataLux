#include "transfer.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>

namespace fileflow::desktop {

std::string FormatBytes(std::uint64_t b) {
    char s[32];
    const auto unit = [&](double v, const char* u) {
        // "64 MB", not "64.0 MB"; "1.5 MB" where the tenth says something.
        if (std::abs(v - std::round(v)) < 0.05 || v >= 100) std::snprintf(s, sizeof s, "%.0f %s", v, u);
        else std::snprintf(s, sizeof s, "%.1f %s", v, u);
    };
    if (b < 1024) {
        std::snprintf(s, sizeof s, "%llu B", static_cast<unsigned long long>(b));
    } else if (b < 1024 * 1024) {
        unit(static_cast<double>(b) / 1024.0, "KB");
    } else {
        unit(static_cast<double>(b) / (1024.0 * 1024.0), "MB");
    }
    return s;
}

std::filesystem::path PathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string NameOf(const std::filesystem::path& p) {
    const std::u8string n = p.filename().u8string();
    return {n.begin(), n.end()};
}

Prepared Prepare(const std::vector<std::string>& paths, CompressionMode mode) {
    Prepared out;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    std::uint64_t total = 0;
    for (const auto& utf8 : paths) {
        const auto path = PathOf(utf8);
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            out.problem = NameOf(path) + " is a folder. Drop the files in it instead.";
            return out;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            out.problem = "Can't read " + NameOf(path) + ".";
            return out;
        }
        total += size;
        if (total > kMaxFileBytes) {
            out.problem = "That's more than " + FormatBytes(kMaxFileBytes) + " in all. Send fewer files at once.";
            return out;
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
            out.problem = "Can't read " + NameOf(path) + ".";
            return out;
        }
        files.emplace_back(NameOf(path), std::move(bytes));
    }
    if (files.empty()) {
        out.problem = "Nothing to send.";
        return out;
    }
    if (files.size() > kEnvelopeMaxBatchFiles) {
        out.problem = "The most one transfer can carry is " + std::to_string(kEnvelopeMaxBatchFiles) + " files.";
        return out;
    }
    if (files.size() == 1 && files[0].second.empty()) {
        out.problem = files[0].first + " is empty.";
        return out;
    }
    out.files = files.size();
    out.name = files.size() == 1 ? files[0].first : std::to_string(files.size()) + " files";
    auto packed = files.size() == 1 ? PackEnvelope(files[0].second, out.name, mode)
                                    : PackBatchEnvelope(files, out.name, mode);
    if (!packed.ok()) {
        out.problem = "Can't pack the files: " + std::string(ErrorName(packed.error())) + ".";
        return out;
    }
    out.envelope = std::move(packed).value();
    return out;
}

Prepared PrepareText(const std::string& text, const std::string& name, CompressionMode mode) {
    Prepared out;
    if (text.empty()) {
        out.problem = "The clipboard has no text to send.";
        return out;
    }
    if (text.size() > kMaxFileBytes) {
        out.problem = "That's more than " + FormatBytes(kMaxFileBytes) + " of text.";
        return out;
    }
    out.files = 1;
    out.name = name;
    auto packed = PackEnvelope(std::vector<std::uint8_t>(text.begin(), text.end()), name, mode);
    if (!packed.ok()) {
        out.problem = "Can't pack the text: " + std::string(ErrorName(packed.error())) + ".";
        return out;
    }
    out.envelope = std::move(packed).value();
    return out;
}

std::uint64_t Transfer::PassFrames() const {
    const auto& m = session->manifest();
    if (m.symbol_size == 0) return 1;
    return (m.file_size + m.symbol_size - 1) / m.symbol_size;
}

std::unique_ptr<Transfer> Open(const Prepared& p, const Speed& speed, std::string* problem) {
    auto layout = FrameLayout::Create({speed.cols, speed.rows}, LayoutConfig{});
    if (!layout.ok()) {
        *problem = "Layout: " + std::string(ErrorName(layout.error()));
        return nullptr;
    }
    auto t = std::make_unique<Transfer>(std::make_unique<FrameLayout>(std::move(layout).value()), speed);
    TransmitConfig tc;
    tc.fec_nsym = kFecNsym;
    tc.profile = speed.profile;
    tc.content = {.original_size = p.envelope.original_size, .codec = p.envelope.codec,
                  .compress_ms = p.envelope.compress_ms};
    auto s = TransmitSession::Create(*t->layout, p.envelope.bytes, p.name,
                                     SessionIdFor(p.envelope.bytes, p.name), tc);
    if (!s.ok()) {
        *problem = "Transmitter: " + std::string(ErrorName(s.error()));
        return nullptr;
    }
    t->session.emplace(std::move(s).value());
    return t;
}

}  // namespace fileflow::desktop
