#include "JiraClient.h"

#include "TrackerFieldCatalogPure.h"
#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"

#include "JsonParseUtil.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using TrackerFieldCatalogPure::ExtractComponentOption;
using TrackerFieldCatalogPure::MergeComponentIntoCatalog;
using TrackerFieldCatalogPure::SortComponentCatalog;

bool MergeProjectComponentsFromEndpoint(const TrackerConfig& /*cfg*/, const std::string& projectKey,
                                        const std::string& base, const cpr::Header& headers,
                                        std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components) {
    if (projectKey.empty()) {
        return false;
    }

    bool mergedAny = false;
    constexpr int kPageSize = 100;
    constexpr int kMaxPages = 50;
    for (int page = 0; page < kMaxPages; ++page) {
        const int startAt = page * kPageSize;
        const std::string url = base + "/rest/api/3/project/" + UrlEncode(projectKey) +
                                "/component?startAt=" + std::to_string(startAt) +
                                "&maxResults=" + std::to_string(kPageSize) + "&orderBy=name";
        auto response = TrackerGetLogged("JiraClient", url, headers);
        if (response.status_code != 200) {
            LOG_WARN("JiraClient: project components enrichment failed. HTTP %d", response.status_code);
            return mergedAny;
        }

        std::string parseErr;
        auto json = smatchet::json_safe::ParseBounded(response.text, parseErr);
        if (!parseErr.empty() || !json.is_object()) {
            LOG_WARN("JiraClient: project components response parse failed: %s", parseErr.c_str());
            return mergedAny;
        }

        const auto valuesIt = json.find("values");
        if (valuesIt == json.end() || !valuesIt->is_array()) {
            LOG_WARN("JiraClient: project components response missing values array.");
            return mergedAny;
        }

        for (const auto& node : *valuesIt) {
            TrackerComponent component;
            TrackerFieldOption option;
            if (!ExtractComponentOption(node, component, option)) {
                continue;
            }
            MergeComponentIntoCatalog(fields, components, component, option);
            mergedAny = true;
        }

        const bool isLast = json.value("isLast", false);
        if (isLast || valuesIt->empty()) {
            break;
        }
    }
    return mergedAny;
}

using IndexByIdMap = std::unordered_map<std::string, size_t>;

