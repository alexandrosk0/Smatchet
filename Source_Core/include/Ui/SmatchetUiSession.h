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
#if defined(SMATCHET_WITH_AI)
#include "AiTypes.h"
#endif

#include "imgui.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    /// One-frame focus latch for the Preferences window. Set true by the menu-bar
    /// item; the window consumer calls `ImGui::SetWindowFocus()` and clears it.
    /// Drives the always-reveal-on-menu-click contract (AGENTS.md).
    bool requestPreferencesFocus = false;
    bool showViewsDashboard = true;
    bool requestActiveProjectFocus = false;
    bool requestViewsDashboardFocus = false;
    bool showPerformance = false;
    /// One-frame focus latch for the Performance window. See `requestPreferencesFocus`.
    bool requestPerformanceFocus = false;
    bool showBlameAnalysis = false;
    bool showBulkImport = false;
    /// One-frame focus latch for the Bulk Import window. See `requestPreferencesFocus`.
    bool requestBulkImportFocus = false;
    bool showBulkExport = false;
    /// One-frame focus latch for the Bulk Export window. See `requestPreferencesFocus`.
    bool requestBulkExportFocus = false;
    bool showAuditTrail = false;
    /// Plan-doc viewer window. Toggled from View → Plan docs and via
    /// `view.toggle.plan_doc_viewer`. Read-only TextEditor backed by
    /// SmatchetPlanDocViewerUi over docs/plans/active/*.md + docs/adr/*.md.
    bool showPlanDocViewer = false;
    /// One-frame focus latch for the Plan-doc viewer. See `requestPreferencesFocus`.
    bool requestPlanDocViewerFocus = false;
    bool requestAuditTrailFocus = false;
    /** When false, the Log window is hidden (dock tab X sets this; reopen from Settings). */
    bool showLogWindow = true;
    /// One-frame focus latch for the Log window. See `requestPreferencesFocus`.
    bool requestLogFocus = false;
    int layoutForceDefaultsFrames = 0;
    std::unordered_set<std::string> pendingReDockWindows;

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

