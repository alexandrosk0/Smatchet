#include "PlaneClient.h"
#include "MarkdownConvert.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "StringUtil.h"
#include "IssueDraft.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cpr/cpr.h>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

void TrimAsciiWs(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.erase(0, 1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.pop_back();
    }
}

/// Split URL authority into host (lowercase) and port suffix e.g. ":443" (may be empty).
void ParseHttpAuthority(const std::string& base, size_t hostStart, size_t hostEnd, std::string& outHostLower,
                        std::string& outPortSuffix) {
    const std::string auth = base.substr(hostStart, hostEnd - hostStart);
    outPortSuffix.clear();
    outHostLower = auth;
    const size_t colon = auth.find(':');
    if (colon != std::string::npos) {
        outHostLower = auth.substr(0, colon);
        outPortSuffix = auth.substr(colon);
    }
    std::transform(outHostLower.begin(), outHostLower.end(), outHostLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
}

/// Cloud Plane SPA is app.plane.so; REST is api.plane.so. Any path on app URL is UI, not API — strip to origin.
/// Host:port supported (e.g. app.plane.so:443). Self-hosted URLs pass through unchanged (except trim).
std::string NormalizePlaneApiBase(std::string base) {
    TrimAsciiWs(base);
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    const auto scheme = base.find("://");
    if (scheme == std::string::npos) {
        return base;
    }
    const size_t hostStart = scheme + 3;
    size_t hostEnd = base.find('/', hostStart);
    if (hostEnd == std::string::npos) {
        hostEnd = base.size();
    }
    std::string hostLower;
    std::string portSuffix;
    ParseHttpAuthority(base, hostStart, hostEnd, hostLower, portSuffix);
    const std::string schemePrefix = base.substr(0, hostStart); // "https://"
    if (hostLower == "app.plane.so") {
        return schemePrefix + std::string("api.plane.so") + portSuffix;
    }
    if (hostLower == "api.plane.so") {
        return schemePrefix + std::string("api.plane.so") + portSuffix;
    }
    return base;
}

/// Browser deep links use app.plane.so; strip accidental /api paths on cloud api host.
std::string NormalizePlaneWebBase(std::string base) {
    TrimAsciiWs(base);
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    const auto scheme = base.find("://");
    if (scheme == std::string::npos) {
        return base;
    }
    const size_t hostStart = scheme + 3;
    size_t hostEnd = base.find('/', hostStart);
    if (hostEnd == std::string::npos) {
        hostEnd = base.size();
    }
    std::string hostLower;
    std::string portSuffix;
    ParseHttpAuthority(base, hostStart, hostEnd, hostLower, portSuffix);
    const std::string schemePrefix = base.substr(0, hostStart);
    if (hostLower == "api.plane.so") {
        return schemePrefix + std::string("app.plane.so") + portSuffix;
    }
    if (hostLower == "app.plane.so") {
        return schemePrefix + std::string("app.plane.so") + portSuffix;
    }
    return base;
}

bool LooksHtmlPrefix(const std::string& t) {
    size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])) != 0) {
        ++i;
    }
    return i < t.size() && t[i] == '<';
}

std::string SanitizeAsciiSnippet(const std::string& s, size_t maxLen) {
    std::string o;
    o.reserve(std::min(maxLen, s.size()));
    for (size_t i = 0; i < s.size() && o.size() < maxLen; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            o += static_cast<char>(c);
        } else {
            o += '?';
        }
    }
    return o;
}
} // namespace

PlaneClient::PlaneClient() : planeProjectId_(""), planeProjectIdentifier_("") {}

PlaneClient::~PlaneClient() {}

std::unordered_map<std::string, std::string> PlaneClient::BuildPlaneHeaders(const TrackerConfig& cfg) {
    return {{"Accept", "application/json"}, {"Content-Type", "application/json"}, {"x-api-key", cfg.PlaneApiKey}};
}

namespace {
/// Safely extract a JSON field as a string regardless of its actual type (int, bool, null, etc.).
std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return {};
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    return v.dump(); // fallback: JSON serialization
}

std::string PlaneWorkItemTitleForDisplay(const nlohmann::json& issue) {
    std::string title = JsonFieldToString(issue, "name");
    TrimAsciiWs(title);
    return title;
}

bool LooksLikeUuid(const std::string& s) {
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
            return false;
    }
    return true;
}

bool ResolvePlaneProject(const std::string& planeApi, const TrackerConfig& cfg, const std::string& projectKey,
                         const cpr::Header& headers, std::string& outId, std::string& outIdentifier,
                         std::string* outError) {
    const std::string projectsUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/";
    cpr::Parameters params;
    params.Add({"per_page", "100"});
    auto response = TrackerGetLogged("PlaneClient", projectsUrl, headers, params);
    if (response.status_code != 200) {
        const std::string err = "Plane API error " + std::to_string(response.status_code) + " resolving project '" +
                                projectKey + "': " + response.text.substr(0, 300);
        if (outError)
            *outError = err;
        return false;
    }

    const nlohmann::json j = nlohmann::json::parse(StripUtf8BomCopy(response.text), nullptr, false);
    if (j.is_discarded()) {
        const std::string err =
            "Plane project list returned invalid JSON while resolving project '" + projectKey + "'.";
        if (outError)
            *outError = err;
        return false;
    }

    const auto projects = (j.is_object() && j.contains("results")) ? j["results"] : j;
    if (!projects.is_array()) {
        const std::string err =
            "Plane project list response has no results array while resolving project '" + projectKey + "'.";
        if (outError)
            *outError = err;
        return false;
    }

    std::string available;
    for (const auto& p : projects) {
        const std::string id = JsonFieldToString(p, "id");
        const std::string identifier = JsonFieldToString(p, "identifier");
        const std::string name = JsonFieldToString(p, "name");
        if (id == projectKey || identifier == projectKey || name == projectKey) {
            outId = id;
            outIdentifier = identifier;
            return true;
        }
        if (available.size() < 220) {
            if (!available.empty())
                available += ", ";
            available += identifier.empty() ? name : identifier;
        }
    }

    const std::string err = "Plane project '" + projectKey + "' was not found in workspace '" + cfg.PlaneWorkspaceSlug +
                            "'. Available project identifiers/names: " + available;
    if (outError)
        *outError = err;
    return false;
}

std::string ToUpperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

