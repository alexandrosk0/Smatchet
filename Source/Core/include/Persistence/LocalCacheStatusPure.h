#pragma once

// LocalCacheStatusPure — the vocabulary for "what state is the local cache in, and what does the user need to be
// told". Pure and header-only (nlohmann + the stdlib), so every wording rule is unit-testable without a window, a
// database or a localization table.
//
// Terms:
//   Opening      the open is running and the recreate gate is up.
//   Ready        the durable file is open.
//   SessionOnly  the durable open failed and an in-memory cache serves reads for this session; durable enqueues
//                are refused with an honest reason (CacheEnqueueBlock::SessionOnly).
// "Using a legacy path" is a flag on the status, not a state: the durable file is one found at an old location.

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace persistence {

enum class LocalCacheState : std::uint8_t { Opening, Ready, SessionOnly };

/// Why a durable offline enqueue is refused right now. None means it is allowed.
enum class CacheEnqueueBlock : std::uint8_t { None, Opening, Recreating, SessionOnly };

/// What a corrupt-file rebuild did, persisted so the next launch can still tell the user. `Rebuilt` false means no
/// rebuild has happened (the default, and what a missing or unreadable record decodes to).
struct LocalCacheRecoveryReport {
    bool Rebuilt = false;
    std::int64_t WhenSec = 0;
    std::string QuarantinedPath;
    std::string Reason;
    bool SalvageAttempted = false;
    std::int64_t SalvagedRows = 0;
    /// Tables whose rows could not be carried over (the whole salvage list when the attach itself failed).
    std::vector<std::string> IncompleteTables;
    /// True only on the launch that did the rebuild, so a later launch does not repeat the notice. Never stored.
    bool FromThisLaunch = false;
};

/// The record's schema version, so an older build ignores a newer shape instead of misreading it.
constexpr int kRecoveryReportVersion = 1;
/// A stored record is small; a larger one is not ours.
constexpr std::size_t kMaxRecoveryReportBytes = 64u * 1024u;

inline std::string EncodeRecoveryReport(const LocalCacheRecoveryReport& report) {
    nlohmann::json j = nlohmann::json::object();
    j["v"] = kRecoveryReportVersion;
    j["rebuilt"] = report.Rebuilt;
    j["when"] = report.WhenSec;
    j["quarantined"] = report.QuarantinedPath;
    j["reason"] = report.Reason;
    j["salvage_attempted"] = report.SalvageAttempted;
    j["salvaged_rows"] = report.SalvagedRows;
    nlohmann::json tables = nlohmann::json::array();
    for (std::size_t i = 0; i < report.IncompleteTables.size(); ++i) {
        tables.push_back(report.IncompleteTables[i]);
    }
    j["incomplete"] = tables;
    return j.dump();
}

/// False (and `out` untouched) for anything that is not a well-formed, current-version record.
inline bool DecodeRecoveryReport(const std::string& text, LocalCacheRecoveryReport& out) {
    std::string err;
    const nlohmann::json j = json_safe::ParseBounded(text, err, kMaxRecoveryReportBytes);
    if (!err.empty() || !j.is_object()) {
        return false;
    }
    const nlohmann::json::const_iterator version = j.find("v");
    if (version == j.end() || !version->is_number_integer() || version->get<int>() != kRecoveryReportVersion) {
        return false;
    }
    LocalCacheRecoveryReport parsed;
    const nlohmann::json::const_iterator rebuilt = j.find("rebuilt");
    const nlohmann::json::const_iterator when = j.find("when");
    const nlohmann::json::const_iterator quarantined = j.find("quarantined");
    const nlohmann::json::const_iterator reason = j.find("reason");
    const nlohmann::json::const_iterator attempted = j.find("salvage_attempted");
    const nlohmann::json::const_iterator rows = j.find("salvaged_rows");
    const nlohmann::json::const_iterator incomplete = j.find("incomplete");
    if (rebuilt == j.end() || !rebuilt->is_boolean() || when == j.end() || !when->is_number_integer() ||
        quarantined == j.end() || !quarantined->is_string() || reason == j.end() || !reason->is_string() ||
        attempted == j.end() || !attempted->is_boolean() || rows == j.end() || !rows->is_number_integer() ||
        incomplete == j.end() || !incomplete->is_array()) {
        return false;
    }
    parsed.Rebuilt = rebuilt->get<bool>();
    parsed.WhenSec = when->get<std::int64_t>();
    parsed.QuarantinedPath = quarantined->get<std::string>();
    parsed.Reason = reason->get<std::string>();
    parsed.SalvageAttempted = attempted->get<bool>();
    parsed.SalvagedRows = rows->get<std::int64_t>();
    for (nlohmann::json::const_iterator it = incomplete->begin(); it != incomplete->end(); ++it) {
        if (!it->is_string()) {
            return false;
        }
        parsed.IncompleteTables.push_back(it->get<std::string>());
    }
    out = parsed;
    return true;
}

/// The cache as the rest of the app sees it. Copied out under a lock by the owner, so a plain value.
struct LocalCacheStatus {
    /// Ready until an async open sets Opening: a cache published without an open phase is simply there.
    LocalCacheState State = LocalCacheState::Ready;
    /// The file the cache was asked to open (the session-only cache itself is `:memory:`).
    std::string Path;
    /// Why the durable open failed (SessionOnly), else empty.
    std::string LastError;
    bool UsingLegacyPath = false;
    std::string LegacyPath;
    std::string ImportedFrom;
    std::vector<std::string> OtherLegacyCaches;
    LocalCacheRecoveryReport LastRecovery;
};

/// Already-localized wording with `{path}`, `{error}`, `{rows}` and `{quarantined}` placeholders, so the pure
/// builder never reaches for a localization table.
struct LocalCacheNoticeStrings {
    std::string SessionOnly;
    std::string Rebuilt;
    std::string RebuiltSalvaged;
    std::string RebuiltSalvageIncomplete;
};

namespace local_cache_status_detail {

struct Placeholder {
    const char* Token;
    std::string Value;
};

/// Substitutes every `{token}` of `text` in ONE pass over the template, so a value that itself contains a placeholder
/// (a path with `{error}` in it) is never expanded a second time. An unknown token is left as written.
inline std::string Expand(const std::string& text, const std::vector<Placeholder>& placeholders) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        bool matched = false;
        if (text[at] == '{') {
            for (std::size_t i = 0; i < placeholders.size(); ++i) {
                const std::string token = placeholders[i].Token;
                if (text.compare(at, token.size(), token) == 0) {
                    out += placeholders[i].Value;
                    at += token.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += text[at];
            ++at;
        }
    }
    return out;
}

} // namespace local_cache_status_detail

/// The one-line startup notice for `status`, or "" when there is nothing to say. A session-only cache always
/// says so; a rebuild is announced only on the launch that did it.
inline std::string DescribeLocalCacheStartupNotice(const LocalCacheStatus& status,
                                                   const LocalCacheNoticeStrings& strings) {
    using local_cache_status_detail::Expand;
    using local_cache_status_detail::Placeholder;
    if (status.State == LocalCacheState::SessionOnly) {
        std::vector<Placeholder> values;
        values.push_back(Placeholder{"{path}", status.Path});
        values.push_back(Placeholder{"{error}", status.LastError});
        return Expand(strings.SessionOnly, values);
    }
    const LocalCacheRecoveryReport& recovery = status.LastRecovery;
    if (status.State != LocalCacheState::Ready || !recovery.Rebuilt || !recovery.FromThisLaunch) {
        return std::string();
    }
    const std::string& tmpl = !recovery.SalvageAttempted          ? strings.Rebuilt
                              : recovery.IncompleteTables.empty() ? strings.RebuiltSalvaged
                                                                  : strings.RebuiltSalvageIncomplete;
    std::vector<Placeholder> values;
    values.push_back(Placeholder{"{rows}", std::to_string(recovery.SalvagedRows)});
    values.push_back(Placeholder{"{quarantined}", recovery.QuarantinedPath});
    return Expand(tmpl, values);
}

} // namespace persistence
} // namespace smatchet
