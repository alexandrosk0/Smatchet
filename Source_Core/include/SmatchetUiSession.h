#pragma once

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetDefaults.h"

#include <nlohmann/json.hpp>
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "IssueTableSerializer.h"
#include "TrackerGridFieldDisplay.h"
#include "NavigationHistory.h"
#include "SpreadsheetState.h"
#include "TrackerFieldSchema.h"
#include "QuerySuggestTypes.h"
#include "SmatchetProjectPicker.h"

#include "imgui.h"

#include <array>
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
    /// When true, `Values[0]` is already in the backend's target format (ADF JSON / HTML /
    /// final string) and the payload layer must NOT run MarkdownConvert on it. Set by the
    /// long-text modal editor's raw-mode path when it can't safely round-trip through Markdown.
    /// See RICH_TEXT_EDITING_V2_PLAN.md.
    bool Preformatted = false;
    /// Original rich payload (ADF JSON or HTML) at the time the user opened the editor.
    /// Persisted alongside the offline queue entry so replay can perform a real 3-way merge:
    ///   base = OriginalRichValue, mine = queued payload, theirs = current server content.
    /// Empty for non-ADF/HTML fields and for edits made before this field was introduced.
    std::string OriginalRichValue;
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
    TrackerConfig cfg;

    /// Merge-conflict resolution modal for offline field edits. See RICH_TEXT_EDITING_V2_PLAN.md PR-F.
    bool showConflictResolveModal = false;
    std::int64_t conflictResolveDbId = 0; ///< DB id of the pending_field_edit with the conflict.
    std::string conflictContextJson;      ///< JSON blob: {base,mine,theirs,richKind}
    /// Editor buffer for the "resolved" pane in the conflict modal (~64 KB, lazy-allocated).
    std::vector<char> conflictResolveBuf;

    /// Inline Command Palette input field rendered in the main menu-bar strip.
    /// Mirrors VS Code Quick Input — typing pre-fills the existing palette modal.
    char paletteInlineBuf[256] = {};

    bool showPreferences = false;
    bool showViewsDashboard = true;
    bool requestActiveProjectFocus = false;
    bool requestViewsDashboardFocus = false;
    bool showPerformance = false;
    bool showBlameAnalysis = false;
    /** 0 = Grid tab, 1 = Annotate tab. */
    int activeGridTab = 0;
    /** Set true to force-select activeGridTab via ImGuiTabItemFlags_SetSelected next frame. */
    bool activeGridTabForcePending = false;
    /** When false, the Annotate tab is not rendered (tab bar shows Grid only). Open paths
     *  set this to true; Close button in Annotate sets it false. */
    bool annotateTabVisible = false;
    bool showBulkImport = false;
    bool showBulkExport = false;
    bool showAuditTrail = false;
    bool requestAuditTrailFocus = false;
    /** When false, the Log window is hidden (dock tab X sets this; reopen from Settings). */
    bool showLogWindow = true;
    int layoutForceDefaultsFrames = 0;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    /** Scripting window; dock tab close clears this; reopen from Automation -> Scripts & Actions.... */
    bool showLuaAutomationWindow = true;
    bool requestLuaAutomationFocus = false;
    /** When true, the next draw selects the Scripts editor tab (not Tools & Actions). */
    bool requestScriptingEditorTabFocus = false;
#endif

