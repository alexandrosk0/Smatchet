#include "TrackerFieldCatalogPure.h"

#include "TrackerFieldValueParser.h"

#include "Json/BoundedJsonParse.h"
#include "JsonParseUtil.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TrackerFieldCatalogPure {

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

bool ExtractComponentOption(const nlohmann::json& node, TrackerComponent& outComponent, TrackerFieldOption& outOption) {
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
    auto componentIt = std::find_if(components.begin(), components.end(),
                                    [&](const TrackerComponent& existing) { return existing.Id == component.Id; });
    if (componentIt == components.end()) {
        components.push_back(component);
    } else if (componentIt->Name.empty()) {
        componentIt->Name = component.Name;
    }

    auto fieldIt =
        std::find_if(fields.begin(), fields.end(), [](const TrackerField& field) { return field.Id == "components"; });
    if (fieldIt == fields.end()) {
        return;
    }

    MergeTrackerFieldOption(fieldIt->AllowedValueOptions, option);
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

    auto fieldIt =
        std::find_if(fields.begin(), fields.end(), [](const TrackerField& field) { return field.Id == "components"; });
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

void BuildDedupedIssueTypeOptions(const nlohmann::json& issueTypeArray, std::vector<std::string>& outAllowedValues,
                                  std::vector<TrackerFieldOption>& outOptions) {
    outAllowedValues.clear();
    outOptions.clear();
    if (!issueTypeArray.is_array()) {
        return;
    }
    std::set<std::string> seenIds;
    std::map<std::string, size_t> indexByLowerName;
    for (const auto& issueTypeObj : issueTypeArray) {
        if (!issueTypeObj.is_object()) {
            continue;
        }
        const std::string issueTypeId = ComponentJsonIdToString(issueTypeObj.value("id", nlohmann::json(nullptr)));
        const std::string issueTypeName = issueTypeObj.value("name", std::string());
        if (issueTypeId.empty() || issueTypeName.empty() || !seenIds.insert(issueTypeId).second) {
            continue;
        }
        const bool hasProjectScope = issueTypeObj.contains("scope") && issueTypeObj["scope"].is_object() &&
                                     issueTypeObj["scope"].contains("project");
        TrackerFieldOption option;
        option.Id = issueTypeId;
        option.Value = issueTypeName;
        try {
            option.PayloadJson = issueTypeObj.dump();
        } catch (...) {
            option.PayloadJson.clear();
        }

        const std::string lowerName = ToLowerAsciiCopy(issueTypeName);
        const auto existing = indexByLowerName.find(lowerName);
        if (existing != indexByLowerName.end()) {
            TrackerFieldOption& prior = outOptions[existing->second];
            const nlohmann::json priorJson = prior.PayloadJson.empty()
                                                 ? nlohmann::json(nullptr)
                                                 : smatchet::json_safe::ParseBoundedOrDiscarded(prior.PayloadJson);
            const bool priorHasScope = priorJson.is_object() && priorJson.contains("scope") &&
                                       priorJson["scope"].is_object() && priorJson["scope"].contains("project");
            if (hasProjectScope && !priorHasScope) {
                prior = std::move(option);
            }
            continue;
        }
        indexByLowerName[lowerName] = outOptions.size();
        outAllowedValues.push_back(issueTypeName);
        outOptions.push_back(std::move(option));
    }
}

namespace {

// Coerce a Jira `id` JSON value (string | signed | unsigned) to a decimal
// string. Mirrors the inline id-coercion the catalog fetch repeats per phase.
std::string JsonIdToDecimalString(const nlohmann::json& obj) {
    const auto it = obj.find("id");
    if (it == obj.end()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    if (it->is_number_unsigned()) {
        return std::to_string(it->get<unsigned long long>());
    }
    return {};
}

} // namespace

bool ParseFieldDefinition(const nlohmann::json& field, TrackerField& outField,
                          std::vector<std::string>& outSprintFieldIds) {
    if (!field.contains("id") || !field.contains("name") || !field["id"].is_string() || !field["name"].is_string()) {
        return false;
    }

    const std::string fieldId = field["id"].get<std::string>();

    TrackerField parsed;
    parsed.Id = fieldId;
    parsed.Name = field["name"].get<std::string>();
    parsed.Type = "unknown";
    parsed.ReadOnly = field.value("isLocked", false);

    if (field.contains("schema") && field["schema"].is_object()) {
        const auto& schema = field["schema"];
        if (schema.contains("type") && schema["type"].is_string()) {
            parsed.Type = schema["type"].get<std::string>();
        }
        parsed.SchemaSystem = (schema.contains("system") && schema["system"].is_string())
                                  ? schema["system"].get<std::string>()
                                  : std::string();
        parsed.SchemaCustom = (schema.contains("custom") && schema["custom"].is_string())
                                  ? schema["custom"].get<std::string>()
                                  : std::string();

        parsed.IsArray = (parsed.Type == "array");
        if (parsed.IsArray && schema.contains("items")) {
            if (schema["items"].is_string()) {
                parsed.ItemsType = schema["items"].get<std::string>();
            } else if (schema["items"].is_object() && schema["items"].contains("type") &&
                       schema["items"]["type"].is_string()) {
                parsed.ItemsType = schema["items"]["type"].get<std::string>();
            }
        }
        parsed.IsUserType = (parsed.Type == "user") || (parsed.IsArray && parsed.ItemsType == "user");
        if (!parsed.SchemaCustom.empty() && parsed.SchemaCustom.find("gh-sprint") != std::string::npos) {
            outSprintFieldIds.push_back(fieldId);
        }
    }

    parsed.IsCustom = (fieldId.find("customfield_") == 0);
    parsed.Family = ClassifyTrackerFieldFamily(parsed);
    try {
        parsed.RawFieldDefinitionJson = field.dump(2);
    } catch (const std::exception& ex) {
        LOG_DEBUG("JiraClient: field definition dump failed field=%s err=%s", fieldId.c_str(), ex.what());
        parsed.RawFieldDefinitionJson.clear();
    } catch (...) {
        LOG_DEBUG("JiraClient: field definition dump failed field=%s (unknown)", fieldId.c_str());
        parsed.RawFieldDefinitionJson.clear();
    }

    outField = std::move(parsed);
    return true;
}

void BuildSimpleCatalogOptions(const nlohmann::json& array, TrackerField& field) {
    if (!array.is_array()) {
        return;
    }
    field.AllowedValues.clear();
    field.AllowedValueOptions.clear();
    std::set<std::string> seenIds;
    for (const auto& obj : array) {
        if (!obj.is_object()) {
            continue;
        }
        const std::string id = JsonIdToDecimalString(obj);
        const std::string name = obj.value("name", std::string());
        if (id.empty() || name.empty() || !seenIds.insert(id).second) {
            continue;
        }
        field.AllowedValues.push_back(name);
        TrackerFieldOption option;
        option.Id = id;
        option.Value = name;
        try {
            option.PayloadJson = obj.dump();
        } catch (...) {
            option.PayloadJson.clear();
        }
        field.AllowedValueOptions.push_back(std::move(option));
    }
    field.Family = ClassifyTrackerFieldFamily(field);
}

namespace {

// Merge a single createmeta issue-type's per-field `allowedValues` into the
// catalog (and harvest `components` allowedValues). Factored out of
// ApplyCreateMetaToCatalog to keep each function within the branch budget.
void MergeCreateMetaIssueTypeFields(const nlohmann::json& fieldsObj,
                                    const std::unordered_map<std::string, std::size_t>& fieldIndexById,
                                    std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components,
                                    std::set<std::string>& uniqueComponentIds) {
    for (auto it = fieldsObj.begin(); it != fieldsObj.end(); ++it) {
        const std::string fieldId = it.key();
        const auto& fieldObj = it.value();

        if (fieldId == "components" && fieldObj.contains("allowedValues") && fieldObj["allowedValues"].is_array()) {
            for (const auto& val : fieldObj["allowedValues"]) {
                if (val.contains("id") && val.contains("name") && val["id"].is_string() && val["name"].is_string()) {
                    const std::string componentId = val["id"].get<std::string>();
                    if (uniqueComponentIds.insert(componentId).second) {
                        TrackerComponent component;
                        component.Id = componentId;
                        component.Name = val["name"].get<std::string>();
                        components.push_back(component);
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

        TrackerField& targetField = fields[indexIt->second];
        for (const auto& val : fieldObj["allowedValues"]) {
            TrackerFieldOption option = TrackerFieldOptionFromJson(val);
            if (!option.Value.empty() || !option.Id.empty()) {
                MergeTrackerFieldOption(targetField.AllowedValueOptions, option);
            }
        }
        RefreshTrackerAllowedValuesFromOptions(targetField);
        targetField.Family = ClassifyTrackerFieldFamily(targetField);
    }
}

// Build a TrackerIssueTypeCreateMeta from one createmeta issuetype node
// (id/name/subtask + required-field ids from the `fields` schema map).
TrackerIssueTypeCreateMeta BuildIssueTypeCreateMetaEntry(const nlohmann::json& issueType,
                                                         const std::string& projectKeyForIssueType) {
    TrackerIssueTypeCreateMeta metaEntry;
    metaEntry.ProjectKey = projectKeyForIssueType;
    metaEntry.IsSubtask = issueType.value("subtask", false);
    metaEntry.IssueTypeId = JsonIdToDecimalString(issueType);
    metaEntry.IssueTypeName = issueType.value("name", std::string());

    if (issueType.contains("fields") && issueType["fields"].is_object()) {
        // Pass 1: collect per-screen "required" flags from createmeta field schemas.
        for (auto it = issueType["fields"].begin(); it != issueType["fields"].end(); ++it) {
            if (it.value().is_object() && it.value().value("required", false)) {
                metaEntry.RequiredFieldIds.insert(it.key());
            }
        }
    }
    return metaEntry;
}

} // namespace

void ApplyCreateMetaToCatalog(const nlohmann::json& metaJson, const std::string& projectKey,
                              const std::unordered_map<std::string, std::size_t>& fieldIndexById,
                              std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components,
                              std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta,
                              std::set<std::string>& outUniqueIssueTypes) {
    if (!metaJson.contains("projects") || !metaJson["projects"].is_array()) {
        return;
    }

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

    std::set<std::string> uniqueComponentIds;
    for (const auto& project : metaJson["projects"]) {
        const std::string projectKeyForIssueType = project.value("key", projectKey);
        if (!project.contains("issuetypes") || !project["issuetypes"].is_array()) {
            continue;
        }

        for (const auto& issueType : project["issuetypes"]) {
            if (issueType.contains("name") && issueType["name"].is_string()) {
                outUniqueIssueTypes.insert(issueType["name"].get<std::string>());
            }

            TrackerIssueTypeCreateMeta metaEntry = BuildIssueTypeCreateMetaEntry(issueType, projectKeyForIssueType);

            if (issueType.contains("fields") && issueType["fields"].is_object()) {
                MergeCreateMetaIssueTypeFields(issueType["fields"], fieldIndexById, fields, components,
                                               uniqueComponentIds);
            }

            upsertIssueTypeMeta(std::move(metaEntry));
        }
    }
}

bool BuildIssueTypeOptionsFromProjectJson(const nlohmann::json& projectJson,
                                          std::vector<TrackerFieldOption>& outOptions) {
    outOptions.clear();
    if (!projectJson.contains("issueTypes") || !projectJson["issueTypes"].is_array()) {
        return false;
    }
    for (const auto& it : projectJson["issueTypes"]) {
        if (!it.is_object()) {
            continue;
        }
        const std::string tid = JsonIdToDecimalString(it);
        const std::string tname = it.value("name", std::string());
        if (tid.empty() || tname.empty()) {
            continue;
        }
        TrackerFieldOption option;
        option.Id = tid;
        option.Value = tname;
        try {
            option.PayloadJson = it.dump();
        } catch (...) {
            option.PayloadJson.clear();
        }
        outOptions.push_back(std::move(option));
    }
    if (outOptions.empty()) {
        return false;
    }
    std::sort(outOptions.begin(), outOptions.end(),
              [](const TrackerFieldOption& a, const TrackerFieldOption& b) { return a.Value < b.Value; });
    return true;
}

bool ExtractSprintBoardIdsFromPage(const nlohmann::json& boardsJson, std::set<int>& seenBoardIds,
                                   std::vector<int>& outBoardIds) {
    if (!boardsJson.is_object() || !boardsJson.contains("values") || !boardsJson["values"].is_array()) {
        return true;
    }
    const auto& values = boardsJson["values"];
    for (const auto& board : values) {
        if (!board.is_object() || !board.contains("id")) {
            continue;
        }
        const int bid = ParseJsonIntLoose(board["id"], -1);
        if (bid > 0 && seenBoardIds.insert(bid).second) {
            outBoardIds.push_back(bid);
        }
    }
    return boardsJson.value("isLast", false) || values.empty();
}

void CollectSprintOptionsFromPage(const nlohmann::json& sprintJson, std::set<std::string>& seenSprintIds,
                                  std::vector<TrackerFieldOption>& outOptions) {
    if (!sprintJson.is_object() || !sprintJson.contains("values") || !sprintJson["values"].is_array()) {
        return;
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
        outOptions.push_back(std::move(opt));
    }
}

} // namespace TrackerFieldCatalogPure
