#include "JiraClient.h"

#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"

#include "JsonParseUtil.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string ComponentJsonIdToString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    return {};
}

const nlohmann::json* ResolveComponentJsonBean(const nlohmann::json& node) {
    if (!node.is_object()) {
        return nullptr;
    }
    const auto beanIt = node.find("componentBean");
    if (beanIt != node.end() && beanIt->is_object()) {
        return &(*beanIt);
    }
    return &node;
}

bool ExtractComponentOption(const nlohmann::json& node, TrackerComponent& outComponent,
                            TrackerFieldOption& outOption) {
    const nlohmann::json* component = ResolveComponentJsonBean(node);
    if (component == nullptr) {
        return false;
    }

    const auto idIt = component->find("id");
    const auto nameIt = component->find("name");
    if (idIt == component->end() || nameIt == component->end() || !nameIt->is_string()) {
        return false;
    }

    const std::string id = ComponentJsonIdToString(*idIt);
    const std::string name = nameIt->get<std::string>();
    if (id.empty() || name.empty()) {
        return false;
    }

    outComponent.Id = id;
    outComponent.Name = name;

    outOption.Id = id;
    outOption.Value = name;
    outOption.SecondaryValue = JsonGetStringIfString(*component, "description");
    try {
        nlohmann::json payload = nlohmann::json::object({{"id", id}, {"name", name}});
        const auto selfIt = component->find("self");
        if (selfIt != component->end() && selfIt->is_string()) {
            payload["self"] = *selfIt;
        }
        const auto descIt = component->find("description");
        if (descIt != component->end() && descIt->is_string()) {
            payload["description"] = *descIt;
        }
        outOption.PayloadJson = payload.dump();
    } catch (...) {
        outOption.PayloadJson.clear();
    }
    return true;
}

void MergeComponentIntoCatalog(std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components,
                               const TrackerComponent& component, const TrackerFieldOption& option) {
    auto componentIt = std::find_if(components.begin(), components.end(), [&](const TrackerComponent& existing) {
        return existing.Id == component.Id;
    });
    if (componentIt == components.end()) {
        components.push_back(component);
    } else if (componentIt->Name.empty()) {
        componentIt->Name = component.Name;
    }

    auto fieldIt = std::find_if(fields.begin(), fields.end(), [](const TrackerField& field) {
        return field.Id == "components";
    });
    if (fieldIt == fields.end()) {
        return;
    }

    MergeTrackerFieldOption(fieldIt->AllowedValueOptions, option);
}