void AppendPagedResults(const std::string& listUrl, const cpr::Header& headers, std::vector<nlohmann::json>& outRows,
                        std::string* outWarn) {
    std::string cursor;
    for (int page = 0; page < 100; ++page) {
        cpr::Parameters params;
        params.Add({"per_page", "100"});
        if (!cursor.empty()) {
            params.Add({"cursor", cursor});
        }
        auto response = TrackerGetLogged("PlaneClient", listUrl, headers, params);
        if (response.status_code != 200) {
            if (outWarn && outWarn->size() < 400) {
                if (!outWarn->empty())
                    *outWarn += "; ";
                *outWarn += "HTTP " + std::to_string(response.status_code) + " on " + listUrl.substr(0, 80);
            }
            return;
        }
        const nlohmann::json j = nlohmann::json::parse(StripUtf8BomCopy(response.text), nullptr, false);
        if (j.is_discarded()) {
            if (outWarn && outWarn->size() < 400) {
                if (!outWarn->empty())
                    *outWarn += "; ";
                *outWarn += "Invalid JSON from " + listUrl.substr(0, 80);
            }
            return;
        }
        auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
        if (!results.is_array()) {
            return;
        }
        std::copy_if(results.begin(), results.end(), std::back_inserter(outRows),
                     [](const auto& row) { return row.is_object(); });
        bool more = false;
        if (j.is_object() && j.contains("next_page_results") && j["next_page_results"].is_boolean()) {
            more = j["next_page_results"].get<bool>();
        }
        cursor.clear();
        if (more && j.is_object() && j.contains("next_cursor") && j["next_cursor"].is_string()) {
            cursor = j["next_cursor"].get<std::string>();
        }
        if (!more || cursor.empty()) {
            break;
        }
    }
}

bool TrackerFieldFromPlaneProperty(const nlohmann::json& prop, TrackerField& out) {
    const std::string id = JsonFieldToString(prop, "id");
    if (id.empty()) {
        return false;
    }
    out.Id = id;
    out.Name = JsonFieldToString(prop, "display_name");
    if (out.Name.empty()) {
        out.Name = JsonFieldToString(prop, "name");
    }
    if (out.Name.empty()) {
        out.Name = out.Id;
    }
    const std::string pt = ToUpperAscii(JsonFieldToString(prop, "property_type"));
    out.IsCustom = true;
    out.ReadOnly = prop.value("is_readonly", false);
    if (prop.contains("is_required") && prop["is_required"].is_boolean()) {
        out.IsRequired = prop["is_required"].get<bool>();
    }

    if (pt == "NUMBER" || pt == "INTEGER" || pt == "DECIMAL" || pt == "FLOAT") {
        out.Type = "number";
        out.Family = TrackerFieldFamily::Number;
    } else if (pt == "DATE") {
        out.Type = "date";
        out.Family = TrackerFieldFamily::Date;
    } else if (pt == "DATETIME" || pt == "TIMESTAMP") {
        out.Type = "datetime";
        out.Family = TrackerFieldFamily::DateTime;
    } else if (pt == "OPTION" || pt == "DROPDOWN" || pt == "SELECT") {
        out.Type = "option";
        out.Family = TrackerFieldFamily::SelectSingle;
        if (prop.contains("options") && prop["options"].is_array()) {
            for (const auto& opt : prop["options"]) {
                if (!opt.is_object()) {
                    continue;
                }
                TrackerFieldOption o;
                o.Id = JsonFieldToString(opt, "id");
                o.Value = JsonFieldToString(opt, "value");
                if (o.Value.empty()) {
                    o.Value = JsonFieldToString(opt, "name");
                }
                if (!o.Id.empty() || !o.Value.empty()) {
                    out.AllowedValueOptions.push_back(std::move(o));
                }
            }
        }
    } else if (pt == "MULTI_SELECT" || pt == "MULTISELECT") {
        out.Type = "array";
        out.IsArray = true;
        out.Family = TrackerFieldFamily::SelectMulti;
    } else if (pt == "MEMBER" || pt == "USER") {
        out.Type = "user";
        out.Family = TrackerFieldFamily::UserSingle;
        out.IsUserType = true;
    } else if (pt == "MULTI_MEMBER" || pt == "MULTI_USER" || pt == "USERS") {
        out.Type = "array";
        out.IsArray = true;
        out.Family = TrackerFieldFamily::UserMulti;
        out.IsUserType = true;
    } else if (pt == "BOOLEAN" || pt == "CHECKBOX") {
        out.Type = "boolean";
        out.Family = TrackerFieldFamily::Text;
    } else {
        // URL / LINK / RICH_TEXT / HTML / TEXT / empty / unknown — all collapse to the
        // string+Text default. Kept as a single branch so cppcheck doesn't flag duplicate
        // bodies; if any of these grow special handling in the future, split them back out.
        out.Type = "string";
        out.Family = TrackerFieldFamily::Text;
    }
    try {
        out.RawFieldDefinitionJson = prop.dump();
    } catch (...) {
        out.RawFieldDefinitionJson.clear();
    }
    return true;
}

} // namespace

std::vector<CachedTicket> PlaneClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                                   const ViewsStore* viewsOverride, std::string* outFetchError,
                                                   std::string* outWarning) {
    std::vector<CachedTicket> results;
    auto onBatch = [&](std::vector<CachedTicket>&& batch) {
        results.insert(results.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    };
    auto shouldCancel = []() { return false; };
    TrackerIssueFetchSummary summary = FetchIssuesStreamed(onBatch, shouldCancel, configOverride, viewsOverride);
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = summary.FullSyncCompleted;
    }
    if (outFetchError) {
        *outFetchError = summary.FetchError;
    }
    if (outWarning) {
        *outWarning = summary.Warning;
    }
    return results;
}