// Project-agnostic enrichment: priority + status + issuetype options come from global endpoints, so
// run them even without a projectKey. Without this the grid renders priority/status as text instead
// of a combo dropdown until the user opens a project-scoped view.
void EnrichGlobalCatalogFields(const std::string& base, const cpr::Header& headers, const IndexByIdMap& fieldIndexById,
                               std::vector<TrackerField>& outFields) {
    try {
        const auto priorityFieldIt = fieldIndexById.find("priority");
        if (priorityFieldIt != fieldIndexById.end()) {
            auto priorityResp = TrackerGetLogged("JiraClient", base + "/rest/api/3/priority", headers);
            if (priorityResp.status_code == 200) {
                std::string parseErr;
                auto priorityJson = smatchet::json_safe::ParseBounded(priorityResp.text, parseErr);
                if (!parseErr.empty()) {
                    LOG_WARN("JiraClient: priority catalog parse failed: %s", parseErr.c_str());
                } else {
                    TrackerFieldCatalogPure::BuildSimpleCatalogOptions(priorityJson,
                                                                       outFields[priorityFieldIt->second]);
                }
            } else {
                LOG_WARN("JiraClient: priority catalog enrichment failed. HTTP %d", priorityResp.status_code);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: priority catalog parse failed: %s", ex.what());
    }

    try {
        const auto issueTypeFieldIt = fieldIndexById.find("issuetype");
        if (issueTypeFieldIt != fieldIndexById.end()) {
            auto issueTypeResp = TrackerGetLogged("JiraClient", base + "/rest/api/3/issuetype", headers);
            if (issueTypeResp.status_code == 200) {
                std::string parseErr;
                auto issueTypeJson = smatchet::json_safe::ParseBounded(issueTypeResp.text, parseErr);
                if (!parseErr.empty()) {
                    LOG_WARN("JiraClient: issuetype catalog parse failed: %s", parseErr.c_str());
                } else if (issueTypeJson.is_array()) {
                    TrackerField& issueTypeField = outFields[issueTypeFieldIt->second];
                    TrackerFieldCatalogPure::BuildDedupedIssueTypeOptions(issueTypeJson, issueTypeField.AllowedValues,
                                                                          issueTypeField.AllowedValueOptions);
                    issueTypeField.Family = ClassifyTrackerFieldFamily(issueTypeField);
                }
            } else {
                LOG_WARN("JiraClient: issuetype catalog enrichment failed. HTTP %d", issueTypeResp.status_code);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: issuetype catalog parse failed: %s", ex.what());
    }

    try {
        const auto statusFieldIt = fieldIndexById.find("status");
        if (statusFieldIt != fieldIndexById.end()) {
            auto statusResp = TrackerGetLogged("JiraClient", base + "/rest/api/3/status", headers);
            if (statusResp.status_code == 200) {
                std::string parseErr;
                auto statusJson = smatchet::json_safe::ParseBounded(statusResp.text, parseErr);
                if (!parseErr.empty()) {
                    LOG_WARN("JiraClient: status catalog parse failed: %s", parseErr.c_str());
                } else {
                    TrackerFieldCatalogPure::BuildSimpleCatalogOptions(statusJson, outFields[statusFieldIt->second]);
                }
            } else {
                LOG_WARN("JiraClient: status catalog enrichment failed. HTTP %d", statusResp.status_code);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: status catalog parse failed: %s", ex.what());
    }
}

// Createmeta enrichment: per-issue-type required-field flags + allowedValues merge, plus the
// authoritative project component endpoint (createmeta only returns create-screen components).
void EnrichFromCreateMeta(const TrackerConfig& cfg, const std::string& projectKey, const std::string& base,
                          const cpr::Header& headers, const IndexByIdMap& fieldIndexById,
                          std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                          std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta,
                          std::set<std::string>& outUniqueIssueTypes) {
    const std::string metaUrl = base + "/rest/api/3/issue/createmeta?projectKeys=" + UrlEncode(projectKey) +
                                "&expand=projects.issuetypes.fields";
    auto metaResponse = TrackerGetLogged("JiraClient", metaUrl, headers);
    if (metaResponse.status_code == 200) {
        try {
            // SMATCHET_DEVIATION(rule=duplication; reason=ParseBounded clone #8; owner=cpp-audit; revisit=2026-09-30)
            std::string parseErr;
            auto metaJson = smatchet::json_safe::ParseBounded(metaResponse.text, parseErr);
            if (!parseErr.empty()) {
                LOG_WARN("JiraClient: createmeta parse failed: %s", parseErr.c_str());
            } else {
                TrackerFieldCatalogPure::ApplyCreateMetaToCatalog(metaJson, projectKey, fieldIndexById, outFields,
                                                                  outComponents, outIssueTypeMeta, outUniqueIssueTypes);
            }
        } catch (const std::exception& ex) {
            LOG_WARN("JiraClient: createmeta parse failed: %s", ex.what());
        }
    } else {
        LOG_WARN("JiraClient: createmeta enrichment failed. HTTP %d", metaResponse.status_code);
    }

    try {
        (void)MergeProjectComponentsFromEndpoint(cfg, projectKey, base, headers, outFields, outComponents);
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: project components enrichment failed: %s", ex.what());
    } catch (...) {
        LOG_WARN("JiraClient: project components enrichment failed (unknown exception).");
    }
}

// Issue type: createmeta often lists only one type (e.g. Epic). Prefer GET /project/{key} issueTypes
// (full list + real ids for PUT). Fall back to the createmeta name-only set if both global enrichments
// failed — the name-only fallback loses real IDs needed for PUT, so it is the last resort.
void EnrichIssueTypeFromProject(const std::string& projectKey, const std::string& base, const cpr::Header& headers,
                                const IndexByIdMap& fieldIndexById, std::vector<TrackerField>& outFields,
                                const std::set<std::string>& uniqueIssueTypes) {
    const auto issueTypeIt = fieldIndexById.find("issuetype");
    if (issueTypeIt == fieldIndexById.end()) {
        return;
    }
    TrackerField& issueTypeField = outFields[issueTypeIt->second];
    bool filledFromProject = false;
    try {
        const std::string projectUrl = base + "/rest/api/3/project/" + UrlEncode(projectKey);
        auto projectResp = TrackerGetLogged("JiraClient", projectUrl, headers);
        if (projectResp.status_code == 200) {
            std::string parseErr;
            auto projectJson = smatchet::json_safe::ParseBounded(projectResp.text, parseErr);
            if (!parseErr.empty()) {
                LOG_WARN("JiraClient: project issueTypes parse failed: %s", parseErr.c_str());
            } else {
                std::vector<TrackerFieldOption> opts;
                if (TrackerFieldCatalogPure::BuildIssueTypeOptionsFromProjectJson(projectJson, opts)) {
                    issueTypeField.AllowedValueOptions = std::move(opts);
                    RefreshTrackerAllowedValuesFromOptions(issueTypeField);
                    issueTypeField.Family = ClassifyTrackerFieldFamily(issueTypeField);
                    filledFromProject = true;
                }
            }
        } else {
            LOG_WARN("JiraClient: project fetch for issue types failed. HTTP %d", projectResp.status_code);
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: project issueTypes parse failed: %s", ex.what());
    }

    if (!filledFromProject && !uniqueIssueTypes.empty() && issueTypeField.AllowedValueOptions.empty()) {
        issueTypeField.AllowedValues.assign(uniqueIssueTypes.begin(), uniqueIssueTypes.end());
        std::sort(issueTypeField.AllowedValues.begin(), issueTypeField.AllowedValues.end());
        issueTypeField.AllowedValueOptions.clear();
        for (const auto& issueTypeName : issueTypeField.AllowedValues) {
            TrackerFieldOption option;
            option.Id = issueTypeName;
            option.Value = issueTypeName;
            issueTypeField.AllowedValueOptions.push_back(option);
        }
        issueTypeField.Family = ClassifyTrackerFieldFamily(issueTypeField);
    }
}

// Discover Jira Agile boards for the project, paging until isLast.
std::vector<int> DiscoverSprintBoardIds(const std::string& projectKey, const std::string& base,
                                        const cpr::Header& headers) {
    std::vector<int> boardIds;
    std::set<int> seenBoardIds;
    const int kBoardsPerPage = 50;
    const int kMaxBoardPages = 20;
    for (int page = 0; page < kMaxBoardPages; ++page) {
        const int startAt = page * kBoardsPerPage;
        const std::string boardsUrl = base + "/rest/agile/1.0/board?projectKeyOrId=" + UrlEncode(projectKey) +
                                      "&maxResults=" + std::to_string(kBoardsPerPage) +
                                      "&startAt=" + std::to_string(startAt);
        auto boardsResp = TrackerGetLogged("JiraClient", boardsUrl, headers);
        if (boardsResp.status_code != 200) {
            LOG_WARN("JiraClient: sprint board discovery failed (HTTP %d).", boardsResp.status_code);
            break;
        }
        std::string parseErr;
        auto boardsJson = smatchet::json_safe::ParseBounded(boardsResp.text, parseErr);
        if (!parseErr.empty() || !boardsJson.is_object() || !boardsJson.contains("values") ||
            !boardsJson["values"].is_array()) {
            LOG_WARN("JiraClient: sprint board discovery response missing values array.");
            break;
        }
        if (TrackerFieldCatalogPure::ExtractSprintBoardIdsFromPage(boardsJson, seenBoardIds, boardIds)) {
            break;
        }
    }
    return boardIds;
}

// Enrich sprint custom fields with selectable sprint options (active/future) from Jira Agile.
void EnrichSprintFields(const std::vector<std::string>& sprintFieldIds, const std::string& projectKey,
                        const std::string& base, const cpr::Header& headers, const IndexByIdMap& fieldIndexById,
                        std::vector<TrackerField>& outFields) {
    if (sprintFieldIds.empty() || projectKey.empty()) {
        return;
    }
    try {
        const std::vector<int> boardIds = DiscoverSprintBoardIds(projectKey, base, headers);

        std::vector<TrackerFieldOption> sprintOptions;
        std::set<std::string> seenSprintIds;
        for (int boardId : boardIds) {
            const std::string sprintUrl = base + "/rest/agile/1.0/board/" + std::to_string(boardId) +
                                          "/sprint?state=active,future&maxResults=100";
            auto sprintResp = TrackerGetLogged("JiraClient", sprintUrl, headers);
            if (sprintResp.status_code != 200) {
                continue;
            }
            std::string sprintParseErr;
            auto sprintJson = smatchet::json_safe::ParseBounded(sprintResp.text, sprintParseErr);
            if (!sprintParseErr.empty()) {
                continue;
            }
            TrackerFieldCatalogPure::CollectSprintOptionsFromPage(sprintJson, seenSprintIds, sprintOptions);
        }

        if (sprintOptions.empty()) {
            return;
        }
        std::sort(sprintOptions.begin(), sprintOptions.end(),
                  [](const TrackerFieldOption& a, const TrackerFieldOption& b) { return a.Value < b.Value; });
        for (const auto& sprintFieldId : sprintFieldIds) {
            const auto fit = fieldIndexById.find(sprintFieldId);
            if (fit == fieldIndexById.end()) {
                continue;
            }
            TrackerField& targetField = outFields[fit->second];
            if (targetField.AllowedValueOptions.empty()) {
                targetField.AllowedValueOptions = sprintOptions;
            } else {
                for (const auto& opt : sprintOptions) {
                    MergeTrackerFieldOption(targetField.AllowedValueOptions, opt);
                }
                std::sort(targetField.AllowedValueOptions.begin(), targetField.AllowedValueOptions.end(),
                          [](const TrackerFieldOption& a, const TrackerFieldOption& b) { return a.Value < b.Value; });
            }
            RefreshTrackerAllowedValuesFromOptions(targetField);
            targetField.Family = ClassifyTrackerFieldFamily(targetField);
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: sprint enrichment failed: %s", ex.what());
    } catch (...) {
        LOG_WARN("JiraClient: sprint enrichment failed (unknown exception).");
    }
}

// Fetch + parse the global /field list into the catalog, collecting sprint custom-field ids.
bool FetchAndParseFieldList(const std::string& base, const cpr::Header& headers, std::vector<TrackerField>& outFields,
                            std::vector<std::string>& outSprintFieldIds, std::string& outError) {
    // SMATCHET_DEVIATION(rule=duplication; reason=ParseBounded clone #8; owner=cpp-audit; revisit=2026-09-30)
    const std::string fieldsListUrl = base + "/rest/api/3/field";
    auto fieldsResponse = TrackerGetLogged("JiraClient", fieldsListUrl, headers);
    if (fieldsResponse.status_code != 200) {
        outError = "Failed to fetch fields: HTTP " + std::to_string(fieldsResponse.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        std::string parseErr;
        auto response = smatchet::json_safe::ParseBounded(fieldsResponse.text, parseErr);
        if (!parseErr.empty()) {
            outError = "Failed to parse /field response: " + parseErr;
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return false;
        }
        if (!response.is_array()) {
            outError = "Unexpected /field response shape.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), RedactHttpBodyForLog(fieldsResponse.text).c_str());
            return false;
        }
        for (const auto& field : response) {
            TrackerField parsedField;
            if (TrackerFieldCatalogPure::ParseFieldDefinition(field, parsedField, outSprintFieldIds)) {
                outFields.push_back(std::move(parsedField));
            }
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse /field response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }
    return true;
}

} // namespace

Result<TrackerFieldCatalogResult, TrackerError> JiraClient::FetchFieldCatalog(const TrackerConfig& cfg,
                                                                              const std::string& projectKey) {
    TrackerFieldCatalogResult outCatalog;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string outError;
    if (!FetchFieldCatalog(cfg, projectKey, fields, components, issueTypeMeta, outError)) {
        // TODO(#21b later slice): re-thread status from inner helper instead of collapsing to Unknown — IsRetryable()
        // consumers land in a later slice.
        return Result<TrackerFieldCatalogResult, TrackerError>::Err(TrackerErrorUnknown(std::move(outError)));
    }

    std::vector<TrackerUser> users;
    std::string usersError;
    if (!FetchUsers(cfg, users, usersError)) {
        outCatalog.Warning = usersError;
    }

    if (!users.empty()) {
        for (auto& field : fields) {
            if (!field.IsUserType) {
                continue;
            }
            field.AllowedValues.clear();
            field.AllowedValueOptions.clear();
            field.AllowedValues.reserve(users.size());
            field.AllowedValueOptions.reserve(users.size());
            for (const auto& user : users) {
                field.AllowedValues.push_back(user.DisplayName);
                TrackerFieldOption option;
                option.Id = user.AccountId;
                option.Value = user.DisplayName;
                field.AllowedValueOptions.push_back(std::move(option));
            }
        }
    }

    outCatalog.Fields = fields;
    outCatalog.Components = components;
    outCatalog.IssueTypeMeta = issueTypeMeta;
    outCatalog.Users = users;
    return Result<TrackerFieldCatalogResult, TrackerError>::Ok(std::move(outCatalog));
}

bool JiraClient::FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                                   std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                                   std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError) {
    outFields.clear();
    outComponents.clear();
    outIssueTypeMeta.clear();
    outError.clear();

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }

    // Project source is an explicit per-operation parameter (no global cfg.ProjectKey).
    // Empty projectKey ≡ unscoped (no create-meta enrichment).
    // See docs/plans/shipped/remove-global-project-key.md §2.6 / §7.
    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);
    std::vector<std::string> sprintFieldIds;

    // Phase 1: fetch + parse the global /field list.
    if (!FetchAndParseFieldList(base, headers, outFields, sprintFieldIds, outError)) {
        return false;
    }

    std::unordered_map<std::string, size_t> fieldIndexById;
    for (size_t i = 0; i < outFields.size(); ++i) {
        fieldIndexById[outFields[i].Id] = i;
    }

    // Phase 2: project-agnostic enrichment (priority / issuetype / status global catalogs).
    EnrichGlobalCatalogFields(base, headers, fieldIndexById, outFields);

    if (projectKey.empty()) {
        LOG_INFO("JiraClient: skipping createmeta enrichment because project key is empty.");
        return true;
    }

    // Phase 3: project-scoped enrichment (createmeta + components, project issue types, sprints).
    std::set<std::string> uniqueIssueTypes;
    EnrichFromCreateMeta(cfg, projectKey, base, headers, fieldIndexById, outFields, outComponents, outIssueTypeMeta,
                         uniqueIssueTypes);
    EnrichIssueTypeFromProject(projectKey, base, headers, fieldIndexById, outFields, uniqueIssueTypes);
    EnrichSprintFields(sprintFieldIds, projectKey, base, headers, fieldIndexById, outFields);

    // Phase 4: finalize families + component ordering.
    for (auto& field : outFields) {
        field.Family = ClassifyTrackerFieldFamily(field);
    }
    SortComponentCatalog(outFields, outComponents);
    return true;
}

Result<TrackerProjectComponents, TrackerError> JiraClient::FetchProjectComponents(const TrackerConfig& cfg,
                                                                                  const std::string& projectKey) {
    TrackerProjectComponents out;
    std::string outError;

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return Result<TrackerProjectComponents, TrackerError>::Err(TrackerErrorInvalidRequest(std::move(outError)));
    }
    if (projectKey.empty()) {
        return Result<TrackerProjectComponents, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchProjectComponents called with an empty project key."));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    // Paged GET /rest/api/3/project/{key}/component — mirrors MergeProjectComponentsFromEndpoint
    // paging, but pushes component + option directly (no catalog/fields vector).
    constexpr int kPageSize = 100;
    constexpr int kMaxPages = 50;
    bool reachedTerminalPage = false;
    for (int page = 0; page < kMaxPages; ++page) {
        const int startAt = page * kPageSize;
        const std::string url = base + "/rest/api/3/project/" + UrlEncode(projectKey) +
                                "/component?startAt=" + std::to_string(startAt) +
                                "&maxResults=" + std::to_string(kPageSize) + "&orderBy=name";
        auto response = TrackerGetLogged("JiraClient", url, headers);
        if (response.status_code != 200) {
            LOG_WARN("JiraClient: per-project components fetch failed for %s. HTTP %d", projectKey.c_str(),
                     response.status_code);
            std::string detail =
                "Per-project components fetch failed (HTTP " + std::to_string(response.status_code) + ").";
            // A 2xx-non-200 reaches this failure branch; TrackerErrorFromHttpStatus would return Ok()
            // (Kind==None, detail discarded). Carry the verbatim detail under an explicit non-OK kind.
            if (response.status_code >= 200 && response.status_code < 300) {
                return Result<TrackerProjectComponents, TrackerError>::Err(
                    TrackerErrorUnknown(std::move(detail), response.status_code));
            }
            return Result<TrackerProjectComponents, TrackerError>::Err(
                TrackerErrorFromHttpStatus(response.status_code, std::move(detail)));
        }

        std::string parseErr;
        auto json = smatchet::json_safe::ParseBounded(response.text, parseErr);
        if (!parseErr.empty() || !json.is_object()) {
            LOG_WARN("JiraClient: per-project components response parse failed for %s: %s", projectKey.c_str(),
                     parseErr.c_str());
            return Result<TrackerProjectComponents, TrackerError>::Err(
                TrackerErrorParse("Per-project components response parse failed."));
        }

        const auto valuesIt = json.find("values");
        if (valuesIt == json.end() || !valuesIt->is_array()) {
            LOG_WARN("JiraClient: per-project components response missing values array for %s.", projectKey.c_str());
            return Result<TrackerProjectComponents, TrackerError>::Err(
                TrackerErrorParse("Per-project components response missing values array."));
        }

        for (const auto& node : *valuesIt) {
            TrackerComponent component;
            TrackerFieldOption option;
            if (!ExtractComponentOption(node, component, option)) {
                continue;
            }
            out.Components.push_back(component);
            out.Options.push_back(option);
        }

        const bool isLast = json.value("isLast", false);
        if (isLast || valuesIt->empty()) {
            reachedTerminalPage = true;
            break;
        }
    }

    if (!reachedTerminalPage) {
        // kMaxPages exhausted without an isLast / empty page — we silently dropped components for a
        // very large project. Report failure so callers (and the per-project retry backoff) treat
        // this as an incomplete fetch rather than a complete one.
        LOG_WARN("JiraClient: per-project components fetch hit page cap (%d) for %s without a terminal page; "
                 "result is incomplete.",
                 kMaxPages, projectKey.c_str());
        return Result<TrackerProjectComponents, TrackerError>::Err(
            TrackerErrorInvalidRequest("Per-project components fetch incomplete (page cap reached)."));
    }

    std::sort(out.Options.begin(), out.Options.end(), [](const TrackerFieldOption& a, const TrackerFieldOption& b) {
        const std::string lowerA = ToLowerAsciiCopy(a.Value);
        const std::string lowerB = ToLowerAsciiCopy(b.Value);
        if (lowerA != lowerB) {
            return lowerA < lowerB;
        }
        return a.Id < b.Id;
    });
    return Result<TrackerProjectComponents, TrackerError>::Ok(std::move(out));
}