#if defined(SMATCHET_WITH_MCP)
    /** MCP status / endpoints / activity; reopen from Automation -> Agent Bridge (MCP).... */
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
    // PR 6: projectKeyBuf / planeProjectBuf removed — project is per-operation, not a saved
    // preference. The Preferences "Recently used projects" listbox reads
    // FieldCatalogCache::ListCachedProjects() directly (no edit buffer needed).
    char trackerTypeBuf[32]{};
    char planeUrlBuf[256]{};
    char planeWorkspaceBuf[128]{};
    char planeApiKeyBuf[512]{};
    char newIssueInheritFieldsBuf[512]{};
    char newIssueInheritFieldsPlaneBuf[512]{};
    bool mcpEnabled = false;
    int mcpPort = SmatchetDefaults::Mcp::kDefaultPort;
    bool mcpAllowRemote = false;
    bool mcpAllowLuaExecution = false;
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
    /** If >= 0, override the post-insert cursor to this many bytes INTO the replace text
     *  (used by JQL function suggestions like `membersOf("…")` so the caret lands between
     *  the parens). -1 = default behaviour (cursor at end of inserted text). */
    int jqlAcpReplaceCaretOffset = -1;
    int jqlAcpListSelected = -1;
    /** Set after JQL apply from popup; next frame: refocus JQL so typing continues. */
    bool jqlAcpWantsJqlInputFocus = false;
    bool jqlAcpScrollToSelected = false;
    /** After inline apply: force cursor to BufTextLen for N more CallbackAlways passes so ImGui scrolls to end. */
    int jqlAcpCaretSnapFramesRemaining = 0;
    /** Set from JQL InputText callback when Enter pressed with no autocomplete rows; UI runs Apply JQL. */
    bool jqlWantsApplyFromEnter = false;
    /** Esc closed suggestion list until focus leaves input. */
    bool jqlAcpListDismissed = false;
    /** Debounced Jira user search (hybrid async on main thread). */
    uint64_t jqlAcpUserSearchRequestId = 0;
    double jqlAcpUserSearchFireAt = 0.0;
    uint64_t jqlAcpUserSearchArmedId = 0;
    std::string jqlAcpUserSearchQuery;
    std::vector<QuerySuggestion> jqlAcpAsyncUserItems;
    std::string jqlAcpAsyncUserError;

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
    int scrollColumnOrderToIndex = -1;
    std::string editingViewId;
    std::vector<std::string> lastSyncedColumnOrder;

    // Modern two-pane Views editor — sidebar + tab state, dirty tracking, drag/keyboard reorder cursor.
    // 0 = Filter, 1 = Fields, 2 = Columns, 3 = Sort (matches ViewsEditorTab enum in the Views UI .cpp).
    int viewsActiveTab = 0;
    bool viewsDirty = false;
    int viewsKeyboardReorderRow = -1;
    // Sidebar search filter for the saved-views list.
    char viewsSidebarSearchBuf[128]{};
    // Modal state: discard-changes confirm when switching views, delete-view confirm,
    // and the "rename inline" buffer for the editor title.
    bool viewsShowDiscardConfirm = false;
    std::string viewsPendingActivateId;
    bool viewsShowDeleteConfirm = false;
    bool viewsTitleEditing = false;
    // Snapshot of the active view at the moment viewsDirty transitioned false -> true.
    // Used by the unsaved-layout strip's Discard button to revert widths / sort specs /
    // column order / name / JQL / fields back to the on-disk state. Captured lazily.
    bool viewsHasOriginalSnapshot = false;
    ViewDefinition viewsOriginalSnapshot;

    SpreadsheetState gridState;
    std::string gridEditError;
    std::string gridEditSuccess;
    /** Edge-trigger dedupe for tracker connectivity toasts (Active Project window). */
    TrackerConnectivityBannerForUi::Level lastToastedTrackerBannerKind = TrackerConnectivityBannerForUi::Level::None;
    std::string lastToastedTrackerBannerMessage;
    /** Suppress spurious offline toasts while probe is cold or live OK but banner text not yet cleared. */
    bool trackerWarningToastStartupGateInitialized = false;
    std::chrono::steady_clock::time_point trackerWarningToastStartupGraceUntil{};
    /** Green "TRACKER OK" chip: hide after `trackerOkChipHideAt` while banner clear and reachable. */
    bool trackerOkChipHideTimerArmed = false;
    std::chrono::steady_clock::time_point trackerOkChipHideAt{};
#if defined(SMATCHET_WITH_MCP)
    /** Grid header "MCP LIVE" chip: anchor for initial visibility window after enable. */
    std::chrono::steady_clock::time_point mcpLiveHeaderAnchorAt{};
    bool mcpLiveHeaderAnchorArmed = false;
    bool mcpLiveHeaderLastCfgEnabled = false;
    /** Previous-frame MCP header chip drawn (enabled or disabled) so hide always runs a fade. */
    bool mcpHeaderLastFrameChipShown = false;
    bool mcpHeaderFadeoutActive = false;
    std::chrono::steady_clock::time_point mcpHeaderFadeoutStartAt{};