TrackerIssueFetchSummary PlaneClient::FetchIssuesStreamed(const BatchCallback& onBatch,
                                                          const CancelCallback& shouldCancel,
                                                          const TrackerConfig* configOverride,
                                                          const ViewsStore* /*viewsOverride*/) {

    TrackerIssueFetchSummary summary;

    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();

    // PR 6: legacy global cfg.PlaneProjectId removed. The active project is extracted from the
    // active view's query (PR 5 sweep ensures legacy views carry their project in the saved
    // query). Empty here ≡ "no project scope" — surfaces the same "configure" error as before.
    // See docs/design/applied/remove-global-project-key.md §2.5 / §7 PR 6.
    const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);

    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || projectKey.empty()) {
        summary.FetchError = "Plane is not configured or active view has no project scope. "
                             "Set URL / Workspace Slug in Preferences, and choose a Plane view with a project.";
        return summary;
    }
    if (cfg.PlaneApiKey.empty()) {
        summary.FetchError = "Plane API key is missing. Set it in Preferences → Tracker.";
        return summary;
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    if (planeApi != cfg.PlaneUrl) {
        LOG_INFO(
            "PlaneClient::FetchIssuesStreamed: REST base %s (configured URL was web app host; use https://api.plane.so "
            "in preferences to skip this rewrite).",
            planeApi.c_str());
    }

    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string prevProjectId;
    std::string prevProjectIdentifier;
    std::vector<TrackerUser> localCachedUsers;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        prevProjectId = planeProjectId_;
        prevProjectIdentifier = planeProjectIdentifier_;
        localCachedUsers = cachedUsers_;
    }

    std::string resolveError;
    std::string tempProjectId = prevProjectId;
    std::string tempProjectIdentifier = prevProjectIdentifier;
    if (!ResolvePlaneProject(planeApi, cfg, projectKey, headers, tempProjectId, tempProjectIdentifier, &resolveError)) {
        summary.FetchError = resolveError;
        return summary;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        planeProjectId_ = tempProjectId;
        planeProjectIdentifier_ = tempProjectIdentifier;
        if (planeProjectId_ != prevProjectId) {
            keyToId_.clear();
        }
    }
    const std::string planeProjectId = tempProjectId;

    std::unordered_map<std::string, std::string> localKeyToId;

    try {
        // Fetch states for display value mapping
        {
            const std::string statesUrl =
                planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/states/";
            auto r = TrackerGetLogged("PlaneClient", statesUrl, headers);
            if (r.status_code == 200) {
                const std::string statesBody = StripUtf8BomCopy(r.text);
                nlohmann::json j = nlohmann::json::parse(statesBody, nullptr, false);
                if (!j.is_discarded()) {
                    std::vector<CachedState> tempCachedStates;
                    auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
                    if (results.is_array()) {
                        for (const auto& s : results) {
                            CachedState cs;
                            cs.Id = JsonFieldToString(s, "id");
                            cs.Name = JsonFieldToString(s, "name");
                            if (!cs.Id.empty())
                                tempCachedStates.push_back(cs);
                        }
                    }
                    {
                        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
                        cachedStates_ = std::move(tempCachedStates);
                    }
                }
            }
        }

        const int pageSize = 100;
        // Hard cap on outer pagination to bound a misbehaving cursor that loops back to itself.
        // 50 pages × 100 work-items/page = 5,000 issues, comfortably above any active-view JQL
        // and matches the per-server safety limit in JiraIssueSearch.cpp.
        constexpr int kMaxPlanePages = 50;
        std::string listCursor;
        bool syncEndedCleanly = false;
        int pageCount = 0;

        while (true) {
            if (shouldCancel && shouldCancel()) {
                syncEndedCleanly = false;
                break;
            }

            if (pageCount >= kMaxPlanePages) {
                const std::string warn = "Plane pagination outer page cap (" + std::to_string(kMaxPlanePages) +
                                         ") reached; remaining issues not fetched. Narrow your view or raise the cap.";
                LOG_WARN("PlaneClient::FetchIssuesStreamed %s", warn.c_str());
                // Soft warning: the issues we did fetch are valid; treat as a partial success
                // rather than a fetch failure so the UI fires its success notify and the
                // connectivity banner stays clear.
                summary.Warning = warn;
                break;
            }
            ++pageCount;

            const std::string listBase = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                         planeProjectId + "/work-items/";
            cpr::Parameters params;
            params.Add({"per_page", std::to_string(pageSize)});
            params.Add({"expand", "assignees,labels,state"});
            if (!listCursor.empty()) {
                params.Add({"cursor", listCursor});
            }

            auto response = TrackerGetLogged("PlaneClient", listBase, headers, params);

            if (response.status_code != 200) {
                const std::string urlHint = SanitizeAsciiSnippet(listBase, 200);
                std::string apiDetail;
                {
                    const std::string tb = StripUtf8BomCopy(response.text);
                    const nlohmann::json ej = nlohmann::json::parse(tb, nullptr, false);
                    if (!ej.is_discarded() && ej.is_object() && ej.contains("detail")) {
                        apiDetail = JsonFieldToString(ej, "detail");
                    }
                }
                std::string err = "Plane API error " + std::to_string(response.status_code) +
                                  " fetching issues (URL: " + urlHint + "): " + response.text.substr(0, 300);
                if (!apiDetail.empty()) {
                    err += " [detail: " + apiDetail + "]";
                }
                if (response.status_code == 404) {
                    err += " Check Workspace Slug and Project ID (UUID from app URL or Plane settings), and API key "
                           "scopes.";
                }
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            const std::string bodyForJson = StripUtf8BomCopy(response.text);
            const bool looksHtml = LooksHtmlPrefix(bodyForJson);

            if (bodyForJson.empty()) {
                const std::string err = "Plane returned empty response body (HTTP 200) when fetching issues.";
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }
            if (looksHtml) {
                const std::string urlHint = SanitizeAsciiSnippet(listBase, 220);
                const std::string err =
                    "Plane returned HTML instead of JSON (HTTP 200). Request URL: " + urlHint +
                    ". For Plane Cloud set base URL to https://api.plane.so (origin only, no /workspace path). "
                    "Self-hosted: use the API origin your reverse proxy serves for /api/v1/.";
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            nlohmann::json j = nlohmann::json::parse(bodyForJson, nullptr, false);
            if (j.is_discarded()) {
                const std::string err =
                    "Plane returned invalid JSON when fetching issues (HTTP 200). Verify Plane URL, workspace slug, "
                    "project UUID, and API key.";
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
            if (!results.is_array()) {
                std::string keyList;
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end() && keyList.size() < 240; ++it) {
                        if (!keyList.empty())
                            keyList += ',';
                        keyList += it.key();
                    }
                }
                const std::string err =
                    "Plane list response has no results array (wrong endpoint or API version). Top-level keys: " +
                    keyList;
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            if (results.empty()) {
                syncEndedCleanly = true;
                break;
            }

            std::vector<CachedTicket> pageIssues;
            for (const auto& issue : results) {
                if (shouldCancel && shouldCancel()) {
                    break;
                }
                try {
                    CachedTicket ticket;
                    const std::string uuid = JsonFieldToString(issue, "id");
                    const std::string seqId = JsonFieldToString(issue, "sequence_id");

                    std::string visualKey;
                    if (!tempProjectIdentifier.empty() && !seqId.empty()) {
                        visualKey = tempProjectIdentifier + "-" + seqId;
                    } else if (!seqId.empty()) {
                        visualKey = "#" + seqId;
                    } else {
                        visualKey = uuid;
                    }

                    ticket.id = visualKey;
                    ticket.fieldValues["uuid"] = uuid;
                    ticket.fieldValues["key"] = visualKey;
                    localKeyToId[visualKey] = uuid;
                    ticket.fieldValues["summary"] = PlaneWorkItemTitleForDisplay(issue);

                    // Status
                    if (issue.contains("state_detail") && issue["state_detail"].is_object()) {
                        ticket.fieldValues["status"] = JsonFieldToString(issue["state_detail"], "id");
                    } else if (issue.contains("state") && issue["state"].is_object()) {
                        ticket.fieldValues["status"] = JsonFieldToString(issue["state"], "id");
                    } else {
                        ticket.fieldValues["status"] = JsonFieldToString(issue, "state");
                    }

                    // Assignee
                    std::string assigneeId;
                    if (issue.contains("assignees") && issue["assignees"].is_array() && !issue["assignees"].empty()) {
                        const auto& first = issue["assignees"][0];
                        if (first.is_object()) {
                            assigneeId = JsonFieldToString(first, "id");
                        } else if (first.is_string()) {
                            assigneeId = first.get<std::string>();
                        }
                    }

                    std::string assigneeName = assigneeId;
                    if (issue.contains("assignee_details") && issue["assignee_details"].is_array() &&
                        !issue["assignee_details"].empty()) {
                        const auto& first = issue["assignee_details"][0];
                        if (first.is_object() && first.contains("display_name")) {
                            assigneeName = JsonFieldToString(first, "display_name");
                        }
                    }

                    if (assigneeName == assigneeId && !assigneeId.empty()) {
                        auto uIt = std::find_if(localCachedUsers.begin(), localCachedUsers.end(),
                                                [&](const auto& u) { return u.AccountId == assigneeId; });
                        if (uIt != localCachedUsers.end()) {
                            assigneeName = uIt->DisplayName;
                        }
                    }

                    ticket.fieldValues["assignee"] = assigneeName;
                    ticket.fieldValues["priority"] = JsonFieldToString(issue, "priority");

                    // Sprint
                    if (issue.contains("cycle_details") && issue["cycle_details"].is_object()) {
                        ticket.fieldValues["sprint"] = JsonFieldToString(issue["cycle_details"], "id");
                    } else if (issue.contains("cycle") && issue["cycle"].is_object()) {
                        ticket.fieldValues["sprint"] = JsonFieldToString(issue["cycle"], "id");
                    } else {
                        ticket.fieldValues["sprint"] = JsonFieldToString(issue, "cycle");
                    }

                    // Labels
                    std::string labelStr;
                    if (issue.contains("label_details") && issue["label_details"].is_array()) {
                        for (const auto& lbl : issue["label_details"]) {
                            if (!labelStr.empty())
                                labelStr += ", ";
                            std::string ln = JsonFieldToString(lbl, "name");
                            if (ln.empty())
                                ln = JsonFieldToString(lbl, "id");
                            labelStr += ln;
                        }
                    } else if (issue.contains("labels") && issue["labels"].is_array()) {
                        for (const auto& lbl : issue["labels"]) {
                            if (!labelStr.empty())
                                labelStr += ", ";
                            if (lbl.is_object()) {
                                std::string ln = JsonFieldToString(lbl, "name");
                                if (ln.empty())
                                    ln = JsonFieldToString(lbl, "id");
                                labelStr += ln;
                            } else if (lbl.is_string()) {
                                labelStr += lbl.get<std::string>();
                            }
                        }
                    }
                    ticket.fieldValues["labels"] = labelStr;

                    ticket.fieldValues["created"] = JsonFieldToString(issue, "created_at");
                    ticket.fieldValues["updated"] = JsonFieldToString(issue, "updated_at");
                    ticket.fieldValues["description"] = JsonFieldToString(issue, "description_stripped");
                    // Preserve the original HTML so the long-text modal editor can round-trip via
                    // Markdown without destroying formatting on save. See RICH_TEXT_EDITING_V2_PLAN.md.
                    const std::string descHtml = JsonFieldToString(issue, "description_html");
                    if (!descHtml.empty()) {
                        ticket.fieldRichValues["description"] = descHtml;
                    }

                    // Issue Type
                    if (issue.contains("type_detail") && issue["type_detail"].is_object()) {
                        ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type_detail"], "name");
                    } else if (issue.contains("type") && issue["type"].is_object()) {
                        ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type"], "name");
                    } else {
                        ticket.fieldValues["issuetype"] = JsonFieldToString(issue, "type");
                    }

                    if (!ticket.id.empty()) {
                        pageIssues.push_back(std::move(ticket));
                    }
                } catch (const std::exception& ex) {
                    LOG_WARN("PlaneClient::FetchIssuesStreamed: skipping issue parse error: %s", ex.what());
                }
            }

            if (shouldCancel && shouldCancel()) {
                syncEndedCleanly = false;
                break;
            }

            size_t added = pageIssues.size();
            summary.FetchedCount += added;

            if (onBatch && added > 0) {
                onBatch(std::move(pageIssues));
            }

            // Cursor pagination
            listCursor.clear();
            bool more = false;
            if (j.is_object()) {
                if (j.contains("next_page_results") && j["next_page_results"].is_boolean()) {
                    more = j["next_page_results"].get<bool>();
                }
                if (more && j.contains("next_cursor") && j["next_cursor"].is_string()) {
                    listCursor = j["next_cursor"].get<std::string>();
                }
            }
            if (!more || listCursor.empty()) {
                syncEndedCleanly = true;
                break;
            }
        }
        summary.FullSyncCompleted = syncEndedCleanly && (!shouldCancel || !shouldCancel());
        LOG_INFO("PlaneClient::FetchIssuesStreamed fetched %zu issues from Plane.", summary.FetchedCount);
    } catch (const nlohmann::json::exception& jex) {
        LOG_ERROR("PlaneClient::FetchIssuesStreamed outer json error: %s", jex.what());
        summary.FetchError = std::string("Plane sync failed (JSON): ") + jex.what();
    } catch (const std::exception& ex) {
        LOG_ERROR("PlaneClient::FetchIssuesStreamed outer error: %s", ex.what());
        summary.FetchError = std::string("Plane sync failed: ") + ex.what();
    } catch (...) {
        LOG_ERROR("PlaneClient::FetchIssuesStreamed outer error: unknown exception");
        summary.FetchError = "Plane sync failed: unknown exception";
    }

    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        for (const auto& kv : localKeyToId) {
            keyToId_[kv.first] = kv.second;
        }
    }

    return summary;
}

