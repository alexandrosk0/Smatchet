#pragma once

#include "AppController.h"
#include "ConfigManager.h"

#include <nlohmann/json.hpp>
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "IssueTableSerializer.h"
#include "JiraGridFieldDisplay.h"
#include "NavigationHistory.h"
#include "SpreadsheetState.h"
#include "TrackerFieldSchema.h"

#include "imgui.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct AttachmentCollectionRequest {
    std::vector<AppController::AttachmentDescriptor> Attachments;
};

struct AttachmentPreviewUpdate {
    std::string LocalPath;
    std::string MimeType;
    std::string Filename;
    std::string Url;
};

struct AttachmentWindowEntry {
    std::string Filename;
    std::string Url;
    std::string MimeType;
    std::string LocalPath;
    int ImageWidth = 0;
    int ImageHeight = 0;
    std::string PreviewError;
    bool PreviewRequestIssued = false;
    ImTextureData* ThumbnailTextureData = nullptr;
};

struct PendingFieldEdit {
    std::string IssueId;
    TrackerField Field;
    std::vector<std::string> Values;
};

struct FieldEditCommitResult {
    enum class Kind { Failed, SavedOnline, QueuedOffline };
    Kind CommitKind = Kind::Failed;
    bool Ok = false;
    std::string Error;
    AppController::FieldEditResult ApplyResult;
    /** When CommitKind == QueuedOffline: JSON object map for `UpdateIssueFields`. */
    std::string QueuedFieldsPayloadJson;
};

struct FieldCatalogFetchResult {
    bool Ok = false;
    std::string BackendKey;
    std::vector<TrackerField> Fields;
    std::vector<TrackerComponent> Components;
    std::vector<TrackerIssueTypeCreateMeta> IssueTypeMeta;
    std::vector<TrackerUser> Users;
    std::string Error;
    std::string Warning;
};

/** Worker-built snapshot for Backend Audit window (applied on UI thread). */
struct AuditDisplayCachePayload {
    std::vector<nlohmann::json> Events;
    std::vector<std::string> FullJson;
    std::vector<std::string> DataJson;
    std::vector<std::string> SearchLower;
    std::string ReadError;
};

enum class CellWriteState { Saving, Success, Error };

struct CellWriteFeedback {
    CellWriteState State = CellWriteState::Saving;
    std::string Message;
    int FramesRemaining = 0;
};

struct UiDrawSession {
    bool cfgInitialized = false;
    JiraConfig cfg;

    bool showPreferences = false;
    bool showViewsDashboard = true;
    bool requestViewsDashboardFocus = false;
    bool showPerformance = false;
    bool showBlameAnalysis = false;
    bool showBulkImport = false;
    bool showBulkExport = false;
    bool showAuditTrail = false;
    bool requestAuditTrailFocus = false;
    /** When false, the Log window is hidden (dock tab X sets this; reopen from Settings). */
    bool showLogWindow = true;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    /** Lua console / automation panel; dock tab X clears this; reopen from Settings. */
    bool showLuaAutomationWindow = true;
    bool requestLuaAutomationFocus = false;
    /** When true, the next draw will switch to the "Scripting" tab in the Lua window. */
    bool requestScriptsWindowFocus = false;
#endif

#if defined(SMATCHET_WITH_MCP)
    /** MCP status / endpoints / activity; reopen from Scripts (or MCP menu when Lua is off). */
    bool showMcpServerWindow = false;
    bool requestMcpServerFocus = false;
#endif

    bool fieldCatalogFetchStarted = false;
    bool fieldCatalogLoading = false;
    std::future<FieldCatalogFetchResult> fieldCatalogFuture;
    bool triggerCatalogRefetch = false;
    std::string fieldCatalogWarning;

    bool appliedInitialView = false;
    /** Last `ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType)` applied to views + initial sync. */
    std::string lastViewsBackendKey;
    /** First JQL fetch runs async so the UI thread is not blocked for multi-second Jira searches. */
    bool initialTicketSyncStarted = false;
    bool initialTicketSyncLoading = false;
    std::future<TrackerIssueFetchPack> initialTicketSyncFuture;

