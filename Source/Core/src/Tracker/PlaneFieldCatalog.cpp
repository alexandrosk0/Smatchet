#include "PlaneClient.h"
#include "PlaneClient_Internal.h"
#include "PlaneFieldCatalogPure.h"

#include "Json/BoundedJsonParse.h"
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
#include <utility>

using smatchet::plane_detail::JsonFieldToString;
using smatchet::plane_detail::NormalizePlaneApiBase;
using smatchet::plane_detail::ParseHttpAuthority;
using smatchet::plane_detail::ResolvePlaneProject;
using smatchet::plane_detail::TrimAsciiWs;

namespace {

std::string FormatPlaneCatalogResourceWarn(const char* resourceLabel, int statusCode) {
    if (statusCode == 402) {
        return std::string(resourceLabel) + ": not available on current Plane plan (upgrade required)";
    }
    if (statusCode == 403) {
        return std::string(resourceLabel) + ": permission denied (HTTP 403)";
    }
    return std::string(resourceLabel) + ": HTTP " + std::to_string(statusCode);
}

bool IsPlanePlanGatedCatalogStatus(int statusCode) { return statusCode == 402; }

bool IsPlanePlanGatedCatalogWarn(const std::string& warn) {
    return warn.find("not available on current Plane plan") != std::string::npos;
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

void AppendPagedResults(const std::string& listUrl, const cpr::Header& headers, std::vector<nlohmann::json>& outRows,
                        std::string* outWarn, const char* resourceLabel) {
    std::string cursor;
    for (int page = 0; page < 100; ++page) {
        cpr::Parameters params;
        params.Add({"per_page", "100"});
        if (!cursor.empty()) {
            params.Add({"cursor", cursor});
        }
        auto response = TrackerGetLogged("PlaneClient", listUrl, headers, params);
        if (response.status_code != 200) {
            if (outWarn && outWarn->empty()) {
                if (resourceLabel != nullptr && resourceLabel[0] != '\0') {
                    *outWarn = FormatPlaneCatalogResourceWarn(resourceLabel, static_cast<int>(response.status_code));
                } else {
                    *outWarn = "HTTP " + std::to_string(response.status_code);
                }
            }
            return;
        }
        std::string parseErr;
        const nlohmann::json j = smatchet::json_safe::ParseBounded(StripUtf8BomCopy(response.text), parseErr);
        if (!parseErr.empty()) {
            if (outWarn && outWarn->empty()) {
                *outWarn = (resourceLabel != nullptr && resourceLabel[0] != '\0')
                               ? std::string(resourceLabel) + ": invalid JSON response"
                               : std::string("Invalid JSON response");
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

TrackerField MakeCoreField(const char* id, const char* name, const char* type, TrackerFieldFamily fam, bool readOnly) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.Type = type;
    f.Family = fam;
    f.ReadOnly = readOnly;
    f.IsUserType = (fam == TrackerFieldFamily::UserSingle || fam == TrackerFieldFamily::UserMulti);
    return f;
}

// Phase: fetch the `states` list, build the status field's options + the parallel (id, name) cache
// pairs. Byte-for-byte mirror of the original inline states block. Appends a warning when nothing
// parsed. Returns neutral id-plus-name pairs rather than the private nested CachedState struct, so
// this free helper stays out of the class; the orchestrator widens the pairs into the typed cache.
TrackerField FetchPlaneStatesField(const std::string& planeApi, const TrackerConfig& cfg,
                                   const std::string& planeProjectId, const cpr::Header& headers,
                                   std::vector<std::pair<std::string, std::string>>& outStates,
                                   std::vector<std::string>& warns) {
    TrackerField statusField = MakeCoreField("status", "State", "string", TrackerFieldFamily::Status, false);
    const std::string statesUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/states/";
    std::string stateWarn;
    std::vector<nlohmann::json> stateRows;
    AppendPagedResults(statesUrl, headers, stateRows, &stateWarn, "States");
    for (const auto& s : stateRows) {
        if (!s.is_object()) {
            continue;
        }
        const std::string id = smatchet::plane::AppendPlaneRowOption(s, "id", "name", statusField);
        if (id.empty()) {
            continue;
        }
        outStates.emplace_back(id, JsonFieldToString(s, "name"));
    }
    if (outStates.empty() && !stateWarn.empty()) {
        warns.push_back(std::string("states: ") + stateWarn);
    } else if (outStates.empty()) {
        warns.push_back("No Plane states returned (empty list or unparsed response).");
    }
    return statusField;
}

// Phase: fetch the `cycles` list, build the sprint field's options + the parallel (id, name) cache
// pairs (widened to CachedCycle by the orchestrator — see FetchPlaneStatesField for the rationale).
TrackerField FetchPlaneCyclesField(const std::string& planeApi, const TrackerConfig& cfg,
                                   const std::string& planeProjectId, const cpr::Header& headers,
                                   std::vector<std::pair<std::string, std::string>>& outCycles,
                                   std::vector<std::string>& warns) {
    TrackerField sprintField = MakeCoreField("sprint", "Cycle", "string", TrackerFieldFamily::Sprint, false);
    const std::string cyclesUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/cycles/";
    std::string cycleWarn;
    std::vector<nlohmann::json> cycleRows;
    AppendPagedResults(cyclesUrl, headers, cycleRows, &cycleWarn, "Cycles");
    for (const auto& c : cycleRows) {
        if (!c.is_object()) {
            continue;
        }
        const std::string id = smatchet::plane::AppendPlaneRowOption(c, "id", "name", sprintField);
        if (id.empty()) {
            continue;
        }
        outCycles.emplace_back(id, JsonFieldToString(c, "name"));
    }
    if (outCycles.empty() && !cycleWarn.empty()) {
        warns.push_back(std::string("cycles: ") + cycleWarn);
    }
    return sprintField;
}

// Phase: fetch the `members` list into TrackerUsers. Mirrors the original member-unwrap logic
// (member/user nested object, bare-UUID member string, name/email fallbacks) byte-for-byte.
void FetchPlaneMembers(const std::string& planeApi, const TrackerConfig& cfg, const std::string& planeProjectId,
                       const cpr::Header& headers, std::vector<TrackerUser>& outUsers,
                       std::vector<std::string>& warns) {
    const std::string membersUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/members/";
    std::vector<nlohmann::json> memberRows;
    std::string memberWarn;
    AppendPagedResults(membersUrl, headers, memberRows, &memberWarn, "Members");
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

        outUsers.push_back(tu);
    }
    if (outUsers.empty() && !memberWarn.empty()) {
        warns.push_back(std::string("members: ") + memberWarn);
    }
    LOG_INFO("PlaneClient: Fetched %zu project members.", outUsers.size());
}

// Phase: fetch the `labels` list, build the labels field's options + the parallel (id, name) cache
// pairs (widened to CachedLabel by the orchestrator — see FetchPlaneStatesField for the rationale).
TrackerField FetchPlaneLabelsField(const std::string& planeApi, const TrackerConfig& cfg,
                                   const std::string& planeProjectId, const cpr::Header& headers,
                                   std::vector<std::pair<std::string, std::string>>& outLabels,
                                   std::vector<std::string>& warns) {
    const std::string labelsUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/labels/";
    std::string labelsWarn;
    std::vector<nlohmann::json> labelRows;
    AppendPagedResults(labelsUrl, headers, labelRows, &labelsWarn, "Labels");

    TrackerField labelsField = MakeCoreField("labels", "Labels", "array", TrackerFieldFamily::Labels, false);
    for (const auto& l : labelRows) {
        if (!l.is_object())
            continue;
        const std::string id = smatchet::plane::AppendPlaneRowOption(l, "id", "name", labelsField);
        if (id.empty())
            continue;
        outLabels.emplace_back(id, JsonFieldToString(l, "name"));
    }
    if (outLabels.empty() && !labelsWarn.empty()) {
        warns.push_back(std::string("labels: ") + labelsWarn);
    }
    LOG_INFO("PlaneClient: Fetched %zu project labels.", outLabels.size());
    return labelsField;
}

// Phase: walk active work-item-types, fetch each type's properties, and map every property into a
// deduped custom-field map. Mirrors the original inline type/property loop byte-for-byte; property
// mapping routes through the pure MapPlanePropertyToField helper.
void FetchPlaneCustomFields(const std::string& planeApi, const TrackerConfig& cfg, const std::string& planeProjectId,
                            const cpr::Header& headers, std::unordered_map<std::string, TrackerField>& outCustoms,
                            std::vector<std::string>& warns) {
    const std::string typesUrl =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/work-item-types/";
    std::vector<nlohmann::json> typeRows;
    std::string typeWarn;
    AppendPagedResults(typesUrl, headers, typeRows, &typeWarn, "Custom fields");

    const bool customFieldPlanGated = typeRows.empty() && !typeWarn.empty() && IsPlanePlanGatedCatalogWarn(typeWarn);
    if (typeRows.empty() && !typeWarn.empty() && !customFieldPlanGated) {
        warns.push_back(typeWarn);
        LOG_WARN("PlaneClient::FetchFieldCatalog: No work-item-types for project %s. %s", planeProjectId.c_str(),
                 typeWarn.c_str());
    } else if (customFieldPlanGated) {
        LOG_TRACE("PlaneClient::FetchFieldCatalog: custom fields plan-gated (HTTP 402); built-in catalog kept");
    }
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
            if (IsPlanePlanGatedCatalogStatus(static_cast<int>(pResp.status_code))) {
                continue;
            }
            warns.push_back(
                FormatPlaneCatalogResourceWarn("Custom field properties", static_cast<int>(pResp.status_code)) +
                " for type " + typeId.substr(0, 8));
            continue;
        }
        std::string parseErr;
        const nlohmann::json pj = smatchet::json_safe::ParseBounded(StripUtf8BomCopy(pResp.text), parseErr);
        if (!parseErr.empty()) {
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
            if (!smatchet::plane::MapPlanePropertyToField(prop, tf)) {
                continue;
            }
            if (outCustoms.find(tf.Id) != outCustoms.end()) {
                continue;
            }
            outCustoms.emplace(tf.Id, std::move(tf));
        }
    }
}

// Phase: rewrite every user-type field's options to the full member roster (drop the placeholder
// options the field carried). Mirrors the original inline user-field population.
void PopulateUserFieldOptions(std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users) {
    if (users.empty()) {
        return;
    }
    for (auto& field : fields) {
        if (!field.IsUserType) {
            continue;
        }
        field.AllowedValueOptions.clear();
        field.AllowedValueOptions.reserve(users.size());
        for (const auto& user : users) {
            TrackerFieldOption opt;
            opt.Id = user.AccountId;
            opt.Value = user.DisplayName;
            field.AllowedValueOptions.push_back(std::move(opt));
        }
    }
}

// Widen neutral id-and-name pairs into a typed cache vector. Works for any of
// the simple cache structs that carry an Id and a Name member.
template <typename CachedT>
std::vector<CachedT> PairsToCached(std::vector<std::pair<std::string, std::string>>& pairs) {
    std::vector<CachedT> out;
    out.reserve(pairs.size());
    for (auto& p : pairs) {
        CachedT c;
        c.Id = std::move(p.first);
        c.Name = std::move(p.second);
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace

Result<TrackerFieldCatalogResult, TrackerError> PlaneClient::FetchFieldCatalog(const TrackerConfig& cfg,
                                                                               const std::string& projectKeyArg) {
    TrackerFieldCatalogResult outCatalog;
    std::string outError;
    std::vector<std::string> warns;

    // Project is an explicit per-call argument (no global cfg.PlaneProjectId).
    // Empty ≡ unscoped: caller didn't pin a project, so refuse rather than silently fetching
    // a stale catalog. See remove-global-project-key.md §2.5 / §7.
    const std::string projectKey = projectKeyArg;

    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || projectKey.empty()) {
        return Result<TrackerFieldCatalogResult, TrackerError>::Err(TrackerErrorInvalidRequest(
            "Plane is not configured or no project was supplied. "
            "Set URL / Workspace Slug in Preferences, and pick a project before refreshing the field catalog."));
    }
    if (cfg.PlaneApiKey.empty()) {
        return Result<TrackerFieldCatalogResult, TrackerError>::Err(
            TrackerErrorInvalidRequest("Plane API key is missing. Set it in Preferences → Tracker."));
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
        // TODO(#21b later slice): re-thread status from inner helper instead of collapsing to Unknown — IsRetryable()
        // consumers land in a later slice.
        return Result<TrackerFieldCatalogResult, TrackerError>::Err(TrackerErrorUnknown(std::move(outError)));
    }
    const std::string planeProjectId = resolvedProjectId;

    // Built-in fields are emitted in a fixed order; the network-backed states / cycles / members /
    // labels / custom-property phases each map their rows into the matching field + parallel cache.
    std::vector<TrackerField> fields;
    fields.reserve(32);
    fields.push_back(MakeCoreField("summary", "Name", "string", TrackerFieldFamily::Text, false));
    fields.push_back(MakeCoreField("description", "Description", "string", TrackerFieldFamily::Text, false));
    fields.push_back(MakeCoreField("priority", "Priority", "string", TrackerFieldFamily::Text, false));

    // States / cycles / labels phases return neutral (id, name) pairs; widen into the typed nested
    // caches (the private nested Cached structs) here, where those types are in scope.
    std::vector<std::pair<std::string, std::string>> statePairs;
    fields.push_back(FetchPlaneStatesField(planeApi, cfg, planeProjectId, headers, statePairs, warns));

    fields.push_back(MakeCoreField("assignee", "Assignee", "user", TrackerFieldFamily::UserSingle, false));

    std::vector<std::pair<std::string, std::string>> cyclePairs;
    fields.push_back(FetchPlaneCyclesField(planeApi, cfg, planeProjectId, headers, cyclePairs, warns));

    std::vector<TrackerUser> localUsers;
    FetchPlaneMembers(planeApi, cfg, planeProjectId, headers, localUsers, warns);
    outCatalog.Users = localUsers;

    std::vector<std::pair<std::string, std::string>> labelPairs;
    fields.push_back(FetchPlaneLabelsField(planeApi, cfg, planeProjectId, headers, labelPairs, warns));

    fields.push_back(MakeCoreField("created", "Created", "datetime", TrackerFieldFamily::DateTime, true));
    fields.push_back(MakeCoreField("updated", "Updated", "datetime", TrackerFieldFamily::DateTime, true));
    // issue-comments PR-C — read-only comments column. The cell renders a bare
    // comment icon (no count); the value is never populated in the Plane issue
    // mapper, so leaving fieldValues["comments"] empty keeps the row free of any
    // per-row network fetch (Pillar 2). Read-only so the grid offers no edit.
    fields.push_back(MakeCoreField("comments", "Comments", "number", TrackerFieldFamily::Number, true));

    std::unordered_map<std::string, TrackerField> customs;
    FetchPlaneCustomFields(planeApi, cfg, planeProjectId, headers, customs, warns);
    fields.reserve(fields.size() + customs.size());
    std::transform(customs.begin(), customs.end(), std::back_inserter(fields),
                   [](auto& kv) { return std::move(kv.second); });

    // Populate user fields with options for the dropdowns
    PopulateUserFieldOptions(fields, localUsers);

    for (size_t i = 0; i < warns.size(); ++i) {
        if (i != 0) {
            outCatalog.Warning += "; ";
        }
        outCatalog.Warning += warns[i];
    }

    outCatalog.Fields = std::move(fields);

    std::vector<CachedState> localStates = PairsToCached<CachedState>(statePairs);
    std::vector<CachedCycle> localCycles = PairsToCached<CachedCycle>(cyclePairs);
    std::vector<CachedLabel> localLabels = PairsToCached<CachedLabel>(labelPairs);

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

    return Result<TrackerFieldCatalogResult, TrackerError>::Ok(std::move(outCatalog));
}

Result<std::unordered_map<std::string, bool>, TrackerError>
PlaneClient::FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/) {
    std::unordered_map<std::string, bool> outFieldIdCanEdit;
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
    return Result<std::unordered_map<std::string, bool>, TrackerError>::Ok(std::move(outFieldIdCanEdit));
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