#endif
    /** 0 none, 1 error, 2 success — mirrors single-slot gridEdit banner before it became toast-only. */
    int lastToastedGridBannerKind = 0;
    std::string lastToastedGridBannerMessage;
    int cachedPendingFieldEditCount = 0; ///< refreshed by OfflineQueueService tick, read by status bar
    std::deque<PendingFieldEdit> queuedFieldEdits;
    bool hasInFlightEdit = false;
    PendingFieldEdit inFlightEdit;
    std::string inFlightOriginalEstimateSnapshot;
    std::string inFlightRemainingEstimateSnapshot;
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
    bool viewSortDirty = false;
    bool forceApplySortSpecs = false;
    std::chrono::steady_clock::time_point lastGridSortAt{};
    bool logAutoScroll = true;

    int gridBottomHorizontalWheelSwallowsRemaining = 0;
    int gridTopHorizontalWheelSwallowsRemaining = 0;

    bool appUpdateStartupCheckStarted = false;
    bool appUpdateCheckInFlight = false;
    bool appUpdateCheckManual = false;
    bool appUpdateModalOpen = false;
    std::future<AppUpdateInfo> appUpdateFuture;
    AppUpdateInfo appUpdateInfo;
    std::string appUpdateActionStatus;

    /** One visual row per string (embedded newlines in messages are split). */
    std::vector<std::string> logViewLines;
    std::uint64_t lastSeenLogRevision = 0;
    /** After log refresh: scroll to bottom until layout reports a real scroll range. */
    bool logScrollToTailPending = false;
    std::uint64_t logScrollTailGiveUpFrame = 0;
    /** User scrolled up to read history; stay off tail until they scroll back to bottom or toggle auto-scroll. */
    bool logTailReleasedByUser = false;
    bool pendingViewStateSave = false;
    std::chrono::steady_clock::time_point pendingViewStateSaveAt{};

    TrackerGridFieldAsyncState trackerGridAsync;

    bool newIssueDraftActive = false;
    /** After "+ New issue", scroll table once so Create/Queue/Cancel stay in view. */
    bool newIssueScrollDraftRowIntoViewPending = false;
    IssueDraft newIssueDraft;
    std::unordered_map<std::string, std::vector<char>> newIssueDraftEditBufs;
    /** Per-field quick-filter buffers for new-issue draft single-select combos. Indexed by
     *  fieldId; cleared whenever `newIssueDraftEditBufs` resets. Mirrors the grid editor's
     *  `SpreadsheetState::SingleSelectSearchBuf` pattern. */
    std::unordered_map<std::string, std::array<char, 128>> newIssueDraftComboSearchBufs;
    /** Last fieldId whose combo was open — when it changes, the new fieldId's search buffer
     *  is zeroed so the user starts on an empty filter when moving between dropdowns. */
    std::string newIssueDraftComboSearchActiveField;
    std::future<IssueCreateResult> newIssueCreateFuture;
    bool newIssueCreateInFlight = false;
    std::vector<std::string> newIssueMissingFieldIds;
    bool newIssueQueueFallbackVisible = false;
    std::string newIssueQueueFallbackError;
    // PR 3: when the draft's ProjectKey diverges from this guard, the new-issue draft UI kicks an
    // async RefreshFieldCatalog so per-project required-fields/create-meta land before submit.
    std::string newIssueDraftLastFetchedProjectKey;

    // PR 4b: project-picker state for the new-issue draft combo and the bulk-import modal.
    // Each has its own State because the lazy "All projects" fetch caches per call site.
    SmatchetProjectPicker::State newIssueProjectPickerState;
    SmatchetProjectPicker::State bulkImportProjectPickerState;
    // Bulk-import modal lifecycle: open=true while the modal is up; pendingChosenKey latches the
    // user's pick so the "Use this project" path can flow into ParseDrafts on confirm.
    bool bulkImportProjectModalOpen = false;
    std::string bulkImportProjectModalChosenKey;

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

    // ----- Scenario subsystem (see docs/design/applied/command-system-plan.md §Scenario) -----
    /// When true the active scenario drives the scroll position this frame.
    /// Written by ScenarioRunner::Tick; read by SmatchetActiveProjectGridUi.
    bool scenarioScrollActive = false;
    /// Pixel Y to set via ImGui::SetScrollY each frame while scenarioScrollActive=true.
    /// -1 = inactive (redundant guard; check scenarioScrollActive).
    int scenarioScrollTarget = -1;

    /// Transient flag — standalone target toggles OS fullscreen; Unreal target ignores.
    bool requestFullScreenToggle = false;

    /// Transient request from `debug.window.resize` — standalone polls this each frame and,
    /// when set, calls glfwSetWindowSize(window, requestWindowWidth, requestWindowHeight).
    /// Lets automated visual tests drive deterministic window dimensions over the MCP CLI.
    bool requestWindowResize = false;
    int requestWindowWidth = 0;
    int requestWindowHeight = 0;

    /// Transient request from `debug.window.screenshot` — standalone polls this each frame and,
    /// when set, reads the GL framebuffer and writes a PNG/PPM to requestScreenshotPath.
    /// Used by the visual-test pipeline to snapshot dock layouts deterministically.
    bool requestScreenshot = false;
    std::string requestScreenshotPath;

    /// Transient request consumed once per frame in `SmatchetUI::Draw` right before the
    /// command-palette draw call. Lets the bucket-C `CommandPaletteFuzzyScenario` drive
    /// the palette open/filter state without reaching into `SmatchetUI`'s private
    /// `commandPalette_` member. Cleared by the consumer the moment the flag is honoured.
    bool requestCommandPaletteOpen = false;
    /// Filter substring applied via `CommandPaletteUi::SetFilterText` when
    /// `requestCommandPaletteOpen` is consumed. Empty means "open with no filter".
    std::string requestCommandPaletteFilter;

    /// When true, render the dock-node debug overlay (toggled by Ctrl+Alt+D).
    bool showDockDebug = false;

    ~UiDrawSession();
};

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
extern UiDrawSession g_ui;
#endif

/** Blocking join for audit file reload worker (no AppController capture). */
void DrainAuditReloadFuture(UiDrawSession& d);