bool MergeProjectComponentsFromEndpoint(const TrackerConfig& cfg, const std::string& base, const cpr::Header& headers,
                                        std::vector<TrackerField>& fields,
                                        std::vector<TrackerComponent>& components) {
    if (cfg.ProjectKey.empty()) {
        return false;
    }

    bool mergedAny = false;
    constexpr int kPageSize = 100;
    constexpr int kMaxPages = 50;
    for (int page = 0; page < kMaxPages; ++page) {
        const int startAt = page * kPageSize;
        const std::string url = base + "/rest/api/3/project/" + UrlEncode(cfg.ProjectKey) +
                                "/component?startAt=" + std::to_string(startAt) +
                                "&maxResults=" + std::to_string(kPageSize) + "&orderBy=name";
        auto response = TrackerGetLogged("JiraClient", url, headers);
        if (response.status_code != 200) {
            LOG_WARN("JiraClient: project components enrichment failed. HTTP %d", response.status_code);
            return mergedAny;
        }

        auto json = nlohmann::json::parse(response.text, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            LOG_WARN("JiraClient: project components response parse failed.");
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

void SortComponentCatalog(std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components) {
    std::sort(components.begin(), components.end(), [](const TrackerComponent& a, const TrackerComponent& b) {
        const std::string lowerA = ToLowerAsciiCopy(a.Name);
        const std::string lowerB = ToLowerAsciiCopy(b.Name);
        if (lowerA != lowerB) {
            return lowerA < lowerB;
        }
        return a.Id < b.Id;
    });

    auto fieldIt = std::find_if(fields.begin(), fields.end(), [](const TrackerField& field) {
        return field.Id == "components";
    });
    if (fieldIt == fields.end()) {
        return;
    }
    std::sort(fieldIt->AllowedValueOptions.begin(), fieldIt->AllowedValueOptions.end(),
              [](const TrackerFieldOption& a, const TrackerFieldOption& b) {
                  const std::string lowerA = ToLowerAsciiCopy(a.Value);
                  const std::string lowerB = ToLowerAsciiCopy(b.Value);
                  if (lowerA != lowerB) {
                      return lowerA < lowerB;
                  }
                  return a.Id < b.Id;
              });
    RefreshTrackerAllowedValuesFromOptions(*fieldIt);
    fieldIt->Family = ClassifyTrackerFieldFamily(*fieldIt);
}

} // namespace

bool JiraClient::FetchFieldCatalog(const TrackerConfig& cfg, TrackerFieldCatalogResult& outCatalog,
                                   std::string& outError) {
    outCatalog = TrackerFieldCatalogResult{};
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    if (!FetchFieldCatalog(cfg, fields, components, issueTypeMeta, outError)) {
        return false;
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
    return true;
}

bool JiraClient::FetchFieldCatalog(const TrackerConfig& cfg, std::vector<TrackerField>& outFields,
                                   std::vector<TrackerComponent>& outComponents,
                                   std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError) {
    outFields.clear();
    outComponents.clear();
    outIssueTypeMeta.clear();
    outError.clear();

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);
    std::vector<std::string> sprintFieldIds;

    const std::string fieldsListUrl = base + "/rest/api/3/field";
    auto fieldsResponse = TrackerGetLogged("JiraClient", fieldsListUrl, headers);
    if (fieldsResponse.status_code != 200) {
        outError = "Failed to fetch fields: HTTP " + std::to_string(fieldsResponse.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        auto response = nlohmann::json::parse(fieldsResponse.text);
        if (!response.is_array()) {
            outError = "Unexpected /field response shape.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), TruncateForLog(fieldsResponse.text, 300).c_str());
            return false;
        }

        for (const auto& field : response) {
            if (!field.contains("id") || !field.contains("name") || !field["id"].is_string() ||
                !field["name"].is_string()) {
                continue;
            }

            const std::string fieldId = field["id"].get<std::string>();

            TrackerField TrackerField;
            TrackerField.Id = fieldId;
            TrackerField.Name = field["name"].get<std::string>();
            TrackerField.Type = "unknown";
            TrackerField.ReadOnly = field.value("isLocked", false);

            if (field.contains("schema") && field["schema"].is_object()) {
                const auto& schema = field["schema"];
                if (schema.contains("type") && schema["type"].is_string()) {
                    TrackerField.Type = schema["type"].get<std::string>();
                }
                TrackerField.SchemaSystem = (schema.contains("system") && schema["system"].is_string())
                                             ? schema["system"].get<std::string>()
                                             : std::string();
                TrackerField.SchemaCustom = (schema.contains("custom") && schema["custom"].is_string())
                                             ? schema["custom"].get<std::string>()
                                             : std::string();

                TrackerField.IsArray = (TrackerField.Type == "array");
                if (TrackerField.IsArray && schema.contains("items")) {
                    if (schema["items"].is_string()) {
                        TrackerField.ItemsType = schema["items"].get<std::string>();
                    } else if (schema["items"].is_object() && schema["items"].contains("type") &&
                               schema["items"]["type"].is_string()) {
                        TrackerField.ItemsType = schema["items"]["type"].get<std::string>();
                    }
                }
                TrackerField.IsUserType =
                    (TrackerField.Type == "user") || (TrackerField.IsArray && TrackerField.ItemsType == "user");
                if (!TrackerField.SchemaCustom.empty() && TrackerField.SchemaCustom.find("gh-sprint") != std::string::npos) {
                    sprintFieldIds.push_back(fieldId);
                }
            }

            TrackerField.IsCustom = (fieldId.find("customfield_") == 0);
            TrackerField.Family = ClassifyTrackerFieldFamily(TrackerField);
            try {
                TrackerField.RawFieldDefinitionJson = field.dump(2);
            } catch (const std::exception& ex) {
                LOG_DEBUG("JiraClient: field definition dump failed field=%s err=%s", fieldId.c_str(), ex.what());
                TrackerField.RawFieldDefinitionJson.clear();
            } catch (...) {
                LOG_DEBUG("JiraClient: field definition dump failed field=%s (unknown)", fieldId.c_str());
                TrackerField.RawFieldDefinitionJson.clear();
            }
            outFields.push_back(std::move(TrackerField));
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse /field response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    if (cfg.ProjectKey.empty()) {
        LOG_INFO("JiraClient: skipping createmeta enrichment because project key is empty.");
        return true;
    }

    std::unordered_map<std::string, size_t> fieldIndexById;
    for (size_t i = 0; i < outFields.size(); ++i) {
        fieldIndexById[outFields[i].Id] = i;
    }

    const std::string metaUrl = base + "/rest/api/3/issue/createmeta?projectKeys=" + UrlEncode(cfg.ProjectKey) +
                                "&expand=projects.issuetypes.fields";
    auto metaResponse = TrackerGetLogged("JiraClient", metaUrl, headers);
    std::set<std::string> uniqueIssueTypes;
    if (metaResponse.status_code == 200) {
        try {
            auto metaJson = nlohmann::json::parse(metaResponse.text);
            std::unordered_map<std::string, std::size_t> issueTypeMetaIndexByKey;
            const auto issueTypeMetaKey = [](const TrackerIssueTypeCreateMeta& m) -> std::string {
                if (!m.IssueTypeId.empty()) {
                    return m.ProjectKey + '\x1f' + m.IssueTypeId;
                }
                return m.ProjectKey + '\x1f' + m.IssueTypeName;
            };
            const auto upsertIssueTypeMeta = [&](TrackerIssueTypeCreateMeta entry) {
                if (entry.IssueTypeId.empty() && entry.IssueTypeName.empty()) {
                    return;
                }
                const std::string k = issueTypeMetaKey(entry);
                const auto found = issueTypeMetaIndexByKey.find(k);
                if (found == issueTypeMetaIndexByKey.end()) {
                    issueTypeMetaIndexByKey[k] = outIssueTypeMeta.size();
                    outIssueTypeMeta.push_back(std::move(entry));
                    return;
                }
                TrackerIssueTypeCreateMeta& dst = outIssueTypeMeta[found->second];
                dst.RequiredFieldIds.insert(entry.RequiredFieldIds.begin(), entry.RequiredFieldIds.end());
                if (dst.IssueTypeName.empty() && !entry.IssueTypeName.empty()) {
                    dst.IssueTypeName = entry.IssueTypeName;
                }
                if (dst.IssueTypeId.empty() && !entry.IssueTypeId.empty()) {
                    dst.IssueTypeId = entry.IssueTypeId;
                }
                dst.IsSubtask = dst.IsSubtask || entry.IsSubtask;
            };

            if (metaJson.contains("projects") && metaJson["projects"].is_array()) {
                std::set<std::string> uniqueComponentIds;
                for (const auto& project : metaJson["projects"]) {
                    std::string projectKey = project.value("key", cfg.ProjectKey);
                    if (!project.contains("issuetypes") || !project["issuetypes"].is_array()) {
                        continue;
                    }

                    for (const auto& issueType : project["issuetypes"]) {
                        if (issueType.contains("name") && issueType["name"].is_string()) {
                            uniqueIssueTypes.insert(issueType["name"].get<std::string>());
                        }

                        TrackerIssueTypeCreateMeta metaEntry;
                        metaEntry.ProjectKey = projectKey;
                        metaEntry.IsSubtask = issueType.value("subtask", false);
                        if (issueType.contains("id")) {
                            if (issueType["id"].is_string()) {
                                metaEntry.IssueTypeId = issueType["id"].get<std::string>();
                            } else if (issueType["id"].is_number_integer()) {
                                metaEntry.IssueTypeId = std::to_string(issueType["id"].get<long long>());
                            } else if (issueType["id"].is_number_unsigned()) {
                                metaEntry.IssueTypeId = std::to_string(issueType["id"].get<unsigned long long>());
                            }
                        }
                        metaEntry.IssueTypeName = issueType.value("name", std::string());

                        if (!issueType.contains("fields") || !issueType["fields"].is_object()) {
                            upsertIssueTypeMeta(std::move(metaEntry));
                            continue;
                        }

                        // Pass 1: collect per-screen "required" flags from createmeta field schemas.
                        for (auto it = issueType["fields"].begin(); it != issueType["fields"].end(); ++it) {
                            const std::string requiredFieldId = it.key();
                            if (it.value().is_object() && it.value().value("required", false)) {
                                metaEntry.RequiredFieldIds.insert(requiredFieldId);
                            }
                        }

                        // Pass 2: merge allowedValues into the global field catalog (and collect components).
                        for (auto it = issueType["fields"].begin(); it != issueType["fields"].end(); ++it) {
                            const std::string fieldId = it.key();
                            const auto& fieldObj = it.value();

                            if (fieldId == "components" && fieldObj.contains("allowedValues") &&
                                fieldObj["allowedValues"].is_array()) {
                                for (const auto& val : fieldObj["allowedValues"]) {
                                    if (val.contains("id") && val.contains("name") && val["id"].is_string() &&
                                        val["name"].is_string()) {
                                        const std::string componentId = val["id"].get<std::string>();
                                        if (uniqueComponentIds.insert(componentId).second) {
                                            TrackerComponent component;
                                            component.Id = componentId;
                                            component.Name = val["name"].get<std::string>();
                                            outComponents.push_back(component);
                                        }
                                    }
                                }
                            }

                            if (!fieldObj.contains("allowedValues") || !fieldObj["allowedValues"].is_array()) {
                                continue;
                            }

                            const auto indexIt = fieldIndexById.find(fieldId);
                            if (indexIt == fieldIndexById.end()) {
                                continue;
                            }

                            // issuetype allowedValues in createmeta are per-screen (e.g. Epic may only list Epic).
                            // Use the full project issue type name set after this loop instead.
                            if (fieldId == "issuetype") {
                                continue;
                            }

                            TrackerField& targetField = outFields[indexIt->second];
                            for (const auto& val : fieldObj["allowedValues"]) {
                                TrackerFieldOption option = TrackerFieldOptionFromJson(val);
                                if (!option.Value.empty() || !option.Id.empty()) {
                                    MergeTrackerFieldOption(targetField.AllowedValueOptions, option);
                                }
                            }
                            RefreshTrackerAllowedValuesFromOptions(targetField);
                            targetField.Family = ClassifyTrackerFieldFamily(targetField);
                        }

                        upsertIssueTypeMeta(std::move(metaEntry));
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_WARN("JiraClient: createmeta parse failed: %s", ex.what());
        }
    } else {
        LOG_WARN("JiraClient: createmeta enrichment failed. HTTP %d", metaResponse.status_code);
    }

    // Createmeta only returns components that are available on create screens. The project component endpoint is the
    // authoritative project catalog and keeps the grid editor useful even when createmeta is sparse.
    try {
        (void)MergeProjectComponentsFromEndpoint(cfg, base, headers, outFields, outComponents);
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: project components enrichment failed: %s", ex.what());
    } catch (...) {
        LOG_WARN("JiraClient: project components enrichment failed (unknown exception).");
    }

    // Issue type: createmeta often lists only one type (e.g. Epic). Prefer GET /project/{key}
    // issueTypes (full list + real ids for PUT). Fall back to createmeta names if needed.
    {
        const auto issueTypeIt = fieldIndexById.find("issuetype");
        if (issueTypeIt != fieldIndexById.end()) {
            TrackerField& issueTypeField = outFields[issueTypeIt->second];
            bool filledFromProject = false;
            try {
                const std::string projectUrl = base + "/rest/api/3/project/" + UrlEncode(cfg.ProjectKey);
                auto projectResp = TrackerGetLogged("JiraClient", projectUrl, headers);
                if (projectResp.status_code == 200) {
                    auto projectJson = nlohmann::json::parse(projectResp.text);
                    if (projectJson.contains("issueTypes") && projectJson["issueTypes"].is_array()) {
                        std::vector<TrackerFieldOption> opts;
                        for (const auto& it : projectJson["issueTypes"]) {
                            if (!it.is_object()) {
                                continue;
                            }
                            std::string tid;
                            if (it.contains("id")) {
                                if (it["id"].is_string()) {
                                    tid = it["id"].get<std::string>();
                                } else if (it["id"].is_number_integer()) {
                                    tid = std::to_string(it["id"].get<long long>());
                                } else if (it["id"].is_number_unsigned()) {
                                    tid = std::to_string(it["id"].get<unsigned long long>());
                                }
                            }
                            const std::string tname = it.value("name", std::string());
                            if (tid.empty() || tname.empty()) {
                                continue;
                            }
                            TrackerFieldOption option;
                            option.Id = std::move(tid);
                            option.Value = tname;
                            try {
                                option.PayloadJson = it.dump();
                            } catch (...) {
                                option.PayloadJson.clear();
                            }
                            opts.push_back(std::move(option));
                        }
                        if (!opts.empty()) {
                            std::sort(opts.begin(), opts.end(), [](const TrackerFieldOption& a, const TrackerFieldOption& b) {
                                return a.Value < b.Value;
                            });
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

            if (!filledFromProject && !uniqueIssueTypes.empty()) {
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
    }

    // Enrich status options so the UI renders a dropdown.
    try {
        const auto statusFieldIt = fieldIndexById.find("status");
        if (statusFieldIt != fieldIndexById.end()) {
            const std::string statusCatalogUrl = base + "/rest/api/3/status";
            auto statusResp = TrackerGetLogged("JiraClient", statusCatalogUrl, headers);
            if (statusResp.status_code == 200) {
                auto statusJson = nlohmann::json::parse(statusResp.text);
                if (statusJson.is_array()) {
                    TrackerField& statusField = outFields[statusFieldIt->second];
                    statusField.AllowedValues.clear();
                    statusField.AllowedValueOptions.clear();
                    std::set<std::string> seenIds;
                    for (const auto& statusObj : statusJson) {
                        if (!statusObj.is_object()) {
                            continue;
                        }

                        std::string statusId;
                        if (statusObj.contains("id")) {
                            if (statusObj["id"].is_string()) {
                                statusId = statusObj["id"].get<std::string>();
                            } else if (statusObj["id"].is_number_integer()) {
                                statusId = std::to_string(statusObj["id"].get<long long>());
                            } else if (statusObj["id"].is_number_unsigned()) {
                                statusId = std::to_string(statusObj["id"].get<unsigned long long>());
                            }
                        }
                        const std::string statusName = statusObj.value("name", std::string());
                        if (statusId.empty() || statusName.empty() || !seenIds.insert(statusId).second) {
                            continue;
                        }

                        statusField.AllowedValues.push_back(statusName);
                        TrackerFieldOption option;
                        option.Id = statusId;
                        option.Value = statusName;
                        try {
                            option.PayloadJson = statusObj.dump();
                        } catch (...) {
                            option.PayloadJson.clear();
                        }
                        statusField.AllowedValueOptions.push_back(std::move(option));
                    }
                    statusField.Family = ClassifyTrackerFieldFamily(statusField);
                }
            } else {
                LOG_WARN("JiraClient: status catalog enrichment failed. HTTP %d", statusResp.status_code);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: status catalog parse failed: %s", ex.what());
    }

    // Enrich sprint custom fields with selectable sprint options (active/future) from Jira Agile.
    if (!sprintFieldIds.empty() && !cfg.ProjectKey.empty()) {
        try {
            std::vector<int> boardIds;
            std::set<int> seenBoardIds;
            const int kBoardsPerPage = 50;
            const int kMaxBoardPages = 20;
            for (int page = 0; page < kMaxBoardPages; ++page) {
                const int startAt = page * kBoardsPerPage;
                const std::string boardsUrl =
                    base + "/rest/agile/1.0/board?projectKeyOrId=" + UrlEncode(cfg.ProjectKey) +
                    "&maxResults=" + std::to_string(kBoardsPerPage) + "&startAt=" + std::to_string(startAt);
                auto boardsResp = TrackerGetLogged("JiraClient", boardsUrl, headers);
                if (boardsResp.status_code != 200) {
                    LOG_WARN("JiraClient: sprint board discovery failed (HTTP %d).", boardsResp.status_code);
                    break;
                }
                auto boardsJson = nlohmann::json::parse(boardsResp.text);
                if (!boardsJson.is_object() || !boardsJson.contains("values") || !boardsJson["values"].is_array()) {
                    LOG_WARN("JiraClient: sprint board discovery response missing values array.");
                    break;
                }

                const auto& values = boardsJson["values"];
                for (const auto& board : values) {
                    if (!board.is_object() || !board.contains("id")) {
                        continue;
                    }
                    const int bid = ParseJsonIntLoose(board["id"], -1);
                    if (bid > 0 && seenBoardIds.insert(bid).second) {
                        boardIds.push_back(bid);
                    }
                }

                const bool isLast = boardsJson.value("isLast", false);
                if (isLast || values.empty()) {
                    break;
                }
            }

            std::vector<TrackerFieldOption> sprintOptions;
            std::set<std::string> seenSprintIds;
            for (int boardId : boardIds) {
                const std::string sprintUrl = base + "/rest/agile/1.0/board/" + std::to_string(boardId) +
                                              "/sprint?state=active,future&maxResults=100";
                auto sprintResp = TrackerGetLogged("JiraClient", sprintUrl, headers);
                if (sprintResp.status_code != 200) {
                    continue;
                }
                auto sprintJson = nlohmann::json::parse(sprintResp.text);
                if (!sprintJson.is_object() || !sprintJson.contains("values") || !sprintJson["values"].is_array()) {
                    continue;
                }
                for (const auto& sprint : sprintJson["values"]) {
                    if (!sprint.is_object() || !sprint.contains("id")) {
                        continue;
                    }
                    std::string sprintId;
                    if (sprint["id"].is_string()) {
                        sprintId = sprint["id"].get<std::string>();
                    } else {
                        const int sid = ParseJsonIntLoose(sprint["id"], -1);
                        if (sid > 0) {
                            sprintId = std::to_string(sid);
                        }
                    }
                    const std::string sprintName = sprint.value("name", std::string());
                    if (sprintId.empty() || sprintName.empty() || !seenSprintIds.insert(sprintId).second) {
                        continue;
                    }
                    TrackerFieldOption opt;
                    opt.Id = sprintId;
                    opt.Value = sprintName;
                    try {
                        opt.PayloadJson = sprint.dump();
                    } catch (...) {
                        opt.PayloadJson.clear();
                    }
                    sprintOptions.push_back(std::move(opt));
                }
            }

            if (!sprintOptions.empty()) {
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
            }
        } catch (const std::exception& ex) {
            LOG_WARN("JiraClient: sprint enrichment failed: %s", ex.what());
        } catch (...) {
            LOG_WARN("JiraClient: sprint enrichment failed (unknown exception).");
        }
    }

    for (auto& field : outFields) {
        field.Family = ClassifyTrackerFieldFamily(field);
    }
    SortComponentCatalog(outFields, outComponents);
    return true;
}