TrackerReachabilityProbeResult PlaneClient::ProbeReachability(const TrackerConfig& cfg) {
    TrackerReachabilityProbeResult out;
    if (cfg.PlaneUrl.empty() || cfg.PlaneApiKey.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "Missing Plane URL or API key.";
        return out;
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    const std::string url = planeApi + "/api/v1/users/me/";
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    // Route through TrackerGetLogged so NetworkUsageTracker + body-trace logging stay in the
    // loop (was a §2.1 P1 bug: raw cpr::Get bypassed both). Classification via TrackerError
    // also fixes the second §2.1 P1: a 404 from a stale base URL was wrongly TransportDown.
    const cpr::Response resp =
        TrackerGetLogged("PlaneClient", url, headers, kTrackerProbeConnectTimeoutMs, kTrackerProbeOverallTimeoutMs);
    const TrackerHttpResult classified = ClassifyTrackerResponse(resp);

    switch (classified.Error.Kind) {
    case TrackerErrorKind::None:
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "HTTP 200";
        break;
    case TrackerErrorKind::Auth:
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status()) + " (Auth Error)";
        break;
    case TrackerErrorKind::ServerError:
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status()) + " (Server Error)";
        break;
    case TrackerErrorKind::NotFound:
    case TrackerErrorKind::InvalidRequest:
    case TrackerErrorKind::RateLimited:
        // Reachable, but the response indicates a config / payload issue rather than transport
        // failure. Surfaces as the auth-or-config banner so a stale base URL or rate-limited
        // probe doesn't flip the connectivity banner to "offline".
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status());
        break;
    case TrackerErrorKind::Transport:
    default:
        out.Kind = TrackerReachabilityProbeKind::TransportDown;
        out.Diagnostic = resp.error.message.empty() ? classified.Error.Detail : resp.error.message;
        break;
    }
    return out;
}

