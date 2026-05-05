#include "PlaneClient.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "StringUtil.h"
#include "IssueDraft.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cpr/cpr.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
std::string StripUtf8BomCopy(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEFu && static_cast<unsigned char>(s[1]) == 0xBBu &&
        static_cast<unsigned char>(s[2]) == 0xBFu) {
        s.erase(0, 3);
    }
    return s;
}

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

std::unordered_map<std::string, std::string> PlaneClient::BuildPlaneHeaders(const TrackerConfig& cfg) const {
    return {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"x-api-key", cfg.PlaneApiKey}
    };
}

namespace {
/// Safely extract a JSON field as a string regardless of its actual type (int, bool, null, etc.).
std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) return {};
    const auto& v = obj[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump(); // fallback: JSON serialization
}

std::string PlaneWorkItemTitleForDisplay(const nlohmann::json& issue) {
    std::string title = JsonFieldToString(issue, "name");
    TrimAsciiWs(title);
    return title;
}

bool LooksLikeUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

bool ResolvePlaneProject(const std::string& planeApi, const TrackerConfig& cfg, const cpr::Header& headers,
                         std::string& outId, std::string& outIdentifier, std::string* outError) {
    const std::string projectsUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/";
    cpr::Parameters params;
    params.Add({"per_page", "100"});
    auto response = TrackerGetLogged("PlaneClient", projectsUrl, headers, params);
    if (response.status_code != 200) {
        const std::string err = "Plane API error " + std::to_string(response.status_code) +
                                " resolving project '" + cfg.PlaneProjectId + "': " +
                                response.text.substr(0, 300);
        if (outError) *outError = err;
        return false;
    }

    const nlohmann::json j = nlohmann::json::parse(StripUtf8BomCopy(response.text), nullptr, false);
    if (j.is_discarded()) {
        const std::string err = "Plane project list returned invalid JSON while resolving project '" +
                                cfg.PlaneProjectId + "'.";
        if (outError) *outError = err;
        return false;
    }

    const auto projects = (j.is_object() && j.contains("results")) ? j["results"] : j;
    if (!projects.is_array()) {
        const std::string err = "Plane project list response has no results array while resolving project '" +
                                cfg.PlaneProjectId + "'.";
        if (outError) *outError = err;
        return false;
    }

    std::string available;
    for (const auto& p : projects) {
        const std::string id = JsonFieldToString(p, "id");
        const std::string identifier = JsonFieldToString(p, "identifier");
        const std::string name = JsonFieldToString(p, "name");
        if (id == cfg.PlaneProjectId || identifier == cfg.PlaneProjectId || name == cfg.PlaneProjectId) {
            outId = id;
            outIdentifier = identifier;
            return true;
        }
        if (available.size() < 220) {
            if (!available.empty()) available += ", ";
            available += identifier.empty() ? name : identifier;
        }
    }

    const std::string err = "Plane project '" + cfg.PlaneProjectId +
                            "' was not found in workspace '" + cfg.PlaneWorkspaceSlug +
                            "'. Available project identifiers/names: " + available;
    if (outError) *outError = err;
    return false;
}

