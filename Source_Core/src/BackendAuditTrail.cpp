#include "BackendAuditTrail.h"

#include "ConfigManager.h"
#include "StringUtil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

namespace BackendAuditTrail {
namespace {

std::mutex& AuditMutex() {
    static std::mutex m;
    return m;
}

struct AuditReadCache {
    std::string Path;
    std::streampos Offset = std::streampos(0);
    std::deque<nlohmann::json> Events;
};

AuditReadCache& ReaderCache() {
    static AuditReadCache cache;
    return cache;
}

std::int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string NowLocalIso() {
    const std::time_t now = std::time(nullptr);
    std::tm tmLocal{};
#if defined(_WIN32)
    localtime_s(&tmLocal, &now);
#else
    localtime_r(&now, &tmLocal);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    return buf;
}

bool LooksSensitiveKey(const std::string& key) {
    const std::string k = ToLowerAsciiCopy(key);
    return k.find("token") != std::string::npos || k.find("secret") != std::string::npos ||
           k.find("password") != std::string::npos || k.find("apikey") != std::string::npos ||
           k.find("api_key") != std::string::npos || k.find("authorization") != std::string::npos ||
           k.find("security") != std::string::npos || k.find("comment") != std::string::npos ||
           k.find("description") != std::string::npos || k.find("accountid") != std::string::npos ||
           k.find("account_id") != std::string::npos || k.find("email") != std::string::npos ||
           k.find("p4_user") != std::string::npos || k.find("assignee") != std::string::npos ||
           k.find("reporter") != std::string::npos || k.find("creator") != std::string::npos ||
           k.find("watchers") != std::string::npos || k == "summary" || k == "body" || k == "text";
}

/** Audit field-edits store the Jira field id in `field_id` and values under generic `before` / `after`. */
bool LooksLikeFieldDiffObject(const nlohmann::json& o) {
    if (!o.is_object()) {
        return false;
    }
    const auto fid = o.find("field_id");
    if (fid == o.end() || !fid->is_string() || fid->get<std::string>().empty()) {
        return false;
    }
    return o.contains("before") || o.contains("after");
}

nlohmann::json RedactJsonWithKey(const std::string& key, const nlohmann::json& value);

nlohmann::json RedactFieldDiffObject(const nlohmann::json& obj) {
    const auto fidIt = obj.find("field_id");
    const std::string auditFieldId =
        (fidIt != obj.end() && fidIt->is_string()) ? fidIt->get<std::string>() : std::string();
    const bool sensitiveField = LooksSensitiveKey(auditFieldId);

    nlohmann::json out = nlohmann::json::object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& childKey = it.key();
        if (childKey == "before" || childKey == "after") {
            if (sensitiveField) {
                out[childKey] = "[redacted]";
            } else {
                out[childKey] = RedactJsonWithKey(auditFieldId, it.value());
            }
        } else {
            out[childKey] = RedactJsonWithKey(childKey, it.value());
        }
    }
    return out;
}

std::string TruncateAuditString(const std::string& s) {
    constexpr std::size_t kMaxAuditString = 1000;
    if (s.size() <= kMaxAuditString) {
        return s;
    }
    return s.substr(0, kMaxAuditString) + "... [truncated]";
}

void TrimCache(AuditReadCache& cache, std::size_t maxEvents) {
    const std::size_t cap = (std::max)(maxEvents, static_cast<std::size_t>(1000));
    while (cache.Events.size() > cap) {
        cache.Events.pop_front();
    }
}

nlohmann::json RedactJsonWithKey(const std::string& key, const nlohmann::json& value) {
    if (LooksSensitiveKey(key)) {
        return "[redacted]";
    }
    if (value.is_object()) {
        if (LooksLikeFieldDiffObject(value)) {
            return RedactFieldDiffObject(value);
        }
        nlohmann::json out = nlohmann::json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = RedactJsonWithKey(it.key(), it.value());
        }
        return out;
    }
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        std::transform(value.begin(), value.end(), std::back_inserter(out),
                       [&key](const auto& item) { return RedactJsonWithKey(key, item); });
        return out;
    }
    if (value.is_string()) {
        return TruncateAuditString(value.get<std::string>());
    }
    return value;
}

} // namespace

std::string GetAuditFilePath() {
    const std::string& base = ConfigManager::GetFilesBaseDirectory();
    if (base.empty()) {
        return "smatchet_backend_audit.jsonl";
    }
    return base + "smatchet_backend_audit.jsonl";
}