bool PlaneClient::FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKeyArg,
                                    TrackerFieldCatalogResult& outCatalog, std::string& outError) {
    outCatalog = TrackerFieldCatalogResult{};
    outError.clear();
    std::vector<std::string> warns;

    // PR 6: project is now an explicit per-call argument (legacy cfg.PlaneProjectId removed).
    // Empty ≡ unscoped: caller didn't pin a project, so refuse rather than silently fetching
    // a stale catalog. See remove-global-project-key.md §2.5 / §7 PR 6.
    const std::string projectKey = projectKeyArg;

    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || projectKey.empty()) {
        outError = "Plane is not configured or no project was supplied. "
                   "Set URL / Workspace Slug in Preferences, and pick a project before refreshing the field catalog.";
        return false;
    }
    if (cfg.PlaneApiKey.empty()) {
        outError = "Plane API key is missing. Set it in Preferences → Tracker.";
        return false;
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string resolvedProjectId;
    std::string resolvedProjectIdentifier;
    if (!ResolvePlaneProject(planeApi, cfg, projectKey, headers, resolvedProjectId, resolvedProjectIdentifier,
                             &outError)) {
        return false;
    }
    const std::string planeProjectId = resolvedProjectId;

    auto makeCore = [](const char* id, const char* name, const char* type, TrackerFieldFamily fam, bool readOnly) {
        TrackerField f;
        f.Id = id;
        f.Name = name;
        f.Type = type;
        f.Family = fam;
        f.ReadOnly = readOnly;
        f.IsUserType = (fam == TrackerFieldFamily::UserSingle || fam == TrackerFieldFamily::UserMulti);
        return f;
    };

    std::vector<TrackerField> fields;
    fields.reserve(32);
    fields.push_back(makeCore("summary", "Name", "string", TrackerFieldFamily::Text, false));
    fields.push_back(makeCore("description", "Description", "string", TrackerFieldFamily::Text, false));
    fields.push_back(makeCore("priority", "Priority", "string", TrackerFieldFamily::Text, false));

    std::vector<CachedState> localStates;
    const std::string statesUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/states/";
    std::string stateWarn;
    std::vector<nlohmann::json> stateRows;
    AppendPagedResults(statesUrl, headers, stateRows, &stateWarn);
    TrackerField statusField = makeCore("status", "State", "string", TrackerFieldFamily::Status, false);
    for (const auto& s : stateRows) {
        if (!s.is_object()) {
            continue;
        }
        CachedState cs;
        cs.Id = JsonFieldToString(s, "id");
        cs.Name = JsonFieldToString(s, "name");
        if (cs.Id.empty()) {
            continue;
        }
        localStates.push_back(cs);
        TrackerFieldOption opt;
        opt.Id = cs.Id;
        opt.Value = cs.Name.empty() ? cs.Id : cs.Name;
        statusField.AllowedValueOptions.push_back(std::move(opt));
    }
    if (localStates.empty() && !stateWarn.empty()) {
        warns.push_back(std::string("states: ") + stateWarn);
    } else if (localStates.empty()) {
        warns.push_back("No Plane states returned (empty list or unparsed response).");
    }
    fields.push_back(std::move(statusField));

    fields.push_back(makeCore("assignee", "Assignee", "user", TrackerFieldFamily::UserSingle, false));

    std::vector<CachedCycle> localCycles;
    TrackerField sprintField = makeCore("sprint", "Cycle", "string", TrackerFieldFamily::Sprint, false);
    const std::string cyclesUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/cycles/";
    std::string cycleWarn;
    std::vector<nlohmann::json> cycleRows;
    AppendPagedResults(cyclesUrl, headers, cycleRows, &cycleWarn);
    for (const auto& c : cycleRows) {
        if (!c.is_object()) {
            continue;
        }
        CachedCycle cc;
        cc.Id = JsonFieldToString(c, "id");
        cc.Name = JsonFieldToString(c, "name");
        if (cc.Id.empty()) {
            continue;
        }
        localCycles.push_back(cc);
        TrackerFieldOption opt;
        opt.Id = cc.Id;
        opt.Value = cc.Name.empty() ? cc.Id : cc.Name;
        sprintField.AllowedValueOptions.push_back(std::move(opt));
    }
    if (localCycles.empty() && !cycleWarn.empty()) {
        warns.push_back(std::string("cycles: ") + cycleWarn);
    }
    fields.push_back(std::move(sprintField));

    std::vector<TrackerUser> localUsers;
    const std::string membersUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/members/";
    std::vector<nlohmann::json> memberRows;
    std::string memberWarn;
    AppendPagedResults(membersUrl, headers, memberRows, &memberWarn);
    for (const auto& m : memberRows) {
        if (!m.is_object())
            continue;

        nlohmann::json u = m;
        if (m.contains("member") && m["member"].is_object()) {
            u = m["member"];
        } else if (m.contains("user") && m["user"].is_object()) {
            u = m["user"];
        }

        TrackerUser tu;
        tu.AccountId = JsonFieldToString(u, "id");
        if (tu.AccountId.empty() && m.contains("member") && m["member"].is_string()) {
            tu.AccountId = m["member"].get<std::string>(); // Sometimes member is just a UUID string!
        }

        tu.DisplayName = JsonFieldToString(u, "display_name");
        if (tu.DisplayName.empty()) {
            tu.DisplayName = JsonFieldToString(m, "display_name");
        }

        if (tu.DisplayName.empty()) {
            std::string fn = JsonFieldToString(u, "first_name");
            std::string ln = JsonFieldToString(u, "last_name");
            if (!fn.empty() || !ln.empty())
                tu.DisplayName = fn + (fn.empty() || ln.empty() ? "" : " ") + ln;
        }
        if (tu.DisplayName.empty() || tu.DisplayName == " ") {
            tu.DisplayName = JsonFieldToString(u, "email");
        }
        tu.EmailAddress = JsonFieldToString(u, "email");

        if (tu.AccountId.empty()) {
            tu.AccountId = JsonFieldToString(m, "id");
        }

        if (tu.AccountId.empty())
            continue;
        if (tu.DisplayName.empty())
            tu.DisplayName = tu.AccountId; // Fallback to ID so it's not empty

        localUsers.push_back(tu);
        outCatalog.Users.push_back(tu);
    }
    if (localUsers.empty() && !memberWarn.empty()) {
        warns.push_back(std::string("members: ") + memberWarn);
    }
    LOG_INFO("PlaneClient: Fetched %zu project members.", localUsers.size());

    std::vector<CachedLabel> localLabels;
    const std::string labelsUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/labels/";
    std::string labelsWarn;
    std::vector<nlohmann::json> labelRows;
    AppendPagedResults(labelsUrl, headers, labelRows, &labelsWarn);

    TrackerField labelsField = makeCore("labels", "Labels", "array", TrackerFieldFamily::Labels, false);
    for (const auto& l : labelRows) {
        if (!l.is_object())
            continue;
        CachedLabel cl;
        cl.Id = JsonFieldToString(l, "id");
        cl.Name = JsonFieldToString(l, "name");
        if (cl.Id.empty())
            continue;
        localLabels.push_back(cl);

        TrackerFieldOption opt;
        opt.Id = cl.Id;
        opt.Value = cl.Name.empty() ? cl.Id : cl.Name;
        labelsField.AllowedValueOptions.push_back(std::move(opt));
    }
    if (localLabels.empty() && !labelsWarn.empty()) {
        warns.push_back(std::string("labels: ") + labelsWarn);
    }
    LOG_INFO("PlaneClient: Fetched %zu project labels.", localLabels.size());
    fields.push_back(std::move(labelsField));

    fields.push_back(makeCore("created", "Created", "datetime", TrackerFieldFamily::DateTime, true));
    fields.push_back(makeCore("updated", "Updated", "datetime", TrackerFieldFamily::DateTime, true));

    const std::string typesUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/work-item-types/";
    std::vector<nlohmann::json> typeRows;
    std::string typeWarn;
    AppendPagedResults(typesUrl, headers, typeRows, &typeWarn);

    if (typeRows.empty()) {
        LOG_WARN("PlaneClient::FetchFieldCatalog: No work-item-types found for project %s. URL: %s. Error: %s",
                 planeProjectId.c_str(), typesUrl.c_str(), typeWarn.c_str());
        if (!typeWarn.empty())
            warns.push_back("work-item-types: " + typeWarn);
    }

    std::unordered_map<std::string, TrackerField> customs;

    for (const auto& tentry : typeRows) {
        if (!tentry.is_object()) {
            continue;
        }
        if (!tentry.value("deleted_at", nlohmann::json()).is_null()) {
            continue;
        }
        if (tentry.contains("is_active") && tentry["is_active"].is_boolean() && !tentry["is_active"].get<bool>()) {
            continue;
        }
        const std::string typeId = JsonFieldToString(tentry, "id");
        if (typeId.empty()) {
            continue;
        }

        const std::string propsUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                     planeProjectId + "/work-item-types/" + typeId + "/work-item-properties/";
        auto pResp = TrackerGetLogged("PlaneClient", propsUrl, headers);
        if (pResp.status_code != 200) {
            warns.push_back("work-item-properties HTTP " + std::to_string(pResp.status_code) + " for type " +
                            typeId.substr(0, 8));
            continue;
        }
        const nlohmann::json pj = nlohmann::json::parse(StripUtf8BomCopy(pResp.text), nullptr, false);
        if (pj.is_discarded()) {
            warns.push_back("work-item-properties invalid JSON for type " + typeId.substr(0, 8));
            continue;
        }
        nlohmann::json propList = (pj.is_object() && pj.contains("results")) ? pj["results"] : pj;
        if (!propList.is_array()) {
            continue;
        }
        for (const auto& prop : propList) {
            if (!prop.is_object()) {
                continue;
            }
            TrackerField tf;
            if (!TrackerFieldFromPlaneProperty(prop, tf)) {
                continue;
            }
            if (customs.find(tf.Id) != customs.end()) {
                continue;
            }
            customs.emplace(tf.Id, std::move(tf));
        }
    }
    fields.reserve(fields.size() + customs.size());
    std::transform(customs.begin(), customs.end(), std::back_inserter(fields),
                   [](auto& kv) { return std::move(kv.second); });

    // Populate user fields with options for the dropdowns
    if (!localUsers.empty()) {
        for (auto& field : fields) {
            if (!field.IsUserType) {
                continue;
            }
            field.AllowedValueOptions.clear();
            field.AllowedValueOptions.reserve(localUsers.size());
            for (const auto& user : localUsers) {
                TrackerFieldOption opt;
                opt.Id = user.AccountId;
                opt.Value = user.DisplayName;
                field.AllowedValueOptions.push_back(std::move(opt));
            }
        }
    }

    for (size_t i = 0; i < warns.size(); ++i) {
        if (i != 0) {
            outCatalog.Warning += "; ";
        }
        outCatalog.Warning += warns[i];
    }

    outCatalog.Fields = std::move(fields);

    // Securely publish the fully loaded local cache results under a quick brief lock.
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        planeProjectId_ = resolvedProjectId;
        planeProjectIdentifier_ = resolvedProjectIdentifier;
        cachedStates_ = std::move(localStates);
        cachedCycles_ = std::move(localCycles);
        cachedUsers_ = std::move(localUsers);
        cachedLabels_ = std::move(localLabels);
    }

    return true;
}

