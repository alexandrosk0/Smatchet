#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

// 1. MUST BE INCLUDED FIRST FOR GCC 13+ COMPATIBILITY
#include <limits>
#include <cstdint>

// 2. NOW WE CAN INCLUDE SOL
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// 3. THE REST OF YOUR INCLUDES
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <unordered_map>
#include "LocalCacheManager.h"
#include "ITrackerClient.h"
#include "JiraClient.h"

class AppController {
public:
    void Initialize(const std::string& dbPath, const std::string& backendType);

    /** Call from plugins in OnEarlyInit only (before Initialize completes InitLua). */
    void AddAutomationLogSink(std::function<void(const std::string&)> sink);

    /**
     * Optional host callback for launching URLs.
     * Use this when embedding in Unreal (avoids OS-level `system("start")` calls).
     */
    void SetOpenUrlHandler(std::function<void(const std::string&)> handler);

    /** Opens a URL using the handler if set; otherwise falls back to default browser. */
    void OpenUrl(const std::string& url) const;

    /**
     * Optional host callback for showing Jira attachments inside Unreal.
     * If set, Smatchet will download the attachment bytes and save them to a local temp file,
     * then call this handler with the file path.
     */
    using AttachmentViewerHandler = std::function<void(const std::string& localPath,
                                                        const std::string& mimeType,
                                                        const std::string& filename)>;
    void SetAttachmentViewerHandler(AttachmentViewerHandler handler);

    /**
     * Open an attachment (image/pdf/etc) without requiring Basic Auth headers in the browser.
     * - If AttachmentViewerHandler is set: downloads to a temp file and calls the handler.
     * - Otherwise: falls back to OpenUrl(url) (may not work for attachments in embedded browsers).
     */
    void OpenAttachment(const std::string& url,
                         const std::string& filename,
                         const std::string& mimeType);

    void InitLua();

    void RunAutoScript(const std::string& scriptPath);

    /** Run a Lua file once (e.g. FieldDisplay.lua) to register UI hooks; errors go to automation log sinks. */
    void RunLuaSetupScript(const std::string& scriptPath);

    /**
     * If a Lua handler was registered for fieldId, invoke it to draw the grid cell.
     * @return true if the handler ran and returned a Lua-truthy value (cell fully handled).
     */
    bool TryLuaFieldDisplay(const std::string& fieldId,
                            const CachedTicket& ticket,
                            const std::string& rawValue,
                            float availWidth,
                            const JiraField* fieldMeta);

    /**
     * Sync issues from the tracker into the local cache.
     * Pass the in-memory UI config + views store when syncing from the app so JQL/fields match
     * the active view without relying on an immediate disk round-trip.
     */
    void SyncWithBackend(const JiraConfig* configOverride = nullptr,
                         const ViewsStore* viewsOverride = nullptr);

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

    /** Fetches watcher users for an issue (Jira only). */
    bool FetchIssueWatchers(const std::string& issueKey,
                            std::vector<JiraUser>& outWatchers,
                            std::string& outError) const;

    bool JiraSearchUsersByQuery(const std::string& query,
                                std::vector<JiraUser>& outUsers,
                                std::string& outError) const;

    bool JiraAddIssueCommentPlain(const std::string& issueKey,
                                  const std::string& plainText,
                                  std::string& outError);

    bool JiraAddIssueCommentBlameContext(const std::string& issueKey,
                                         const std::string& p4User,
                                         const std::string& functionName,
                                         const std::string& filePath,
                                         int lineNumber,
                                         const std::string& changelist,
                                         const std::string& date,
                                         bool approximated,
                                         const std::string& codeSnippet,
                                         std::string& outError);

    bool JiraFetchUserGroupNames(const std::string& accountId,
                                 std::vector<std::string>& outGroupNames,
                                 std::string& outError) const;

private:
    std::unique_ptr<LocalCacheManager> Cache;
    std::unique_ptr<ITrackerClient> Backend;
    JiraClient* JiraBackend = nullptr;
    std::vector<CachedTicket> ActiveTickets;
    std::vector<JiraField> AvailableJiraFields;
    std::vector<JiraComponent> AvailableJiraComponents;
    std::string LastJiraFieldCatalogError;
    sol::state lua;
    std::vector<std::function<void(const std::string&)>> AutomationLogSinks;
    std::function<void(const std::string&)> OpenUrlHandler;
    AttachmentViewerHandler AttachmentViewerHandlerCallback;
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlers_;
    /** Lowercased Jira field display name (from catalog) -> handler. */
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlersByDisplayName_;
    /** Absolute path to the `Scripts` folder (trailing slash), or empty to use `Scripts/` relative to cwd. */
    std::string luaScriptsDirectory_;

    std::string ResolveLuaScriptPath(const std::string& filename) const;
};


#endif