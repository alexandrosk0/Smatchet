#include "PlaneClient.h"
#include "PlaneClient_Internal.h"

#include "Logger.h"
#include "StringUtil.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cpr/cpr.h>
#include <iterator>
#include <string>
#include <unordered_map>

using smatchet::plane_detail::JsonFieldToString;
using smatchet::plane_detail::NormalizePlaneApiBase;
using smatchet::plane_detail::ParseHttpAuthority;
using smatchet::plane_detail::ResolvePlaneProject;
using smatchet::plane_detail::TrimAsciiWs;

namespace {

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
    // Plane v1 has no per-issue capability endpoint; report every built-in field the mutation
    // paths (`BuildCreatePayload`, `BuildUpdatePayload`, `AddIssueToSprint`) can serialize as
    // editable. Server still gets the final say — a rejected update surfaces through the same
    // error path as any other mutation failure. Custom-property (UUID) editability is deferred
    // until C4 lands `properties.<uuid>` serialization; reporting it editable here would only
    // surface a UI affordance that the payload builder silently drops.
    for (const char* fieldId :
         {"summary", "description", "priority", "status", "assignee", "labels", "sprint", "type", "parent"}) {
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