bool PlaneClient::FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/,
                                     std::unordered_map<std::string, bool>& outFieldIdCanEdit, std::string& outError) {
    outError.clear();
    outFieldIdCanEdit.clear();
    for (const char* fieldId : {"summary", "description", "priority", "status", "assignee", "labels", "sprint"}) {
        outFieldIdCanEdit[fieldId] = true;
    }
    return true;
}

std::string PlaneClient::BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const {
    // User wants: https://app.plane.so/<workspace-slug>/browse/<issue-key>/
    // NormalizePlaneWebBase trims trailing '/' from webBase; guard against a slug that starts with
    // '/' to prevent a double-slash like https://app.plane.so//workspace/browse/PROJ-1/.
    const std::string webBase = NormalizePlaneWebBase(cfg.PlaneUrl);
    const std::string& slug = cfg.PlaneWorkspaceSlug;
    const std::string sep = (!slug.empty() && slug[0] == '/') ? "" : "/";
    return webBase + sep + slug + "/browse/" + issueKey + "/";
}

bool PlaneClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) {
    // Resolve config + headers + project + UUID under the cache lock, then drop the lock before
    // the HTTP PATCH so UI thread calls to ResolveDisplayValue / display-name lookups are not
    // blocked for the duration of the round trip.
    TrackerConfig cfg = ConfigManager::Load();
    // PR 6: legacy cfg.PlaneProjectId removed. Resolve via the active view query (filled by the
    // current view; legacy installs swept by PR 5). UpdateIssueFields is invoked from grid edits
    // which are scoped to the current view → its query carries the project.
    const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string resolvedProjectId;
    std::string targetUuid;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        if (planeProjectId_.empty()) {
            // First-call HTTP under the lock is acceptable: it runs at most once per process
            // lifetime; the steady-state PATCH path below is the hot one that needed the fix.
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_,
                                &outError);
        }
        if (planeProjectId_.empty()) {
            return false;
        }
        resolvedProjectId = planeProjectId_;

        targetUuid = issueId;
        if (!LooksLikeUuid(issueId)) {
            auto it = keyToId_.find(issueId);
            if (it != keyToId_.end()) {
                targetUuid = it->second;
            } else {
                // If not in cache, we might be replaying an offline edit after restart.
                // In a real app we might need to search Plane for this key to get the UUID,
                // but for now we'll assume it's in the cache from a recent fetch.
                outError = "Could not resolve Plane visual key '" + issueId + "' to UUID. Try refreshing the grid.";
                return false;
            }
        }
    }

    const std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                            resolvedProjectId + "/work-items/" + targetUuid + "/";

    auto response = TrackerPatchLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "PATCH", url, response);

    if (response.status_code != 200 && response.status_code != 204) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(response.text);
            if (j.is_object() && j.contains("error"))
                detail = j["error"].dump();
            else if (j.is_object() && j.contains("detail"))
                detail = j["detail"].dump();
            else
                detail = response.text;
        } catch (...) {
            detail = response.text;
        }
        outError = "Plane API error: " + std::to_string(response.status_code) + " " + detail;
        return false;
    }
    return true;
}