    /** Jira connectivity recovery: refetch issues for current view without blocking the UI thread. */
    bool connectivityRecoveryTicketResyncPending = false;
    bool connectivityRecoveryTicketFetchLoading = false;
    std::future<TrackerIssueFetchPack> connectivityRecoveryTicketFetchFuture;

    NavigationHistory navHistory;

    char domainBuf[128]{};
    char emailBuf[128]{};
    char tokenBuf[512]{};
    char projectKeyBuf[64]{};
    char trackerTypeBuf[32]{};
    char planeUrlBuf[256]{};
    char planeWorkspaceBuf[128]{};
    char planeProjectBuf[128]{};
    char planeApiKeyBuf[512]{};
    char newIssueInheritFieldsBuf[512]{};
    char newIssueInheritFieldsPlaneBuf[512]{};
    char aiApiKeyBuf[512]{};

    char aiModelBuf[128]{};
    char aiBaseUrlBuf[256]{};
    bool mcpEnabled = false;
    int mcpPort = 8080;
    bool mcpAllowRemote = false;
    char mcpAuthTokenBuf[512]{};
    bool preferencesBuffersLoaded = false;
    /** Integrations tab: brief "saved" line after MCP fields persist. */
    std::chrono::steady_clock::time_point mcpPrefsSavedHintUntil{};

    char viewNameBuf[128]{};
    char viewJqlBuf[512]{};
    /** Last cursor/selection from JQL InputText callback (for autocomplete). */
    int jqlAcpLastCursor = 0;
    int jqlAcpLastSelectionStart = 0;
    int jqlAcpLastSelectionEnd = 0;
    /** Pending replace from autocomplete (applied inside InputText callback). */
    int jqlAcpReplaceStart = -1;
    int jqlAcpReplaceEnd = -1;
    std::string jqlAcpReplaceText;
    bool jqlAcpApplyReplace = false;
    int jqlAcpListSelected = 0;
    /** Set after JQL apply from popup; next frame: refocus JQL so typing continues. */
    bool jqlAcpWantsJqlInputFocus = false;
    bool jqlAcpScrollToSelected = false;
    /** After mouse autocomplete flush: move caret here (-1 = ignore). Applied in InputText CallbackAlways. */
    int jqlAcpWantsCursorPos = -1;
    /** QueueJqlApplyFromBuild from list click; flush sets jqlAcpWantsCursorPos from replace range + insert. */
    bool jqlAcpPendingMouseCaretAfterPick = false;
    /** Set from JQL InputText callback when Enter pressed with no autocomplete rows; UI runs Apply JQL. */
    bool jqlWantsApplyFromEnter = false;

    char selectedFieldsBuf[1024]{};
    char fieldSearchBuf[128]{};
    char auditSearchBuf[256]{};
    int auditActionFilter = 0; // 0 all, 1 creates, 2 updates/transitions, 3 comments, 4 attachments, 5 offline
    int auditResultFilter = 0; // 0 all, 1 success, 2 failure
    bool auditNewestFirst = true;
    int auditPage = 0;
    int auditRowsPerPage = 100;
    /** Filled when Backend Audit window reloads from disk (throttled — not every frame). */
    std::vector<nlohmann::json> auditCachedEvents;
    std::vector<std::string> auditCachedFullJson;
    std::vector<std::string> auditCachedDataJson;
    std::vector<std::string> auditCachedSearchLower;
    std::string auditCachedReadError;
    std::chrono::steady_clock::time_point auditLastFilePoll{};
    /** False while closed; opening the window forces one immediate audit file poll. */
    bool auditTrailWindowWasOpen = false;
    bool auditReloadInFlight = false;
    bool auditReloadPending = false;
    std::future<AuditDisplayCachePayload> auditReloadFuture;
    std::vector<std::string> editingColumnOrder;
    int selectedColumnOrderIndex = -1;
    std::string editingViewId;
    std::vector<std::string> lastSyncedColumnOrder;