std::string ToUpperAscii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
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
                if (!outWarn->empty()) *outWarn += "; ";
                *outWarn += "HTTP " + std::to_string(response.status_code) + " on " + listUrl.substr(0, 80);
            }
            return;
        }
        const nlohmann::json j = nlohmann::json::parse(StripUtf8BomCopy(response.text), nullptr, false);
        if (j.is_discarded()) {
            if (outWarn && outWarn->size() < 400) {
                if (!outWarn->empty()) *outWarn += "; ";
                *outWarn += "Invalid JSON from " + listUrl.substr(0, 80);
            }
            return;
        }
        auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
        if (!results.is_array()) {
            return;
        }
        for (const auto& row : results) {
            if (row.is_object()) {
                outRows.push_back(row);
            }
        }
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
    if (prop.contains("is_required") && prop["is_required"].is_boolean() && prop["is_required"].get<bool>()) {
        (void)0;
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
    } else if (pt == "URL" || pt == "LINK") {
        out.Type = "string";
        out.Family = TrackerFieldFamily::Text;
    } else if (pt == "RICH_TEXT" || pt == "HTML" || pt == "TEXT" || pt.empty()) {
        out.Type = "string";
        out.Family = TrackerFieldFamily::Text;
    } else {
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

std::vector<CachedTicket> PlaneClient::FetchIssues(bool* outFullSyncCompleted,
                                                   const TrackerConfig* configOverride,
                                                   const ViewsStore* /*viewsOverride*/,
                                                   std::string* outFetchError) {
    if (outFullSyncCompleted) *outFullSyncCompleted = false;

    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();

    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || cfg.PlaneProjectId.empty()) {
        if (outFetchError) *outFetchError = "Plane is not configured. Set URL, Workspace Slug, and Project ID in Preferences.";
        return {};
    }
    if (cfg.PlaneApiKey.empty()) {
        if (outFetchError) *outFetchError = "Plane API key is missing. Set it in Preferences → Tracker.";
        return {};
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    if (planeApi != cfg.PlaneUrl) {
        LOG_INFO("PlaneClient::FetchIssues: REST base %s (configured URL was web app host; use https://api.plane.so "
                 "in preferences to skip this rewrite).",
                 planeApi.c_str());
    }

    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string resolveError;
    std::string prevProjectId = planeProjectId_;
    if (!ResolvePlaneProject(planeApi, cfg, headers, planeProjectId_, planeProjectIdentifier_, &resolveError)) {
        if (outFetchError) *outFetchError = resolveError;
        return {};
    }
    if (planeProjectId_ != prevProjectId) {
        keyToId_.clear();
    }
    const std::string planeProjectId = planeProjectId_;

    // Paginated issue fetch (+ states): outer try; JSON via parse(..., allow_exceptions=false) to avoid
    // uncaught parse_error on some MinGW/std::async stacks when typed catches fail across TU boundaries.
    std::vector<CachedTicket> issues;
    try {
        // Fetch states for display value mapping
        {
            const std::string statesUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug +
                                          "/projects/" + planeProjectId + "/states/";
            auto r = TrackerGetLogged("PlaneClient", statesUrl, headers);
            if (r.status_code == 200) {
                const std::string statesBody = StripUtf8BomCopy(r.text);
                nlohmann::json j = nlohmann::json::parse(statesBody, nullptr, false);
                if (!j.is_discarded()) {
                    cachedStates_.clear();
                    auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
                    if (results.is_array()) {
                        for (auto& s : results) {
                            CachedState cs;
                            cs.Id = JsonFieldToString(s, "id");
                            cs.Name = JsonFieldToString(s, "name");
                            if (!cs.Id.empty()) cachedStates_.push_back(cs);
                        }
                    }
                }
            }
        }

    // Plane API v1 lists work items at .../work-items/ with cursor pagination (not .../issues/).
    const int pageSize = 100;
    std::string listCursor;
    while (true) {
        const std::string listBase =
            planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId +
            "/work-items/";
        cpr::Parameters params;
        params.Add({"per_page", std::to_string(pageSize)});
        // Plane expects "labels" not "label" in expand; bad values can yield 404 on some deployments.
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
            std::string err = "Plane API error " + std::to_string(response.status_code) + " fetching issues (URL: " +
                               urlHint + "): " + response.text.substr(0, 300);
            if (!apiDetail.empty()) {
                err += " [detail: " + apiDetail + "]";
            }
            if (response.status_code == 404) {
                err += " Check Workspace Slug and Project ID (UUID from app URL or Plane settings), and API key "
                       "scopes.";
            }
            LOG_ERROR("PlaneClient::FetchIssues %s", err.c_str());
            if (outFetchError) *outFetchError = err;
            return issues; // Return partial results if any
        }

        const std::string bodyForJson = StripUtf8BomCopy(response.text);
        const bool looksHtml = LooksHtmlPrefix(bodyForJson);

        if (bodyForJson.empty()) {
            const std::string err = "Plane returned empty response body (HTTP 200) when fetching issues.";
            LOG_ERROR("PlaneClient::FetchIssues %s", err.c_str());
            if (outFetchError) *outFetchError = err;
            return issues;
        }
        if (looksHtml) {
            const std::string urlHint = SanitizeAsciiSnippet(listBase, 220);
            const std::string err =
                "Plane returned HTML instead of JSON (HTTP 200). Request URL: " + urlHint +
                ". For Plane Cloud set base URL to https://api.plane.so (origin only, no /workspace path). "
                "Self-hosted: use the API origin your reverse proxy serves for /api/v1/.";
            LOG_ERROR("PlaneClient::FetchIssues %s", err.c_str());
            if (outFetchError) *outFetchError = err;
            return issues;
        }

        // Non-throwing parse (nlohmann 3.11: failures yield value_t::discarded when allow_exceptions=false).
        nlohmann::json j = nlohmann::json::parse(bodyForJson, nullptr, false);
        if (j.is_discarded()) {
            const std::string err =
                "Plane returned invalid JSON when fetching issues (HTTP 200). Verify Plane URL, workspace slug, "
                "project UUID, and API key.";
            LOG_ERROR("PlaneClient::FetchIssues %s", err.c_str());
            if (outFetchError) *outFetchError = err;
            return issues;
        }

        auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
        if (!results.is_array()) {
            std::string keyList;
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end() && keyList.size() < 240; ++it) {
                    if (!keyList.empty()) keyList += ',';
                    keyList += it.key();
                }
            }
            const std::string err =
                "Plane list response has no results array (wrong endpoint or API version). Top-level keys: " +
                keyList;
            LOG_ERROR("PlaneClient::FetchIssues %s", err.c_str());
            if (outFetchError) *outFetchError = err;
            return issues;
        }
        if (results.empty()) {
            break;
        }

        for (const auto& issue : results) {
            try {
                CachedTicket ticket;
                const std::string uuid = JsonFieldToString(issue, "id");
                const std::string seqId = JsonFieldToString(issue, "sequence_id");

                std::string visualKey;
                if (!planeProjectIdentifier_.empty() && !seqId.empty()) {
                    visualKey = planeProjectIdentifier_ + "-" + seqId;
                } else if (!seqId.empty()) {
                    visualKey = "#" + seqId;
                } else {
                    visualKey = uuid;
                }

                ticket.id = visualKey;
                ticket.fieldValues["uuid"] = uuid;
                ticket.fieldValues["key"] = visualKey;
                keyToId_[visualKey] = uuid;
                ticket.fieldValues["summary"] = PlaneWorkItemTitleForDisplay(issue);

                // Status / state
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
                if (issue.contains("assignee_details") && issue["assignee_details"].is_array() && !issue["assignee_details"].empty()) {
                    const auto& first = issue["assignee_details"][0];
                    if (first.is_object() && first.contains("display_name")) {
                        assigneeName = JsonFieldToString(first, "display_name");
                    }
                }
                
                if (assigneeName == assigneeId && !assigneeId.empty()) {
                    for (const auto& u : cachedUsers_) {
                        if (u.AccountId == assigneeId) {
                            assigneeName = u.DisplayName;
                            break;
                        }
                    }
                }
                
                ticket.fieldValues["assignee"] = assigneeName;

                ticket.fieldValues["priority"] = JsonFieldToString(issue, "priority");

                // Cycle / Sprint
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
                        if (!labelStr.empty()) labelStr += ", ";
                        std::string ln = JsonFieldToString(lbl, "name");
                        if (ln.empty()) ln = JsonFieldToString(lbl, "id");
                        labelStr += ln;
                    }
                } else if (issue.contains("labels") && issue["labels"].is_array()) {
                    for (const auto& lbl : issue["labels"]) {
                        if (!labelStr.empty()) labelStr += ", ";
                        if (lbl.is_object()) {
                            std::string ln = JsonFieldToString(lbl, "name");
                            if (ln.empty()) ln = JsonFieldToString(lbl, "id");
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
                
                // Issue Type / Work Item Type
                if (issue.contains("type_detail") && issue["type_detail"].is_object()) {
                    ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type_detail"], "name");
                } else if (issue.contains("type") && issue["type"].is_object()) {
                    ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type"], "name");
                } else {
                    ticket.fieldValues["issuetype"] = JsonFieldToString(issue, "type");
                }

                if (!ticket.id.empty()) {
                    issues.push_back(std::move(ticket));
                }
            } catch (const std::exception& ex) {
                LOG_WARN("PlaneClient::FetchIssues: skipping issue parse error: %s", ex.what());
            } catch (...) {
                LOG_WARN("PlaneClient::FetchIssues: skipping issue — unexpected exception during field mapping.");
            }
        }

        // Cursor pagination (see Plane list work-items docs)
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
            break;
        }
    }
    if (outFullSyncCompleted) *outFullSyncCompleted = true;
    LOG_INFO("PlaneClient::FetchIssues fetched %zu issues from Plane.", issues.size());
    return issues;
    } catch (const nlohmann::json::exception& jex) {
        LOG_ERROR("PlaneClient::FetchIssues outer json error: %s", jex.what());
        if (outFetchError && outFetchError->empty()) {
            *outFetchError = std::string("Plane sync failed (JSON): ") + jex.what();
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("PlaneClient::FetchIssues outer error: %s", ex.what());
        if (outFetchError && outFetchError->empty()) {
            *outFetchError = std::string("Plane sync failed: ") + ex.what();
        }
    } catch (...) {
        LOG_ERROR("PlaneClient::FetchIssues outer error: unknown exception");
        if (outFetchError && outFetchError->empty()) {
            *outFetchError = "Plane sync failed: unknown exception";
        }
    }
    return issues;
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

    auto resp = cpr::Get(cpr::Url{url}, headers, cpr::Timeout{kTrackerProbeOverallTimeoutMs});
    const long sc = resp.status_code;

    if (sc == 200) {
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "HTTP 200";
    } else if (sc == 401 || sc == 403) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(sc) + " (Auth Error)";
    } else if (sc >= 500) {
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        out.Diagnostic = "HTTP " + std::to_string(sc) + " (Server Error)";
    } else {
        out.Kind = TrackerReachabilityProbeKind::TransportDown;
        out.Diagnostic = resp.error.message.empty() ? ("HTTP " + std::to_string(sc)) : resp.error.message;
    }
    return out;
}