bool PlaneClient::BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                                    nlohmann::json& outPayload, std::string& outError) {
    std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
    if (field.Id == "summary") {
        outPayload["name"] = values.empty() ? "" : values[0];
    } else if (field.Id == "description") {
        // Modal editor produces Markdown; convert to the HTML subset Plane accepts. Empty input
        // round-trips to empty string. The modal's raw-mode save (kicks in for HTML the converter
        // can't safely round-trip) ships verbatim — sniffed by "starts with `<` and contains
        // a closing tag", which is essentially never true for real user-typed Markdown. See
        // RICH_TEXT_EDITING_V2_PLAN.md.
        const std::string& md = values.empty() ? std::string() : values[0];
        if (md.empty()) {
            outPayload["description_html"] = std::string();
        } else {
            size_t firstNonWs = 0;
            while (firstNonWs < md.size() && std::isspace(static_cast<unsigned char>(md[firstNonWs])))
                ++firstNonWs;
            const bool looksLikeRawHtml =
                firstNonWs < md.size() && md[firstNonWs] == '<' && md.find("</") != std::string::npos;
            outPayload["description_html"] = looksLikeRawHtml ? md : MarkdownConvert::MarkdownToHtml(md);
        }
    } else if (field.Id == "priority") {
        outPayload["priority"] = values.empty() ? "medium" : values[0];
    } else if (field.Id == "status") {
        outPayload["state"] = values.empty() ? "" : values[0];
    } else if (field.Id == "assignee") {
        outPayload["assignee"] = values.empty() ? nullptr : nlohmann::json(values[0]);
    } else if (field.Id == "labels") {
        std::vector<std::string> labelIds;
        for (const auto& val : values) {
            auto lIt =
                std::find_if(cachedLabels_.begin(), cachedLabels_.end(), [&](const auto& l) { return l.Name == val; });
            std::string id = (lIt != cachedLabels_.end()) ? lIt->Id : val;
            labelIds.push_back(id);
        }
        outPayload["labels"] = labelIds;
    } else if (field.Id == "sprint") {
        outPayload["cycle"] = values.empty() ? nullptr : nlohmann::json(values[0]);
    } else {
        outError = "Field not supported for update in Plane: " + field.Id;
        return false;
    }
    return true;
}

bool PlaneClient::UpdateField(const std::string& issueId, const TrackerField& field,
                              const std::vector<std::string>& values, std::string& outError) {
    nlohmann::json payload;
    if (!BuildFieldPayload(field, values, payload, outError)) {
        return false;
    }
    return UpdateIssueFields(issueId, payload, outError);
}

std::string PlaneClient::ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                             const std::string& value) const {
    std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
    if (fieldId == "status") {
        auto sIt =
            std::find_if(cachedStates_.begin(), cachedStates_.end(), [&](const auto& s) { return s.Id == value; });
        if (sIt != cachedStates_.end())
            return sIt->Name;
    }

    if (!field) {
        return value;
    }
    if (field->Id == "sprint") {
        auto cIt =
            std::find_if(cachedCycles_.begin(), cachedCycles_.end(), [&](const auto& c) { return c.Id == value; });
        if (cIt != cachedCycles_.end())
            return cIt->Name;
    }
    if (field->Id == "issuetype") {
        auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                  [&](const auto& opt) { return opt.Id == value; });
        if (optIt != field->AllowedValueOptions.end())
            return optIt->Value;
    }
    if (field->Id == "assignee" || field->IsUserType) {
        auto uIt =
            std::find_if(cachedUsers_.begin(), cachedUsers_.end(), [&](const auto& u) { return u.AccountId == value; });
        if (uIt != cachedUsers_.end())
            return uIt->DisplayName;
        // Fallback to searching AllowedValueOptions if not in cachedUsers_
        auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                  [&](const auto& opt) { return opt.Id == value; });
        if (optIt != field->AllowedValueOptions.end())
            return optIt->Value;
        LOG_DEBUG("PlaneClient: Failed to resolve user UUID '%s' for field '%s' (cache size: %zu)", value.c_str(),
                  field->Id.c_str(), cachedUsers_.size());
    }
    if (field->Id == "labels") {
        // Multi-select labels might be comma-separated or single UUID
        if (value.find(',') != std::string::npos) {
            std::vector<std::string> parts = SplitAndTrim(value);
            std::string resolved;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0)
                    resolved += ", ";
                auto lIt = std::find_if(cachedLabels_.begin(), cachedLabels_.end(),
                                        [&](const auto& l) { return l.Id == parts[i]; });
                if (lIt != cachedLabels_.end()) {
                    resolved += lIt->Name;
                } else {
                    auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                              [&](const auto& opt) { return opt.Id == parts[i]; });
                    if (optIt != field->AllowedValueOptions.end()) {
                        resolved += optIt->Value;
                    } else {
                        resolved += parts[i];
                    }
                }
            }
            return resolved;
        } else {
            auto lIt =
                std::find_if(cachedLabels_.begin(), cachedLabels_.end(), [&](const auto& l) { return l.Id == value; });
            if (lIt != cachedLabels_.end())
                return lIt->Name;

            auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                      [&](const auto& opt) { return opt.Id == value; });
            if (optIt != field->AllowedValueOptions.end())
                return optIt->Value;
        }
    }

    return value;
}

std::string PlaneClient::CreateIssue(const nlohmann::json& fields, std::string& outError) {
    // Resolve config + headers + project under the cache lock, then drop the lock before the HTTP
    // POST so UI display-name lookups are not blocked during the round trip. Re-acquire briefly at
    // the end to record `visualKey -> uuid` in `keyToId_`.
    //
    // Backlog #12: the cfg snapshot and the cache read must happen under the same lock.
    // Snapshotting cfg before the lock would let a concurrent ConfigManager::Save() rotate
    // credentials or workspace identifiers between the snapshot and the cache lookup — the
    // request would then go out with stale auth while the cache was inspected with fresh state.
    std::string planeApi;
    cpr::Header headers;
    std::string workspaceSlug;
    std::string resolvedProjectId;
    std::string projectIdentifier;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        TrackerConfig cfg = ConfigManager::Load();
        // PR 6: legacy cfg.PlaneProjectId removed (see remove-global-project-key.md §2.5).
        // Resolve from the active view's query — same pattern as UpdateIssueFields above.
        const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
        planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
        for (const auto& kv : BuildPlaneHeaders(cfg)) {
            headers.insert({kv.first, kv.second});
        }
        workspaceSlug = cfg.PlaneWorkspaceSlug;
        if (planeProjectId_.empty()) {
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_,
                                &outError);
        }
        if (planeProjectId_.empty()) {
            return "";
        }
        resolvedProjectId = planeProjectId_;
        projectIdentifier = planeProjectIdentifier_;
    }

    const std::string url =
        planeApi + "/api/v1/workspaces/" + workspaceSlug + "/projects/" + resolvedProjectId + "/work-items/";

    auto response = TrackerPostLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "POST", url, response);

    if (response.status_code != 200 && response.status_code != 201) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(response.text);
            // Plane often returns {"error": "..."} or {"detail": "..."} or a dict of field errors
            if (j.is_object()) {
                if (j.contains("error"))
                    detail = j["error"].dump();
                else if (j.contains("detail"))
                    detail = j["detail"].dump();
                else
                    detail = j.dump(); // Show the full object if it's field-level errors
            } else {
                detail = response.text;
            }
        } catch (...) {
            detail = response.text;
        }
        outError = "Plane API error: " + std::to_string(response.status_code) + " " + detail;
        return "";
    }

    try {
        auto j = nlohmann::json::parse(response.text);
        const std::string uuid = JsonFieldToString(j, "id");
        const std::string seqId = JsonFieldToString(j, "sequence_id");

        std::string visualKey;
        if (!projectIdentifier.empty() && !seqId.empty()) {
            visualKey = projectIdentifier + "-" + seqId;
        } else if (!seqId.empty()) {
            visualKey = "#" + seqId;
        } else {
            visualKey = uuid;
        }

        if (!uuid.empty() && !visualKey.empty()) {
            std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
            keyToId_[visualKey] = uuid;
        }

        return visualKey;
    } catch (...) {
    }

    return "";
}

