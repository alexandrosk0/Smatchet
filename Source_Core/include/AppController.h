#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

// 1. MUST BE INCLUDED FIRST FOR GCC 13+ COMPATIBILITY
#include <limits>
#include <cstdint>

// 2. NOW WE CAN INCLUDE SOL
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// 3. THE REST OF YOUR INCLUDES
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include "LocalCacheManager.h"
#include "ITrackerClient.h"
#include "JiraClient.h"

class AppController {
public:
    void Initialize(const std::string& dbPath, const std::string& backendType);

    void InitLua();

    void RunAutoScript(const std::string& scriptPath);

    void SyncWithBackend();

    void RefreshLocalData();

    void UpdateTicket(const CachedTicket& ticket);

    const std::vector<CachedTicket>& GetActiveTickets() const { return ActiveTickets; }

    bool RefreshJiraFieldCatalog(const JiraConfig& cfg);

    const std::vector<JiraField>& GetAvailableJiraFields() const { return AvailableJiraFields; }
    const std::vector<JiraComponent>& GetAvailableJiraComponents() const { return AvailableJiraComponents; }
    const std::string& GetJiraFieldCatalogError() const { return LastJiraFieldCatalogError; }
    void SetJiraFieldCatalog(std::vector<JiraField> fields,
                             std::vector<JiraComponent> components,
                             const std::string& error);

    const JiraField* FindJiraFieldById(const std::string& fieldId) const;

    bool SubmitJiraFieldEdit(const std::string& issueId,
                             const JiraField& field,
                             const std::vector<std::string>& rawValues,
                             std::string& outError);

private:
    std::unique_ptr<LocalCacheManager> Cache;
    std::unique_ptr<ITrackerClient> Backend;
    JiraClient* JiraBackend = nullptr;
    std::vector<CachedTicket> ActiveTickets;
    std::vector<JiraField> AvailableJiraFields;
    std::vector<JiraComponent> AvailableJiraComponents;
    std::string LastJiraFieldCatalogError;
    sol::state lua;
};


#endif