    SpreadsheetState gridState;
    std::string gridEditError;
    std::string gridEditSuccess;
    std::deque<PendingFieldEdit> queuedFieldEdits;
    bool hasInFlightEdit = false;
    PendingFieldEdit inFlightEdit;
    std::string inFlightOriginalEstimateSnapshot;
    std::string inFlightRemainingEstimateSnapshot;
    bool inFlightCommitStarted = false;
    std::future<FieldEditCommitResult> inFlightCommitFuture;
    std::string inFlightIssueTypeKeySnapshot;
    int inFlightDelayFrames = 0;
    std::unordered_map<std::string, CellWriteFeedback> cellFeedbackByKey;

    char gridFilterBuf[128]{};
    std::string lastGridActiveViewId;
    std::string lastGridContextSignature;
    bool newIssueDiscardAsyncCreateResult = false;
    std::vector<size_t> cachedSortedIndices;
    std::vector<size_t> filteredIndices;
    std::string cachedSortFingerprint;
    std::uint64_t cachedSortTicketsRevision = 0;
    std::uint64_t cachedSortCatalogRevision = 0;
    bool cachedSortValid = false;

    int gridBottomHorizontalWheelSwallowsRemaining = 0;
    int gridTopHorizontalWheelSwallowsRemaining = 0;

    std::string aiResponse;
    bool aiIsThinking = false;
    bool aiPromptPending = false;
    std::string aiPromptMessage;

    std::vector<char> logBuffer;
    std::uint64_t lastSeenLogRevision = 0;
    bool pendingViewStateSave = false;
    std::chrono::steady_clock::time_point pendingViewStateSaveAt{};

    JiraGridFieldAsyncState jiraGridAsync;

    bool newIssueDraftActive = false;
    /** After "+ New issue", scroll table once so Create/Queue/Cancel stay in view. */
    bool newIssueScrollDraftRowIntoViewPending = false;
    IssueDraft newIssueDraft;
    std::unordered_map<std::string, std::vector<char>> newIssueDraftEditBufs;
    std::future<IssueCreateResult> newIssueCreateFuture;
    bool newIssueCreateInFlight = false;
    std::vector<std::string> newIssueMissingFieldIds;
    bool newIssueQueueFallbackVisible = false;
    std::string newIssueQueueFallbackError;

    std::vector<char> bulkImportTextBuf;
    char bulkImportPathBuf[1024]{};
    int bulkImportFormatSel = 0;
    IssueTableSerializer::ImportResult bulkImportPreview;
    std::vector<std::string> bulkImportStatus;
    std::vector<std::future<IssueCreateResult>> bulkImportFutures;
    size_t bulkImportCompleted = 0;
    bool bulkImportRunning = false;
    std::string bulkImportError;
    bool bulkImportWasOpen = false;

    char bulkExportPathBuf[1024]{};
    int bulkExportFormatSel = 1;
    std::string bulkExportFeedback;

    std::mutex attachmentPreviewMutex;
    std::deque<AttachmentCollectionRequest> attachmentCollectionQueue;
    std::deque<AttachmentPreviewUpdate> attachmentPreviewUpdateQueue;
    bool attachmentPreviewCallbackRegistered = false;
    bool attachmentPreviewWindowOpen = false;
    std::vector<AttachmentWindowEntry> attachmentWindowEntries;
    int attachmentWindowSelectedIndex = 0;

    /** One-shot copy of startup legacy pending-drop banner from AppController. */
    bool offlineLegacyStartupBannerConsumed = false;
    std::string offlineLegacyStartupBannerText;
    std::string deadLetterPanelStatus;
    bool deadLetterPanelStatusHasClearDeadline = false;
    std::chrono::steady_clock::time_point deadLetterPanelStatusClearAt{};
    std::string offlineQueuePanelStatus;
    bool offlineQueuePanelStatusHasClearDeadline = false;
    std::chrono::steady_clock::time_point offlineQueuePanelStatusClearAt{};

    ~UiDrawSession();
};

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
extern UiDrawSession g_ui;
#endif

/** Blocking join for audit file reload worker (no AppController capture). */
void DrainAuditReloadFuture(UiDrawSession& d);
