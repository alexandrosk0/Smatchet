#include "TrackerFieldCatalogPure.h"

#include "TrackerFieldValueParser.h"

#include "StringUtil.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
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
                                                 : nlohmann::json::parse(prior.PayloadJson, nullptr, false);
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

} // namespace TrackerFieldCatalogPure
