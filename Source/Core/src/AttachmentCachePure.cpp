#include "AttachmentCachePure.h"

#include "Json/BoundedJsonParse.h"
#include "Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace smatchet {
namespace attachments {

namespace {

bool IsReservedChar(unsigned char c) {
    return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0u) == 0x80u; }

// The longest prefix of `s` that fits in `maxBytes` without splitting a UTF-8 sequence.
std::string Utf8Prefix(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return s;
    }
    std::size_t end = maxBytes;
    while (end > 0 && IsUtf8Continuation(static_cast<unsigned char>(s[end]))) {
        --end;
    }
    return s.substr(0, end);
}

std::string TrimDotsAndSpaces(const std::string& s) {
    const std::size_t first = s.find_first_not_of(". ");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = s.find_last_not_of(". ");
    return s.substr(first, last - first + 1);
}

// Windows refuses these as a file's base name whatever the extension (CON, CON.txt, "con .log").
bool IsReservedDeviceName(const std::string& name) {
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    std::transform(base.begin(), base.end(), base.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$" ||
        base == "clock$") {
        return true;
    }
    return base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '1' &&
           base[3] <= '9';
}

// ".png"-shaped: a dot and 1-10 ASCII letters or digits.
bool IsPlainExtension(const std::string& extension) {
    if (extension.size() < 2 || extension.size() > 11 || extension[0] != '.') {
        return false;
    }
    return std::all_of(extension.begin() + 1, extension.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

} // namespace

std::string EntryKeyForUrl(const std::string& url) { return smatchet::hashing::Sha256Hex(url); }

bool IsEntryKey(const std::string& name) {
    return name.size() == 64 && std::all_of(name.begin(), name.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string SanitizeFileName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20u || c == 0x7Fu) {
            continue;
        }
        out.push_back(IsReservedChar(c) ? '_' : ch);
    }
    out = TrimDotsAndSpaces(out);
    if (out.empty()) {
        return "attachment";
    }
    if (IsReservedDeviceName(out)) {
        out.insert(out.begin(), '_');
    }
    if (out.size() > kMaxFileNameBytes) {
        // Keep a short extension so the shortened file still opens in the right application.
        const std::size_t dot = out.rfind('.');
        const bool keepExtension = dot != std::string::npos && dot > 0 && out.size() - dot <= 16;
        const std::string extension = keepExtension ? out.substr(dot) : std::string();
        const std::string stem = keepExtension ? out.substr(0, dot) : out;
        out = Utf8Prefix(stem, kMaxFileNameBytes - extension.size()) + extension;
        out = TrimDotsAndSpaces(out);
        if (out.empty()) {
            return "attachment";
        }
    }
    return out;
}

std::string StoredFileName(const std::string& filename, const std::string& extension) {
    std::string name = SanitizeFileName(filename);
    if (name.find('.') == std::string::npos && IsPlainExtension(extension)) {
        name += extension;
    }
    // Sanitize again so the stored name is exactly what ParseEntryMeta accepts back (the cap, once the
    // extension is appended).
    return SanitizeFileName(name);
}

std::vector<std::string> SelectEvictions(std::vector<CacheEntryInfo> entries, std::uint64_t capBytes,
                                         std::int64_t nowEpochSec, std::int64_t graceSeconds) {
    std::vector<std::string> evict;
    std::uint64_t total = 0;
    for (const CacheEntryInfo& e : entries) {
        total += e.SizeBytes;
    }
    const auto recentlyUsed = [nowEpochSec, graceSeconds](const CacheEntryInfo& e) {
        return nowEpochSec - e.LastUsedEpochSec < graceSeconds;
    };
    std::sort(entries.begin(), entries.end(), [](const CacheEntryInfo& a, const CacheEntryInfo& b) {
        return a.LastUsedEpochSec != b.LastUsedEpochSec ? a.LastUsedEpochSec < b.LastUsedEpochSec : a.Key < b.Key;
    });
    std::vector<CacheEntryInfo> complete;
    complete.reserve(entries.size());
    for (CacheEntryInfo& e : entries) {
        if (!e.Complete && !recentlyUsed(e)) {
            evict.push_back(e.Key);
            total -= e.SizeBytes;
        } else if (e.Complete) {
            complete.push_back(std::move(e));
        }
    }
    for (const CacheEntryInfo& e : complete) {
        if (total <= capBytes || recentlyUsed(e)) {
            break; // sorted oldest first: every later entry is at least as recent
        }
        evict.push_back(e.Key);
        total -= e.SizeBytes;
    }
    return evict;
}

std::string SerializeEntryMeta(const EntryMeta& meta) {
    try {
        nlohmann::json j = nlohmann::json::object();
        j["url"] = meta.Url;
        j["mime"] = meta.Mime;
        j["file"] = meta.FileName;
        // Strict: text that is not valid UTF-8 throws, so a URL is never stored altered (a lookup
        // compares it byte for byte).
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    } catch (const std::exception&) {
        return std::string(); // the caller skips caching this attachment
    }
}

bool ParseEntryMeta(const std::string& json, EntryMeta& out) {
    std::string parseErr;
    const nlohmann::json j = smatchet::json_safe::ParseBounded(json, parseErr);
    if (!parseErr.empty() || !j.is_object()) {
        return false;
    }
    const auto url = j.find("url");
    const auto file = j.find("file");
    const auto mime = j.find("mime");
    if (url == j.end() || !url->is_string() || file == j.end() || !file->is_string()) {
        return false;
    }
    EntryMeta meta;
    meta.Url = url->get<std::string>();
    meta.FileName = file->get<std::string>();
    meta.Mime = (mime != j.end() && mime->is_string()) ? mime->get<std::string>() : std::string();
    if (meta.Url.empty() || meta.FileName.empty() || SanitizeFileName(meta.FileName) != meta.FileName) {
        return false;
    }
    out = std::move(meta);
    return true;
}

} // namespace attachments
} // namespace smatchet
