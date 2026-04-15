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
#include <mutex>
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
    using AttachmentPreviewHandler = std::function<bool(const std::string& localPath,
                                                        const std::string& mimeType,
                                                        const std::string& filename,
                                                        const std::string& sourceUrl)>;
    void SetAttachmentPreviewHandler(AttachmentPreviewHandler handler);
    struct AttachmentDescriptor {
        std::string Filename;
        std::string Url;
        std::string MimeType;
    };
    using AttachmentCollectionHandler = std::function<void(const std::vector<AttachmentDescriptor>& attachments)>;
    void SetAttachmentCollectionHandler(AttachmentCollectionHandler handler);
    void ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments);

    /**
     * Open an attachment (image/pdf/etc) without requiring Basic Auth headers in the browser.
     * - If AttachmentViewerHandler is set: downloads to a temp file and calls the handler.
     * - Otherwise, for image mime types: downloads and offers in-app preview handler.
     * - If no host/in-app handler path is available: falls back to OpenUrl(url).
     */
    void OpenAttachment(const std::string& url,
                         const std::string& filename,
                         const std::string& mimeType);
    bool DownloadAttachmentForPreview(const std::string& url,
                                      const std::string& filename,
                                      const std::string& mimeType,
                                      std::string* outError = nullptr);

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

    /**
     * Per-issue Jira edit metadata: true if the field may be edited for this issue.
     * Matches SubmitJiraFieldEdit: sprint fields, timetracking estimate columns, and `status` ignore
     * editmeta (Jira does not list status like a normal settable field; updates use transitions).
     * `priority`: if editmeta is loaded but omits `priority`, allow edit (Jira omits it inconsistently).
     * Returns true when editmeta is not loaded yet (optimistic) or for non-Jira backends.
     * After a failed editmeta fetch for an issue, returns false for fields not in the bypass list.
     * @param fieldMeta optional catalog row for fieldId (avoids lookup; same as nullptr + catalog).
     */
    bool CanEditJiraFieldForIssue(const std::string& issueId,
                                  const std::string& fieldId,
                                  const JiraField* fieldMeta = nullptr) const;

    bool SubmitJiraFieldEdit(const std::string& issueId,
                             const JiraField& field,
                             const std::vector<std::string>& rawValues,
                             std::string& outError);
    /** Best-effort async warmup so edit controls can reflect per-issue permissions sooner. */
    void WarmIssueEditMetaAsync(const std::string& issueId);

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
    AttachmentPreviewHandler AttachmentPreviewHandlerCallback;
    AttachmentCollectionHandler AttachmentCollectionHandlerCallback;
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlers_;
    /** Lowercased Jira field display name (from catalog) -> handler. */
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlersByDisplayName_;
    /** Absolute path to the `Scripts` folder (trailing slash), or empty to use `Scripts/` relative to cwd. */
    std::string luaScriptsDirectory_;

    struct IssueEditMetaCache {
        bool loaded = false;
        /** Field id -> Jira allows an update operation (set/add/remove). */
        std::unordered_map<std::string, bool> fieldCanEdit;
    };

    mutable std::mutex editMetaMutex_;
    std::unordered_map<std::string, IssueEditMetaCache> issueEditMeta_;
    std::unordered_map<std::string, IssueEditMetaCache> issueTypeEditMeta_;

    bool EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError = nullptr);
    bool RefreshIssueEditMeta(const std::string& issueId, std::string* outError = nullptr);
    void InvalidateIssueEditMeta(const std::string& issueId);
    void PruneEditMetaCacheToActiveTickets();
    void WarmIssueTypeEditMetaAtStartAsync();
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;
    std::string ResolveLuaScriptPath(const std::string& filename) const;
};


#endif