#if defined(SMATCHET_WITH_AI)
    // Smatchet Assistant side panel. Hydrated from `cfg.AssistantPanelOpen` on
    // first frame and round-tripped through ConfigManager::Save on user toggles.
    // Stream/history/cancel state is per-process — never persisted. Dock side
    // (`cfg.AssistantPanelOnSecondarySide`) governs which side bar the docked
    // window attaches to; `assistantPendingSideSwap` is a one-frame latch driven
    // by the swap button so the next `ImGui::Begin` reseats the dock id.
    bool assistantPanelOpen = false;
    bool requestAssistantFocus = false;
    bool assistantPendingSideSwap = false;
    std::vector<AiMessage> assistantHistory;
    /// Parallel to `assistantHistory`: `assistantHistoryRowIds[i]` is the SQLite row
    /// id assigned to `assistantHistory[i]` by `LocalCacheManager::AppendChatMessage`.
    /// Sentinel `-1` means "append in flight; worker hasn't reported the row id back
    /// yet". Pin-toggle posts no-op on `-1` (the row isn't durable yet); on the next
    /// hydrate the message reloads with its post-restart row id. Kept parallel by
    /// every `assistantHistory.push_back(...)` site — see Phase 3.6.
    std::vector<std::int64_t> assistantHistoryRowIds;
    /// False until `HydrateFromConfigOnce` (or its async tail) finishes loading
    /// persisted history from the SQLite cache. `dispatchSend` + pin-toggle no-op
    /// when false so a Send during the hydrate window doesn't interleave a new
    /// message into the about-to-be-replaced vector.
    bool assistantHistoryHydrated = false;
    /// Pinned-bookmark scroll-to-message latch. `-1` = no pending jump; otherwise an
    /// index into `assistantHistory`. `DrawHistoryArea` reads + clears on consumption.
    /// (Phase 6 of ai-chat-claude-desktop-parity; Phase 3 only seeds the field so the
    /// dispatch_send + hydrate paths can compile against the post-Phase-6 shape.)
    int assistantScrollToMessageIndex = -1;
    /// `now < assistantCopyToastUntilMs` → render the 1-line copy-confirmation strip.
    /// Set by the Copy SmallButton; auto-dismisses on time-out. (Phase 7 surface;
    /// seeded here so the hydrate field cluster lives in one place.)
    std::int64_t assistantCopyToastUntilMs = 0;
    std::string assistantCopyToastLabel;
    std::string assistantInputBuf;
    std::string assistantStreamBuf;
    bool assistantInFlight = false;
    /// Cancel atom for the in-flight turn. UI thread sets to true via
    /// `AiAssistantController::Cancel`; worker's `WriteCallback` polls and
    /// aborts. Default-null; controller allocates a fresh atom per Submit.
    std::shared_ptr<std::atomic<bool>> assistantCancel;
    std::string assistantLastError;
    /// True while the history view is scrolled to the bottom — streaming new
    /// tokens auto-pin to tail; scrolling up releases the pin until the user
    /// scrolls back down.
    bool assistantAutoScrollAtTail = true;
    /// Monotonic per-turn id. Incremented on Send; the controller passes this
    /// value back through every MainThreadDispatcher callback so the UI thread
    /// can drop stale tokens (Cancel + immediate Send must not let the first
    /// turn's tail bytes corrupt the second turn's stream).
    std::uint64_t assistantTurnGen = 0;
    /// Per-session Model + Effort overrides (chat-window header Combos). Empty
    /// = use Preferences-saved value. Not persisted across runs — the user
    /// re-picks per session as a transient experiment. AiAssistantController
    /// reads these via the Submit() overload's modelOverride/effortOverride
    /// arguments at Send time.
    std::string assistantPerTurnModel;
    std::string assistantPerTurnEffort;

    // --- Assistant Preferences tab: Test-connection async probe state. ---
    // Mirrors Phase B's cancel-atom + MainThreadDispatcher hand-off shape. The
    // worker thread is detached; it constructs a fresh IAiClient, runs
    // ProbeReachability, and posts the result via MainThreadDispatcher with the
    // shared cancel atom captured by value. When the Preferences window closes
    // mid-probe, the UI sets `*assistantPrefsTestCancel = true` so the posted
    // callback short-circuits before touching the about-to-be-stale buffers.
    // All three fields are UI-thread-owned; the worker only reads its captured
    // shared_ptr copy of the cancel atom.
    bool assistantPrefsTestInFlight = false;
    /// Empty when no probe has run for the current edit. Re-armed to
    /// `"Testing..."` on click, then replaced with `"OK verified"` or
    /// `"FAIL <msg>"` by the posted callback. Cleared on any field edit to
    /// signal staleness.
    std::string assistantPrefsTestResult;
    /// Distinguishes display tints — Info (Testing), Success (verified), Error
    /// (failure). UI consumes this directly; toast paths bypass it.
    int assistantPrefsTestResultType = 0; // 0 info, 1 success, 2 error
    std::shared_ptr<std::atomic<bool>> assistantPrefsTestCancel;
    /// Set by the Test-connection success callback when the probe used a
    /// fallback default URL (cfg base URL was empty). The next paint of the
    /// Assistant tab reseeds the static InputText buffers from cfg so the
    /// just-persisted default value shows up in the field, then clears this
    /// flag. UI-thread-only.
    bool assistantPrefsForceBufferReseed = false;
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
    char githubBaseUrlBuf[256]{};
    char githubPatBuf[512]{};
    char githubOwnerBuf[128]{};
    char githubRepoBuf[128]{};
    char newIssueInheritFieldsBuf[512]{};
    char newIssueInheritFieldsPlaneBuf[512]{};
    char newIssueInheritFieldsGitHubBuf[512]{};
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

    /// JQL @-mention worker-thread search (Pillar 2 — finding #3). When set, the result of
    /// `SearchUsersByQuery` is pending in `jqlAcpUserSearchFuture`; polled per-frame via
    /// `wait_for(0ms)` and reduced into the items vector on completion.
    /// `jqlAcpUserSearchInFlightId` identifies which request the future belongs to so a
    /// stale completion (user typed a new query) is discarded.
    struct JqlUserSearchResult {
        bool Ok = false;
        std::vector<TrackerUser> Users;
        std::string Error;
    };
    std::future<JqlUserSearchResult> jqlAcpUserSearchFuture;
    uint64_t jqlAcpUserSearchInFlightId = 0;

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

    /// Installer-download worker state for `DrawAppUpdateModal` (Pillar 2 — finding #4).
    /// `installerDownloadInFlight` gates re-issue while the worker is running; `installerDownloadCancel`
    /// is checked by the cpr write callback so closing the modal aborts the download.
    bool installerDownloadInFlight = false;
    std::shared_ptr<std::atomic<bool>> installerDownloadCancel;

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
    /** After "+ New issue" pressed: focus the Summary InputText on next frame. Cleared
     *  after SetKeyboardFocusHere runs, or when the draft is cancelled / discarded. */
    bool newIssueFocusSummaryPending = false;
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

    // Pillar 2 — bulk-tickets file I/O deferred via worker. The button stays disabled while a
    // load/save is in flight; result is posted back through MainThreadDispatcher. Cancel atom
    // is checked by the worker so a modal close (or app shutdown) short-circuits the post-back.
    bool bulkImportLoadInFlight = false;
    std::shared_ptr<std::atomic<bool>> bulkImportLoadCancel;
    bool bulkExportSaveInFlight = false;
    std::shared_ptr<std::atomic<bool>> bulkExportSaveCancel;

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

    // ----- Scenario subsystem (see docs/plans/shipped/command-system-plan.md §Scenario) -----
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

    /// End-of-frame coalesced ConfigManager::Save. SmatchetPreferencesUi widgets
    /// flag `prefsDirty = true` (via MarkPrefsDirty) on every mutation; the
    /// debounced fire in SmatchetUI::Draw drains the dirty flag and persists
    /// once per ~100 ms window. Eliminates the per-frame Save cascade documented
    /// in docs/plans/shipped/pillar-1-2-audit-2026-05-17.md § H11 + § Pillar 1 P1.
    /// AI Assistant tab keeps its explicit Save flow (PR #181 / #184) and does
    /// NOT route through this flag.
    bool prefsDirty = false;
    std::chrono::steady_clock::time_point prefsSaveDueAt{};

    ~UiDrawSession();
};

/// Mark Preferences config as dirty and schedule a debounced ConfigManager::Save
/// at most ~100 ms later. Idempotent — repeated calls within the window keep the
/// same due-time on the first call (the latest mutation also gets persisted
/// because the Save reads the live `d.cfg` at fire time). UI-thread-only.
inline void MarkPrefsDirty(UiDrawSession& d) {
    if (!d.prefsDirty) {
        d.prefsDirty = true;
        d.prefsSaveDueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
}

extern UiDrawSession g_ui;

/** Blocking join for audit file reload worker (no AppController capture). */
void DrainAuditReloadFuture(UiDrawSession& d);