bool PlaneClient::AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/,
                                     std::vector<std::pair<std::string, std::string>>& /*outFailures*/,
                                     std::string& outError) {
    outError = "AttachFilesToIssue not implemented for Plane";
    return false;
}

bool PlaneClient::AddIssueToSprint(const std::string& issueKey, const std::string& sprintId, std::string& outError) {
    nlohmann::json payload;
    payload["cycle"] = sprintId;
    return UpdateIssueFields(issueKey, payload, outError);
}

bool PlaneClient::BuildCreatePayload(const IssueDraft& draft, const std::vector<TrackerField>& /*catalog*/,
                                     nlohmann::json& outPayload, std::string& outError) {
    outPayload = nlohmann::json::object();
    outError.clear();

    outPayload["name"] = draft.FieldValues.count("summary") ? draft.FieldValues.at("summary") : "";

    if (draft.FieldValues.count("description")) {
        const std::string& desc = draft.FieldValues.at("description");
        if (!desc.empty()) {
            // New-issue draft also goes through the modal-style Markdown surface; convert to the
            // HTML subset Plane accepts. See RICH_TEXT_EDITING_V2_PLAN.md.
            outPayload["description_html"] = MarkdownConvert::MarkdownToHtml(desc);
        }
    }

    outPayload["priority"] = draft.FieldValues.count("priority") ? draft.FieldValues.at("priority") : "medium";

    if (draft.FieldValues.count("status")) {
        outPayload["state"] = draft.FieldValues.at("status");
    }

    if (!draft.IssueTypeId.empty()) {
        outPayload["type"] = draft.IssueTypeId;
    } else if (!draft.IssueTypeName.empty()) {
        outPayload["type"] = draft.IssueTypeName; // Plane usually uses UUID for type
    }

    if (!draft.ParentKey.empty()) {
        outPayload["parent"] = draft.ParentKey;
    }

    if (draft.FieldValues.count("assignee")) {
        outPayload["assignee"] = draft.FieldValues.at("assignee");
    }

    return true;
}

bool PlaneClient::BuildUpdatePayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                                     nlohmann::json& outPayload, std::string& outError) {
    // For Plane, update payload is the same as create payload (subset of fields)
    return BuildCreatePayload(draft, catalog, outPayload, outError);
}

bool PlaneClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                     const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                                     std::string& outError) {
    // Basic implementation: fetch all issues and filter.
    // Ideally we'd use a Plane filter, but for now let's just use the main FetchIssues and filter in memory.
    std::string fetchErr;
    auto all = FetchIssues(nullptr, &cfg, nullptr, &fetchErr);
    if (!fetchErr.empty()) {
        outError = fetchErr;
        return false;
    }

    std::unordered_set<std::string> keys(issueKeys.begin(), issueKeys.end());
    for (auto& t : all) {
        if (keys.count(t.id) || keys.count(t.GetFieldValue("key"))) {
            outTickets.push_back(std::move(t));
        }
    }
    return true;
}

namespace {

// Pull `project_id` from a Plane structured-query JSON blob. "" sentinel covers
// both "no project_id key" and "malformed JSON" — callers must fall back.
std::string ExtractProjectFromPlaneQuery(const std::string& planeQueryJson) {
    if (planeQueryJson.empty()) {
        return "";
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(planeQueryJson);
        if (j.is_object()) {
            auto it = j.find("project_id");
            if (it != j.end() && it->is_string()) {
                return it->get<std::string>();
            }
            auto it2 = j.find("project");
            if (it2 != j.end() && it2->is_string()) {
                return it2->get<std::string>();
            }
        }
    } catch (const std::exception&) {
        return "";
    } catch (...) {
        return "";
    }
    return "";
}

} // namespace

std::string PlaneClient::ExtractProjectFromQuery(const std::string& query) const {
    return ExtractProjectFromPlaneQuery(query);
}

namespace {
constexpr std::int64_t kPlaneListProjectsTtlSeconds = 300; // 5 minutes

std::int64_t PlaneNowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

void PlaneClient::InvalidateListProjectsCache() {
    std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
    cachedProjects_.clear();
    cachedProjectsAtUnix_ = 0;
}

std::vector<RemoteProject> PlaneClient::ListProjects() {
    // Fast path: serve from cache when still warm.
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        const std::int64_t now = PlaneNowUnixSeconds();
        if (!cachedProjects_.empty() && (now - cachedProjectsAtUnix_) < kPlaneListProjectsTtlSeconds) {
            return cachedProjects_;
        }
    }

    const TrackerConfig cfg = ConfigManager::Load();
    if (cfg.PlaneWorkspaceSlug.empty()) {
        LOG_WARN("PlaneClient::ListProjects: PlaneWorkspaceSlug is empty.");
        return {};
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers[kv.first] = kv.second;
    }

    const std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/";
    cpr::Parameters params;
    params.Add({"per_page", "100"});

    const cpr::Response resp = TrackerGetLogged("PlaneClient", url, headers, params);
    if (resp.status_code != 200) {
        LOG_WARN("PlaneClient::ListProjects: HTTP %ld on %s", resp.status_code, url.c_str());
        return {};
    }

    std::vector<RemoteProject> projects;
    try {
        const nlohmann::json j = nlohmann::json::parse(StripUtf8BomCopy(resp.text), nullptr, false);
        if (j.is_discarded()) {
            LOG_WARN("PlaneClient::ListProjects: invalid JSON in response.");
            return {};
        }
        const auto& arr = (j.is_object() && j.contains("results")) ? j["results"] : j;
        if (!arr.is_array()) {
            LOG_WARN("PlaneClient::ListProjects: response has no results array.");
            return {};
        }
        projects.reserve(arr.size());
        for (const auto& p : arr) {
            if (!p.is_object()) {
                continue;
            }
            RemoteProject rp;
            rp.id = JsonFieldToString(p, "id");
            rp.key = JsonFieldToString(p, "identifier"); // may be empty
            rp.displayName = JsonFieldToString(p, "name");
            if (rp.id.empty()) {
                continue;
            }
            projects.push_back(std::move(rp));
        }
    } catch (const std::exception& ex) {
        LOG_WARN("PlaneClient::ListProjects: parse error: %s", ex.what());
        return {};
    } catch (...) {
        LOG_WARN("PlaneClient::ListProjects: parse error (unknown)");
        return {};
    }

    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        cachedProjects_ = projects;
        cachedProjectsAtUnix_ = PlaneNowUnixSeconds();
    }
    return projects;
}