bool PlaneClient::FetchFieldCatalog(const TrackerConfig& cfg, TrackerFieldCatalogResult& outCatalog,
                                    std::string& outError) {
    outCatalog = TrackerFieldCatalogResult{};
    outError.clear();
    std::vector<std::string> warns;

    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || cfg.PlaneProjectId.empty()) {
        outError = "Plane is not configured. Set URL, Workspace Slug, and Project ID in Preferences.";
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
    if (!ResolvePlaneProject(planeApi, cfg, headers, planeProjectId_, planeProjectIdentifier_, &outError)) {
        return false;
    }
    const std::string planeProjectId = planeProjectId_;

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

    cachedStates_.clear();
    const std::string statesUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                  planeProjectId + "/states/";
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
        cachedStates_.push_back(cs);
        TrackerFieldOption opt;
        opt.Id = cs.Id;
        opt.Value = cs.Name.empty() ? cs.Id : cs.Name;
        statusField.AllowedValueOptions.push_back(std::move(opt));
    }
    if (cachedStates_.empty() && !stateWarn.empty()) {
        warns.push_back(std::string("states: ") + stateWarn);
    } else if (cachedStates_.empty()) {
        warns.push_back("No Plane states returned (empty list or unparsed response).");
    }
    fields.push_back(std::move(statusField));

    fields.push_back(makeCore("assignee", "Assignee", "user", TrackerFieldFamily::UserSingle, false));

    cachedCycles_.clear();
    TrackerField sprintField = makeCore("sprint", "Cycle", "string", TrackerFieldFamily::Sprint, false);
    const std::string cyclesUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                   planeProjectId + "/cycles/";
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
        cachedCycles_.push_back(cc);
        TrackerFieldOption opt;
        opt.Id = cc.Id;
        opt.Value = cc.Name.empty() ? cc.Id : cc.Name;
        sprintField.AllowedValueOptions.push_back(std::move(opt));
    }
    if (cachedCycles_.empty() && !cycleWarn.empty()) {
        warns.push_back(std::string("cycles: ") + cycleWarn);
    }
    fields.push_back(std::move(sprintField));
    
    cachedUsers_.clear();
    const std::string membersUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                    planeProjectId + "/members/";
    std::vector<nlohmann::json> memberRows;
    std::string memberWarn;
    AppendPagedResults(membersUrl, headers, memberRows, &memberWarn);
    for (const auto& m : memberRows) {
        if (!m.is_object()) continue;

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
            if (!fn.empty() || !ln.empty()) tu.DisplayName = fn + (fn.empty() || ln.empty() ? "" : " ") + ln;
        }
        if (tu.DisplayName.empty() || tu.DisplayName == " ") {
            tu.DisplayName = JsonFieldToString(u, "email");
        }
        tu.EmailAddress = JsonFieldToString(u, "email");
        
        if (tu.AccountId.empty()) {
             tu.AccountId = JsonFieldToString(m, "id");
        }

        if (tu.AccountId.empty()) continue;
        if (tu.DisplayName.empty()) tu.DisplayName = tu.AccountId; // Fallback to ID so it's not empty
        
        cachedUsers_.push_back(tu);
        outCatalog.Users.push_back(tu);
    }
    if (cachedUsers_.empty() && !memberWarn.empty()) {
        warns.push_back(std::string("members: ") + memberWarn);
    }
    LOG_INFO("PlaneClient: Fetched %zu project members.", cachedUsers_.size());

    cachedLabels_.clear();
    const std::string labelsUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                   planeProjectId + "/labels/";
    std::string labelsWarn;
    std::vector<nlohmann::json> labelRows;
    AppendPagedResults(labelsUrl, headers, labelRows, &labelsWarn);
    
    TrackerField labelsField = makeCore("labels", "Labels", "array", TrackerFieldFamily::Labels, false);
    for (const auto& l : labelRows) {
        if (!l.is_object()) continue;
        CachedLabel cl;
        cl.Id = JsonFieldToString(l, "id");
        cl.Name = JsonFieldToString(l, "name");
        if (cl.Id.empty()) continue;
        cachedLabels_.push_back(cl);
        
        TrackerFieldOption opt;
        opt.Id = cl.Id;
        opt.Value = cl.Name.empty() ? cl.Id : cl.Name;
        labelsField.AllowedValueOptions.push_back(std::move(opt));
    }
    if (cachedLabels_.empty() && !labelsWarn.empty()) {
        warns.push_back(std::string("labels: ") + labelsWarn);
    }
    LOG_INFO("PlaneClient: Fetched %zu project labels.", cachedLabels_.size());
    fields.push_back(std::move(labelsField));




    fields.push_back(makeCore("created", "Created", "datetime", TrackerFieldFamily::DateTime, true));
    fields.push_back(makeCore("updated", "Updated", "datetime", TrackerFieldFamily::DateTime, true));


    const std::string typesUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                 planeProjectId + "/work-item-types/";
    std::vector<nlohmann::json> typeRows;
    std::string typeWarn;
    AppendPagedResults(typesUrl, headers, typeRows, &typeWarn);
    
    if (typeRows.empty()) {
        LOG_WARN("PlaneClient::FetchFieldCatalog: No work-item-types found for project %s. URL: %s. Error: %s",
                 planeProjectId.c_str(), typesUrl.c_str(), typeWarn.c_str());
        if (!typeWarn.empty()) warns.push_back("work-item-types: " + typeWarn);
    }

    std::unordered_map<std::string, TrackerField> customs;

    for (const auto& tentry : typeRows) {
        if (!tentry.is_object()) {
            continue;
        }
        if (!tentry.value("deleted_at", nlohmann::json()).is_null()) {
            continue;
        }
        if (tentry.contains("is_active") && tentry["is_active"].is_boolean() &&
            !tentry["is_active"].get<bool>()) {
            continue;
        }
        const std::string typeId = JsonFieldToString(tentry, "id");
        const std::string typeName = JsonFieldToString(tentry, "name");
        if (typeId.empty()) {
            continue;
        }



        const std::string propsUrl =
            planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId +
            "/work-item-types/" + typeId + "/work-item-properties/";
        auto pResp = TrackerGetLogged("PlaneClient", propsUrl, headers);
        if (pResp.status_code != 200) {
            warns.push_back("work-item-properties HTTP " + std::to_string(pResp.status_code) +
                            " for type " + typeId.substr(0, 8));
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
    for (auto& kv : customs) {
        fields.push_back(std::move(kv.second));
    }


    // Populate user fields with options for the dropdowns
    if (!cachedUsers_.empty()) {
        for (auto& field : fields) {
            if (!field.IsUserType) {
                continue;
            }
            field.AllowedValueOptions.clear();
            field.AllowedValueOptions.reserve(cachedUsers_.size());
            for (const auto& user : cachedUsers_) {
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
    return true;
}

bool PlaneClient::FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/,
                                     std::unordered_map<std::string, bool>& outFieldIdCanEdit,
                                     std::string& outError) {
    outError.clear();
    outFieldIdCanEdit.clear();
    for (const char* fieldId : {"summary", "description", "priority", "status", "assignee", "labels", "sprint"}) {
        outFieldIdCanEdit[fieldId] = true;
    }
    return true;
}

std::string PlaneClient::BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const {
    // User wants: https://app.plane.so/<workspace-slug>/browse/<issue-key>/
    const std::string webBase = NormalizePlaneWebBase(cfg.PlaneUrl);
    return webBase + "/" + cfg.PlaneWorkspaceSlug + "/browse/" + issueKey + "/";
}

bool PlaneClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) {
    TrackerConfig cfg = ConfigManager::Load();
    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    if (planeProjectId_.empty()) {
        std::string dummyId, dummyIdent;
        ResolvePlaneProject(planeApi, cfg, headers, planeProjectId_, planeProjectIdentifier_, &outError);
    }
    if (planeProjectId_.empty()) {
        return false;
    }

    std::string targetUuid = issueId;
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

    std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug +
                      "/projects/" + planeProjectId_ + "/work-items/" + targetUuid + "/";
    
    auto response = TrackerPatchLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "PATCH", url, response);

    if (response.status_code != 200 && response.status_code != 204) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(response.text);
            if (j.is_object() && j.contains("error")) detail = j["error"].dump();
            else if (j.is_object() && j.contains("detail")) detail = j["detail"].dump();
            else detail = response.text;
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
    if (field.Id == "summary") {
        outPayload["name"] = values.empty() ? "" : values[0];
    } else if (field.Id == "description") {
        outPayload["description_html"] = values.empty() ? "" : values[0];
    } else if (field.Id == "priority") {
        outPayload["priority"] = values.empty() ? "medium" : values[0];
    } else if (field.Id == "status") {
        outPayload["state"] = values.empty() ? "" : values[0];
    } else if (field.Id == "assignee") {
        outPayload["assignee"] = values.empty() ? nullptr : nlohmann::json(values[0]);
    } else if (field.Id == "labels") {
        std::vector<std::string> labelIds;
        for (const auto& val : values) {
            std::string id = val;
            for (const auto& l : cachedLabels_) {
                if (l.Name == val) {
                    id = l.Id;
                    break;
                }
            }
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
    if (fieldId == "status") {
        for (const auto& s : cachedStates_) {
            if (s.Id == value) return s.Name;
        }
    }

    if (!field) {
        return value;
    }
    if (field->Id == "sprint") {
        for (const auto& c : cachedCycles_) {
            if (c.Id == value) return c.Name;
        }
    }
    if (field->Id == "issuetype") {
        for (const auto& opt : field->AllowedValueOptions) {
            if (opt.Id == value) return opt.Value;
        }
    }
    if (field->Id == "assignee" || field->IsUserType) {
        for (const auto& u : cachedUsers_) {
            if (u.AccountId == value) return u.DisplayName;
        }
        // Fallback to searching AllowedValueOptions if not in cachedUsers_
        for (const auto& opt : field->AllowedValueOptions) {
            if (opt.Id == value) return opt.Value;
        }
        LOG_DEBUG("PlaneClient: Failed to resolve user UUID '%s' for field '%s' (cache size: %zu)", value.c_str(), field->Id.c_str(), cachedUsers_.size());
    }
    if (field->Id == "labels") {
        // Multi-select labels might be comma-separated or single UUID
        if (value.find(',') != std::string::npos) {
            std::vector<std::string> parts = SplitAndTrim(value);
            std::string resolved;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) resolved += ", ";
                bool found = false;
                for (const auto& l : cachedLabels_) {
                    if (l.Id == parts[i]) {
                        resolved += l.Name;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // Check AllowedValueOptions too
                    for (const auto& opt : field->AllowedValueOptions) {
                        if (opt.Id == parts[i]) {
                            resolved += opt.Value;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) resolved += parts[i];
            }
            return resolved;
        } else {
            for (const auto& l : cachedLabels_) {
                if (l.Id == value) return l.Name;
            }
            for (const auto& opt : field->AllowedValueOptions) {
                if (opt.Id == value) return opt.Value;
            }
        }
    }

    return value;
}

std::string PlaneClient::CreateIssue(const nlohmann::json& fields, std::string& outError) {
    TrackerConfig cfg = ConfigManager::Load();
    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }
    if (planeProjectId_.empty()) {
        std::string dummyId, dummyIdent;
        ResolvePlaneProject(planeApi, cfg, headers, planeProjectId_, planeProjectIdentifier_, &outError);
    }
    if (planeProjectId_.empty()) {
        return "";
    }
    std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug +
                      "/projects/" + planeProjectId_ + "/work-items/";

    auto response = TrackerPostLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "POST", url, response);

    if (response.status_code != 200 && response.status_code != 201) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(response.text);
            // Plane often returns {"error": "..."} or {"detail": "..."} or a dict of field errors
            if (j.is_object()) {
                if (j.contains("error")) detail = j["error"].dump();
                else if (j.contains("detail")) detail = j["detail"].dump();
                else detail = j.dump(); // Show the full object if it's field-level errors
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
        if (!planeProjectIdentifier_.empty() && !seqId.empty()) {
            visualKey = planeProjectIdentifier_ + "-" + seqId;
        } else if (!seqId.empty()) {
            visualKey = "#" + seqId;
        } else {
            visualKey = uuid;
        }

        if (!uuid.empty() && !visualKey.empty()) {
            keyToId_[visualKey] = uuid;
        }

        return visualKey;
    } catch (...) {}


    return "";
}

bool PlaneClient::AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/,
                                     std::vector<std::pair<std::string, std::string>>& /*outFailures*/,
                                     std::string& outError) {
    outError = "AttachFilesToIssue not implemented for Plane";
    return false;
}

bool PlaneClient::AddIssueToSprint(const std::string& issueKey, const std::string& sprintId,
                                   std::string& outError) {
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
            // Plane API v1 requires valid HTML for description_html.
            // If it doesn't look like HTML, wrap it in a paragraph.
            if (desc.find('<') == std::string::npos) {
                outPayload["description_html"] = "<p>" + desc + "</p>";
            } else {
                outPayload["description_html"] = desc;
            }
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