std::string MakeOperationId(const std::string& prefix) {
    static std::atomic<unsigned long long> counter{0};
    std::ostringstream os;
    os << (prefix.empty() ? "audit" : prefix) << "-" << NowEpochMs() << "-" << counter.fetch_add(1);
    return os.str();
}

nlohmann::json RedactJson(const nlohmann::json& value) { return RedactJsonWithKey(std::string(), value); }

std::string RedactText(const std::string& key, const std::string& value) {
    if (LooksSensitiveKey(key)) {
        return "[redacted]";
    }
    return TruncateAuditString(value);
}

nlohmann::json MakeFieldDiffUnknownBefore(const nlohmann::json& fields) {
    nlohmann::json diff = nlohmann::json::array();
    if (!fields.is_object()) {
        return diff;
    }
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        diff.push_back(nlohmann::json{{"field_id", it.key()},
                                      {"before", "unknown"},
                                      {"before_reason", "previous value not available at mutation boundary"},
                                      {"after", it.value()}});
    }
    return diff;
}

void AppendBegin(const std::string& action, const std::string& source, const std::string& issueKey,
                 const std::string& operationId, const nlohmann::json& data) {
    AuditEvent event;
    event.Action = action;
    event.Source = source;
    event.IssueKey = issueKey;
    event.OperationId = operationId;
    event.Phase = "begin";
    event.Success = true;
    event.Data = data;
    AppendEvent(event);
}

void AppendResult(const std::string& action, const std::string& source, const std::string& issueKey,
                  const std::string& operationId, bool success, const std::string& error, const nlohmann::json& data) {
    AuditEvent event;
    event.Action = action;
    event.Source = source;
    event.IssueKey = issueKey;
    event.OperationId = operationId;
    event.Phase = "result";
    event.Success = success;
    event.Error = error;
    event.Data = data;
    AppendEvent(event);
}

void AppendEvent(const AuditEvent& event) {
    try {
        nlohmann::json j = nlohmann::json::object();
        j["timestamp_ms"] = NowEpochMs();
        j["timestamp_local"] = NowLocalIso();
        j["action"] = event.Action;
        j["source"] = event.Source.empty() ? "app" : event.Source;
        j["issue_key"] = event.IssueKey;
        j["operation_id"] = event.OperationId.empty() ? MakeOperationId(event.Action) : event.OperationId;
        j["phase"] = event.Phase.empty() ? "result" : event.Phase;
        j["success"] = event.Success;
        if (!event.Error.empty()) {
            j["error"] = RedactText("error", event.Error);
        }
        j["data"] = RedactJson(event.Data);

        std::lock_guard<std::mutex> lock(AuditMutex());
        std::ofstream file(GetAuditFilePath(), std::ios::app | std::ios::binary);
        if (!file.is_open()) {
            return;
        }
        file << j.dump() << '\n';
    } catch (...) {
        // Audit must never block or fail backend mutations.
    }
}

std::vector<nlohmann::json> ReadRecentEvents(std::size_t maxEvents, std::string* outError) {
    if (outError) {
        outError->clear();
    }
    std::vector<nlohmann::json> out;
    try {
        std::lock_guard<std::mutex> lock(AuditMutex());
        const std::string path = GetAuditFilePath();
        AuditReadCache& cache = ReaderCache();
        if (cache.Path != path) {
            cache = AuditReadCache{};
            cache.Path = path;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            cache.Offset = std::streampos(0);
            cache.Events.clear();
            return out;
        }
        file.seekg(0, std::ios::end);
        const std::streampos end = file.tellg();
        if (end < cache.Offset) {
            cache.Offset = std::streampos(0);
            cache.Events.clear();
        }
        file.clear();
        file.seekg(cache.Offset);
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }
            try {
                cache.Events.push_back(nlohmann::json::parse(line));
            } catch (...) {
                // Skip corrupt partial lines.
            }
        }
        cache.Offset = file.eof() ? end : file.tellg();
        TrimCache(cache, maxEvents);
        out.reserve((std::min)(maxEvents, cache.Events.size()));
        const std::size_t skip = cache.Events.size() > maxEvents ? cache.Events.size() - maxEvents : 0;
        for (std::size_t i = skip; i < cache.Events.size(); ++i) {
            out.push_back(cache.Events[i]);
        }
    } catch (const std::exception& ex) {
        if (outError) {
            *outError = ex.what();
        }
    } catch (...) {
        if (outError) {
            *outError = "Unknown audit read error.";
        }
    }
    return out;
}

} // namespace BackendAuditTrail






