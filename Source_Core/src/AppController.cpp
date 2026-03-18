#include "AppController.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

void AppController::Initialize(const std::string& dbPath, const std::string& backendType) {
    Cache = std::unique_ptr<LocalCacheManager>(new LocalCacheManager(dbPath));

    if (backendType == "Jira") {
        Backend = std::unique_ptr<ITrackerClient>(new JiraClient());
        JiraBackend = dynamic_cast<JiraClient*>(Backend.get());
    }

    // Defer SyncWithBackend to first SmatchetUI::Draw so active view JQL/fields are
    // applied first — avoids fetching issues twice at startup.
    RefreshLocalData();

    InitLua();
}

void AppController::InitLua() {
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table);

    // Bind the CachedTicket struct
    lua.new_usertype<CachedTicket>("Ticket",
        "id", &CachedTicket::id,
        "get_field", &CachedTicket::GetFieldValue
    );

    lua.set_function("log_info", [](std::string msg) {
        std::printf("[LUA] %s\n", msg.c_str());
    });
}

void AppController::RunAutoScript(const std::string& scriptPath) {
    for (auto& ticket : ActiveTickets) {
        lua["ticket"] = &ticket;
        lua.script_file(scriptPath);
    }
}

void AppController::SyncWithBackend() {
    if (Backend && Cache) {
        auto freshTickets = Backend->FetchIssues();
        for (const auto& t : freshTickets) {
            Cache->SaveTicket(t);
        }
    }
    RefreshLocalData();
}

void AppController::RefreshLocalData() {
    if (Cache) {
        ActiveTickets = Cache->GetAllTickets();
    }
}

void AppController::UpdateTicket(const CachedTicket& ticket) {
    if (Cache) {
        Cache->SaveTicket(ticket);
        RefreshLocalData(); // Push changes back to ActiveTickets vector
    }
}

bool AppController::RefreshJiraFieldCatalog(const JiraConfig& cfg) {
    if (!JiraBackend) {
        LastJiraFieldCatalogError = "Jira backend is not initialized.";
        AvailableJiraFields.clear();
        AvailableJiraComponents.clear();
        return false;
    }

    std::vector<JiraField> fetchedFields;
    std::vector<JiraComponent> fetchedComponents;
    std::string error;
    const bool ok = JiraBackend->FetchFieldCatalog(cfg, fetchedFields, fetchedComponents, error);
    if (!ok) {
        LastJiraFieldCatalogError = error;
        AvailableJiraFields.clear();
        AvailableJiraComponents.clear();
        return false;
    }

    AvailableJiraFields = std::move(fetchedFields);
    AvailableJiraComponents = std::move(fetchedComponents);
    LastJiraFieldCatalogError.clear();
    return true;
}

void AppController::SetJiraFieldCatalog(std::vector<JiraField> fields,
                                       std::vector<JiraComponent> components,
                                       const std::string& error) {
    if (!error.empty()) {
        AvailableJiraFields.clear();
        AvailableJiraComponents.clear();
        LastJiraFieldCatalogError = error;
        return;
    }
    AvailableJiraFields = std::move(fields);
    AvailableJiraComponents = std::move(components);
    LastJiraFieldCatalogError.clear();
    for (auto& field : AvailableJiraFields) {
        if (field.Id == "comment") {
            field.ReadOnly = true;
        }
    }
    if (FindJiraFieldById("history") == nullptr) {
        JiraField historyField;
        historyField.Id = "history";
        historyField.Name = "History";
        historyField.ReadOnly = true;
        AvailableJiraFields.push_back(std::move(historyField));
    }
}

const JiraField* AppController::FindJiraFieldById(const std::string& fieldId) const {
    const auto it = std::find_if(
        AvailableJiraFields.begin(),
        AvailableJiraFields.end(),
        [&](const JiraField& field) { return field.Id == fieldId; });
    return it == AvailableJiraFields.end() ? nullptr : &(*it);
}

bool AppController::SubmitJiraFieldEdit(const std::string& issueId,
                                        const JiraField& field,
                                        const std::vector<std::string>& rawValues,
                                        std::string& outError) {
    outError.clear();
    if (!Backend || !Cache) {
        outError = "Backend or cache is not initialized.";
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    for (const auto& value : rawValues) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }

    nlohmann::json valuePayload;
    if (field.IsArray) {
        valuePayload = nlohmann::json::array();
        for (const auto& value : values) {
            if (field.IsUserType) {
                valuePayload.push_back(nlohmann::json{{"accountId", value}});
            } else if (field.ItemsType == "option" || field.ItemsType == "component" || !field.AllowedValueOptions.empty()) {
                valuePayload.push_back(nlohmann::json{{"id", value}});
            } else {
                valuePayload.push_back(value);
            }
        }
    } else {
        const std::string scalarValue = values.empty() ? std::string() : values.front();
        if (scalarValue.empty()) {
            valuePayload = nullptr;
        } else if (field.IsUserType) {
            valuePayload = nlohmann::json{{"accountId", scalarValue}};
        } else if (field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty()) {
            valuePayload = nlohmann::json{{"id", scalarValue}};
        } else {
            valuePayload = scalarValue;
        }
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload[field.Id] = valuePayload;
    if (!Backend->UpdateIssueFields(issueId, fieldsPayload, outError)) {
        return false;
    }

    // Keep local cache and in-memory model in sync with the successful Jira update.
    auto ticketIt = std::find_if(
        ActiveTickets.begin(),
        ActiveTickets.end(),
        [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (ticketIt != ActiveTickets.end()) {
        CachedTicket updatedTicket = *ticketIt;

        std::string displayValue;
        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                std::string displayPart = values[i];
                for (const auto& option : field.AllowedValueOptions) {
                    if (option.Id == values[i]) {
                        displayPart = option.Value;
                        break;
                    }
                }
                if (i != 0) {
                    displayValue += ", ";
                }
                displayValue += displayPart;
            }
        }

        updatedTicket.fieldValues[field.Id] = displayValue;
        UpdateTicket(updatedTicket);
    } else {
        RefreshLocalData();
    }

    return true;
}

