#pragma once

#include "AppController.h"
#include "ConfigManager.h"
#include "GridPane.h"
#include "SmatchetDefaults.h"
#include "CancelToken.h"

// Use json_fwd here rather than the full json.hpp: the complete json type is
// needed only at the few use sites and the out-of-line UiDrawSession destructor,
// which include it directly. Keeps json off this 75-includer header. See debt
// entry json-fwd-swap-by-value-carriers.
#include <nlohmann/json_fwd.hpp>
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "IssueTableSerializer.h"
#include "TrackerGridFieldDisplay.h"
#include "NavigationHistory.h"
#include "SpreadsheetState.h"
#include "FieldCatalogCache.h"
#include "TrackerFieldSchema.h"
#include "QuerySuggestTypes.h"
#include "SmatchetProjectPicker.h"
#if defined(SMATCHET_WITH_AI)
#include "AiTypes.h"
#include "AiOutboundConsent.h"
#endif

#include "imgui.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace diagnostics {
struct SubmitResult; // log-a-bug-github — held by shared_ptr in UiDrawSession
}
} // namespace smatchet

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
    // S5: a thumbnail decode→upload task is currently in flight for this entry. Set when the
    // decode is launched, cleared by the dispatcher upload callback. Prevents double-kicking the
    // same entry while its decode runs, and lets a rate-limited (saturated) skip retry on a later
    // frame instead of the thumbnail silently never loading.
    bool ThumbnailDecodeInFlight = false;
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
    /// Original scalar DISPLAY value (CachedTicket::GetFieldValue) at edit-commit time — the
    /// scalar twin of OriginalRichValue. Persisted with the offline queue entry so replay can
    /// detect a concurrent server change for non-rich fields (base vs re-fetched display). Empty
    /// for rich fields (which use OriginalRichValue) and for edits with no captured base. ADR-0016.
    std::string OriginalValue;
    /// True when OriginalValue is a CAPTURED scalar base (even if blank), distinguishing a genuinely
    /// empty captured base from "no base captured" so replay still conflict-checks it. Set at the
    /// QueueEdit choke point for scalar edits; stays false for rich edits. See ADR-0016.
    bool HasOriginalValue = false;
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
    /// Project key this catalog was scoped to (resolved from the active-view JQL). The grid
    /// completion pins it via AppController::SetCurrentCatalogProject() before SetFieldCatalog()
    /// so the snapshot lands under the project-scoped cache entry that carries component options.
    /// Empty when the JQL resolves no project (unscoped fetch).
    std::string ProjectKey;
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

/// Which top-level Preferences tab is selected this frame. Recorded by each
/// tab's BeginTabItem-true branch so the shared footer under the tab bar can
/// show only the save-semantics line relevant to the active tab. Nested
/// sub-tabs (Fields Inputs children) map to their top-level parent.
enum class PreferencesActiveTab : std::uint8_t {
    Tracker = 0,
    Integrations,
    Assistant,
    Whisper,
    LocalData,
    Appearance,
    Templates,
    Annotate,
    Keybindings,
    UserInfo,
};

struct CellWriteFeedback {
    CellWriteState State = CellWriteState::Saving;
    std::string Message;
    int FramesRemaining = 0;
};

/// Reusable JQL/filter editor state — buffer + autocomplete bookkeeping for one
/// query-input surface. The dashboard Views editor and the global omnibar each own
/// an independent instance so their in-flight async user-search request-ids never
/// collide (a shared id let one surface's stale completion clobber the other's).
struct JqlEditorState {
    char buf[512]{};
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
};

struct UiDrawSession {
    bool cfgInitialized = false;
    TrackerConfig cfg;

    // ---- Dual-UI mode (dual-ui-mode-desktop-mobile plan) ----
    // Resolved once per frame at the top of SmatchetUI::Draw via drawResolveUiMode.
    // `effectiveUiMode` is the single source of truth AND the hysteresis carry: in
    // Auto mode it survives across frames so the 720/860 dead band holds the last
    // decision instead of flapping. mobilePage/mobileDrawerOpen are the mobile
    // shell's per-session view state; the two *Seeded flags reset on the
    // Mobile->Desktop edge so re-entering Mobile re-applies the saved/seeded layout.
    EffectiveUiMode effectiveUiMode = EffectiveUiMode::Desktop;
    MobilePage mobilePage = MobilePage::Grid;
    bool mobileDrawerOpen = false;
    bool mobilePageSeeded = false;
    // mobileDockSeeded doubles as the "mobile ini session active" latch: set true on the
    // Desktop->Mobile edge when the shell detaches io.IniFilename (so ImGui stops auto-saving
    // the desktop imgui.ini) and routes WantSaveIniSettings to imgui_mobile.ini; cleared on the
    // Mobile->Desktop edge after the desktop ini is re-attached + reloaded. See slice 5.
    bool mobileDockSeeded = false;
    // True between mobile entry and the first MobileContentDock submit when no imgui_mobile.ini
    // existed — triggers the one-shot DockBuilder seed (grid list / detail vertical split).
    bool mobileDockNeedsSeed = false;
    // Host-owned desktop imgui.ini path pointer captured on the Desktop->Mobile edge so the exact
    // pointer is restored to io.IniFilename on the way back (re-attaches ImGui's desktop auto-save).
    const char* savedDesktopIniFilename = nullptr;

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
    /// Active Preferences tab this frame (footer save-semantics line). Persists
    /// across frames; defaults to the first tab.
    PreferencesActiveTab preferencesActiveTab = PreferencesActiveTab::Tracker;
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
    bool showAnnotateAnalysis = false;
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
    /// User Info window (SmatchetUserInfoUi). Opened from the grid right-click
    /// menu on user-type cells; dock tab X / Escape clears this.
    bool showUserInfo = false;
    /// One-frame focus latch for the User Info window. See `requestPreferencesFocus`.
    bool requestUserInfoFocus = false;
    /// Staged open/retarget request (SmatchetUserInfoUi::Open writes these;
    /// DrawWindow consumes them next frame via adoptPendingRequest).
    bool userInfoRequestPending = false;
    std::string userInfoSourcePaneId;
    std::string userInfoDisplayName;
    std::string userInfoEmail;
    std::string userInfoAccountId;
    /// Host-wired: "Add to view query" from the User Info window's issue-key
    /// context menu (paneId, issueKey) — SmatchetUI binds this to
    /// userInfoAddToQuery so the popup TU stays decoupled from view plumbing.
    std::function<void(const std::string& paneId, const std::string& issueKey)> onUserInfoAddToQuery;
    /** When false, the Log window is hidden (dock tab X sets this; reopen from Settings). */
    bool showLogWindow = true;
    /// One-frame focus latch for the Log window. See `requestPreferencesFocus`.
    bool requestLogFocus = false;
    /** Notification Center (toast-history log, ticket-change-monitor S3). Opened by the View
     *  menu, the `notifications` command, or a transient toast click (consumed in
     *  SmatchetUI::Draw via SmatchetToastManager::ConsumeOpenCenterRequest); closed via the X. */
    bool showNotificationCenterWindow = false;
    /// One-frame focus latch for the Notification Center. See `requestPreferencesFocus`.
    bool requestNotificationCenterFocus = false;
    int layoutForceDefaultsFrames = 0;
    std::unordered_set<std::string> pendingReDockWindows;
    /// Latch set when a layout reset is requested MID-FRAME (menu bar, command dispatch).
    /// The heavy dock work (LoadIniSettingsFromMemory + force-redock) must run at
    /// end-of-frame, never inline — a mid-frame LoadIni defers the node-tree rebuild to the
    /// next NewFrame while the same-frame redock corrupts the stale tree (orphans 0x02/0x04/
    /// 0x08). drawEndOfFramePersistence drains this via SmatchetUI_ApplyDeferredLayoutReset.
    bool pendingLayoutReset = false;

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
    /// Stashed per-block rows for the first-send outbound-context consent modal,
    /// measured once when the gate fires (DispatchAiSend) and read each frame the
    /// modal is open. Empty when the modal is not pending. Drives the disclosure
    /// list + total-bytes line; the gate decision itself reads
    /// `cfg.AssistantOutboundConsentShown` via `ShouldRequireOutboundConsent`.
    std::vector<smatchet::ai::OutboundConsentBlockRow> assistantConsentRows;
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
    /// Probe-generation token: bumped on every probe launch AND on every field
    /// edit (ClearStaleTestResult), captured into the worker at launch. The
    /// posted result-commit callback drops its verdict when the counter moved
    /// mid-flight, so a provider/credential edit can't surface a stale
    /// Verified/Failed. UI-thread-only.
    std::uint64_t assistantPrefsTestGeneration = 0;
    /// Set by the Test-connection success callback when the probe used a
    /// fallback default URL (cfg base URL was empty). The next paint of the
    /// Assistant tab reseeds the static InputText buffers from the working copy
    /// so the just-persisted default value shows up in the field, then clears
    /// this flag. UI-thread-only.
    bool assistantPrefsForceBufferReseed = false;

    // --- Assistant Preferences tab: explicit Save / Discard working copy. ---
    // Unlike the other Preferences tabs (which write straight into `cfg` +
    // MarkPrefsDirty for a debounced autosave), the Assistant tab edits a
    // staging copy of the AI-related fields and only commits them into `cfg` on
    // an explicit Save. Discard reverts the staging copy back to `cfg`. The
    // validator banner + Test-connection probe read this working copy so they
    // reflect the unsaved edits the user is typing. Seeded from `cfg` on the
    // tab open-edge (see `assistantPrefsWorkingSeeded`). UI-thread-only.
    TrackerConfig assistantPrefsWorking;
    /// False until the working copy has been seeded from `cfg` for the current
    /// open session of the tab. Reset on window close so reopening re-seeds.
    bool assistantPrefsWorkingSeeded = false;
#endif

    bool fieldCatalogFetchStarted = false;
    bool fieldCatalogLoading = false;
    std::future<FieldCatalogFetchResult> fieldCatalogFuture;
    bool triggerCatalogRefetch = false;
    std::string fieldCatalogWarning;

    bool appliedInitialView = false;
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
    // No projectKeyBuf / planeProjectBuf — project is per-operation, not a saved
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
    char linearApiKeyBuf[512]{};
    char linearBaseUrlBuf[256]{};
    char linearTeamKeyBuf[128]{};
    char linearTeamIdBuf[128]{};
    char linearWorkspaceUrlBuf[256]{};
    char newIssueInheritFieldsBuf[512]{};
    char newIssueInheritFieldsPlaneBuf[512]{};
    char newIssueInheritFieldsGitHubBuf[512]{};
    char newIssueInheritFieldsLinearBuf[512]{};
    // User Info window / VCS commit feed (docs/plans/user-info-window.md). Edited in the
    // Tracker tab; written immediately on change (MCP-style immediate-dirty, no Save button).
    char gitCommitReposBuf[256]{};
    char productionGroupKeywordBuf[128]{};
    int userActivityDayWindow = 30;
    int maxUserChanges = 50;
    int vcsFeedLayoutIndex = 0; // 0=unified, 1=separate
    bool mcpEnabled = false;
    int mcpPort = SmatchetDefaults::Mcp::kDefaultPort;
    bool mcpAllowRemote = false;
    bool mcpAllowLuaExecution = false;
    char mcpAuthTokenBuf[512]{};
    bool preferencesBuffersLoaded = false;
    /** Integrations tab: brief "saved" line after MCP fields persist. */
    std::chrono::steady_clock::time_point mcpPrefsSavedHintUntil{};

    /// Snapshot-on-open of `FieldCatalogCache::ListCachedProjects()` (Pillar 2 — #767/#892).
    /// `ListCachedProjects()` does an ifstream + JSON parse + migrate + sort; calling it
    /// every frame a popup/tab is open blocks the render thread on disk I/O. Instead the
    /// open-edge of the JQL project-pill popup (#767) and the Preferences Tracker tab (#892)
    /// each refresh this snapshot once, and the per-frame draw reads the cached vector.
    /// `cachedProjectsSnapshotValid` gates the refresh: each surface tracks its own
    /// open-edge latch (`projectPillPopupWasOpen` / `prefsTrackerTabWasOpen`) and refreshes
    /// the shared snapshot on the closed→open transition. Stale data on reopen is accepted
    /// (these are LRU project lists — a refresh-on-reopen is fine; see plan § Risks).
    std::vector<FieldCatalogCache::CachedProjectEntry> cachedProjectsSnapshot;
    bool cachedProjectsSnapshotValid = false;
    /// Open-edge latch for the JQL project-pill popup (#767). False while the popup is closed;
    /// the draw refreshes the snapshot on the frame it transitions to open.
    bool projectPillPopupWasOpen = false;
    /// Open-edge latch for the Preferences Tracker tab (#892). See `projectPillPopupWasOpen`.
    bool prefsTrackerTabWasOpen = false;

    char viewNameBuf[128]{};
    /// Dashboard Views JQL editor (buffer + autocomplete state). Extracted into a reusable
    /// JqlEditorState so the omnibar can own a second independent instance (request-id isolation).
    JqlEditorState viewJqlEditor;
    /// Global omnibar JQL/search editor — independent instance (own async request-ids).
    JqlEditorState omniJqlEditor;
    /// Pane id whose saved omnibar text is currently loaded into omniJqlEditor.buf. drawOmnibar
    /// restores the focused pane's text on a change so each tab keeps its own omnibar input.
    std::string omnibarSyncedPaneId;

    // Authoritative selected-field id set for the Views editor (#views-field-uncheck).
    // The toggle handlers / select-all / clear mutate THIS directly; it is seeded
    // from view.Fields on view activate/create/discard. Any code writing view.Fields
    // from the editor selection must read this set. (A former 1024-byte CSV mirror
    // buffer — the original truncation source of #views-field-uncheck — was removed
    // once the set became authoritative.)
    std::unordered_set<std::string> selectedFieldSet;
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
    /// One-frame deferred view-create latch (Pillar 3 — crash fix). Views::Create
    /// push_backs into store.Views: a mid-frame create from a click handler
    /// ("+ New" / Ctrl+N / "Duplicate" / the grid "Save as new" modal) reallocates
    /// the vector and dangles every ViewDefinition* resolved earlier in the frame
    /// (the dashboard's ctx.activeView, each pane's ActiveProjectDrawCtx
    /// .activeViewForGrid); the remainder of the frame then reads freed memory
    /// (observed crash: unhandled C++ exception -> std::terminate minidump).
    /// Click sites copy a full payload while their pointers are still valid and
    /// latch it here; SmatchetUI::Draw consumes the latch (applyPendingViewCreate)
    /// at the TOP of the next frame, BEFORE any view pointer is resolved.
    /// Consume-once; SINGLE-SLOT, LAST-WINS (a second latch in the same frame
    /// overwrites the first — reachable only by Ctrl+N + a same-frame click,
    /// where both payloads are identical anyway).
    bool viewsPendingCreate = false;
    ViewDefinition viewsPendingCreatePayload;
    std::string viewsPendingCreateToastTitle;
    int viewsPendingCreateToastMs = 1800;
    /// Grid "Save as new" also adopts the created view's Jql/fields into cfg + saves.
    bool viewsPendingCreateAdoptCfg = false;
    /// Same deferral for DELETE: Views::DeleteActive erases from the same vector
    /// (same invalidation class as Create — it was safe pre-latch only because the
    /// delete modal happened to be the frame's last view-pointer reader). Consumed
    /// by SmatchetUI::applyPendingViewDelete, immediately after the create latch.
    bool viewsPendingDeleteActive = false;
    std::string viewsPendingDeleteToastName;
    /// DR13b — the id of the view targeted by the delete-confirm modal + delete latch. Set at
    /// right-click "Delete view..." (the right-clicked row) or the editor-header Delete button
    /// (the active view). Lets deletion hit the right-clicked view regardless of which view is
    /// active or whether the active view's editor is dirty. Empty falls back to the active view.
    std::string viewsPendingDeleteId;

    // ---- Grid panes (multi-grid-tabs Slice 2, ADR-0018) ----
    // The per-pane grid runtime that used to live here as singleton fields
    // (gridState / gridFilterBuf / sort+filter caches / lastGridActiveViewId /
    // lastGridContextSignature / wheel hysteresis / forceApplySortSpecs) migrated
    // into GridPane. Bootstrapped from smatchet_panes.json by the pane-window host
    // (SmatchetGridPaneWindows).
    std::vector<GridPane> gridPanes;
    /// SESSION-level backend bucket the views/catalog/initial-sync state was last
    /// loaded for. Deliberately NOT per-pane: the live context is session-scoped, so
    /// the backend-change session reset (drawViewStateAndConnectivity) must key on a
    /// session delta — per-pane tracking went blind on A→B→A pane-focus hops because
    /// the returning pane's own key never changed (review HIGH-4). Fires on host pane
    /// focus switches too, since a switch re-points cfg.TrackerType.
    std::string lastViewsBackendKey;
    /// Consume-once latch set by the pane focus-switch path when it re-points
    /// cfg.TrackerType at a pane whose own GridLiveContext is already sync-live
    /// (Slice 3): the lastViewsBackendKey session reset above must still fire
    /// (catalog refetch, editor-buffer reset), but its downstream initial
    /// SyncWithBackend kick would be a redundant network re-fetch of data the
    /// pane's context already holds — the kick consumes this flag and skips it.
    bool suppressNextBackendSwitchInitialSync = false;
    /// Backend key the suppress latch above was set FOR: the
    /// consume site honours the latch only while the session tracker still matches,
    /// so a same-frame Preferences backend switch can't have its legitimate initial
    /// sync swallowed by a stale pane-focus suppression.
    std::string suppressNextBackendSwitchInitialSyncKey;
    /// One-frame latch: the HOST reassigned focusedPaneId outside the normal
    /// window-focus path (focused-pane close, "+" duplicate, bootstrap restore).
    /// Consumed by drawGridPaneWindows, which treats it as a real focus switch so
    /// the newly focused pane's saved viewId is activated — without it the
    /// steady-state pane↔view sync rebound the survivor to the CLOSED pane's view
    /// and persisted the loss (review HIGH-2 / MEDIUM-3).
    bool gridPaneFocusReassigned = false;
    /// Kinds for the one-frame deferred pane action latch below (multi-grid Slice 3,
    /// plan item 19 — extends Slice 2's refresh-only latch, review MEDIUM-2).
    enum class PaneDeferredActionKind { None, RefreshView, NewIssueDraft };
    /// One-frame deferred toolbar action from a not-yet-focused pane ({paneId, kind}):
    /// acting on the click frame would target the still-focused pane's live context /
    /// active view. The host consumes this AFTER applying the focus/view switch;
    /// consume-once (a request whose pane didn't gain focus drops).
    std::string paneDeferredActionPaneId;
    PaneDeferredActionKind paneDeferredActionKind = PaneDeferredActionKind::None;
    std::string focusedPaneId;
    bool gridPanesLoaded = false;
    /// Debounced smatchet_panes.json save latch (mirrors prefsDirty).
    bool gridPanesDirty = false;
    std::chrono::steady_clock::time_point gridPanesSaveDueAt{};
    /// Transient (one frame): pane id whose window reported ImGui focus this frame.
    /// Written inside the pane window's Begin/End scope, consumed by the host loop
    /// to detect focus switches (drives the Slice-2 focused-pane live-context swap).
    std::string paneWindowFocusedThisFrame;
    /// Previous frame's `paneWindowFocusedThisFrame`. A focus switch is only committed
    /// once the SAME pane has reported focus on two consecutive frames
    /// (ping-pong guard): adopting a focused pane rewrites the session-global
    /// cfg.TrackerType, and with two simultaneously-visible split panes on different
    /// backends that global churn perturbs ImGui nav focus, flipping the report to the
    /// sibling next frame — an infinite re-adopt loop. A genuine click holds focus for
    /// many frames (commits with 1-frame latency); a 1-frame nav bounce never reaches
    /// the two-frame threshold, so the rewrite never fires and focus settles.
    std::string lastPaneFocusReport;
    /// Transient (one frame): the "+" button / pane commands latch a pane-creation
    /// request applied AFTER the pane loop (gridPanes never mutates mid-iteration).
    /// sourceId.empty() = no request; see PaneAddRequest for field semantics.
    PaneAddRequest paneAddRequest;
    /// Set by the pane-window host (RAII) to the pane currently being drawn so
    /// shared grid helpers (header toolbar, cell support, new-issue draft) reach
    /// the CURRENT pane through `pane()` without signature churn. Null outside
    /// the pane render loop — `pane()` then resolves to the focused pane, which
    /// is the permanent semantics for global actions (command palette, menus,
    /// MCP/Lua) per ADR-0018.
    GridPane* activePaneForDraw = nullptr;

    /// Focused pane (never null): resolves focusedPaneId, falls back to the first
    /// pane, and self-heals an empty vector with a default pane so pre-bootstrap
    /// reads (frame 1) and a corrupt panes file can never crash (Pillar 3).
    GridPane& focusedPane() {
        if (gridPanes.empty()) {
            GridPane fallback;
            fallback.id = "main";
            fallback.title = "Grid";
            gridPanes.push_back(fallback);
            focusedPaneId = "main";
        }
        if (GridPane* byId = FindGridPaneById(gridPanes, focusedPaneId)) {
            return *byId;
        }
        focusedPaneId = gridPanes.front().id;
        return gridPanes.front();
    }

    /// Read-only focused pane for const consumers (AI context build, exports). Cannot
    /// self-heal — returns a static empty pane when the vector is empty (pre-bootstrap).
    const GridPane& focusedPane() const {
        static const GridPane kEmptyPane;
        if (gridPanes.empty()) {
            return kEmptyPane;
        }
        if (const GridPane* byId = FindGridPaneById(gridPanes, focusedPaneId)) {
            return *byId;
        }
        return gridPanes.front();
    }

    /// The pane the current draw call targets: the pane being rendered inside the
    /// pane-window loop, the focused pane everywhere else. See activePaneForDraw.
    GridPane& pane() { return activePaneForDraw ? *activePaneForDraw : focusedPane(); }

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

    bool newIssueDiscardAsyncCreateResult = false;
    bool viewSortDirty = false;
    bool logAutoScroll = true;

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
    // When the draft's ProjectKey diverges from this guard, the new-issue draft UI kicks an
    // async RefreshFieldCatalog so per-project required-fields/create-meta land before submit.
    std::string newIssueDraftLastFetchedProjectKey;

    // Project-picker state for the new-issue draft combo and the bulk-import modal.
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
    // WS-A cooperative cancel: each in-flight create captures this token (passed to
    // CreateIssueAsync); a `.clear()` (re-parse / re-run) signals it then moves the
    // still-valid futures into the graveyard so the UI frame never blocks on a
    // running create's destructor. The token is Reset() per fresh run.
    smatchet::ui::CancelToken bulkImportCancel;
    // Abandoned-but-still-running create futures, drained at app teardown
    // (DrainUiDrawSessionFuturesBeforeAppTeardown) while AppController is alive.
    std::vector<std::future<IssueCreateResult>> bulkImportFutureGraveyard;
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
    /// Pixel X to set via ImGui::SetScrollX on the focused pane each frame. -1 = inactive.
    /// Written directly by interaction-injection scenarios (no ScenarioRunner out-param —
    /// horizontal scroll has no CurrentScrollX runner hook); read by SmatchetActiveProjectGridUi.
    int scenarioScrollTargetX = -1;

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
    /// log-a-bug-github — set by the bug-report modal when its capture request is for the
    /// report (not a debug/test shot). The standalone capture path echoes completion back
    /// via bugReportShotReady so the modal never polls the filesystem on the UI thread.
    bool requestScreenshotBugReport = false;
    /// font-redaction censor — set on the bug-report Submit frame; the standalone loop
    /// repoints the fonts to the Redaction font on the NEXT (captured) frame so every
    /// attached bug-report screenshot is █-redacted (sharp, layout-preserving).
    bool requestRedactFontThisFrame = false;

    /// pink-clear dock-gap scan (pink-clear-dock-gap-scan) — when true for a frame,
    /// the standalone render path uses requestClearColor{R,G,B,A} as the GL clear
    /// color INSTEAD of the theme WindowBg, so any uncovered dock gap renders in the
    /// sentinel color (typically magenta 255,0,255). TRANSIENT: the consumer in
    /// Source/Standalone/main.cpp reads then clears this flag every frame (mirrors
    /// requestRedactFontThisFrame), so the normal theme clear is restored the moment
    /// the override is no longer re-set. A scenario re-sets it each warm-up frame to
    /// hold the diagnostic clear for the captured frame. No-op on the DX12 path.
    bool requestClearColorActive = false;
    float requestClearColorR = 0.0f;
    float requestClearColorG = 0.0f;
    float requestClearColorB = 0.0f;
    float requestClearColorA = 1.0f;

    // --- "Log a Bug" modal (docs/plans/shipped/log-a-bug-github.md Slice 4) ---
    // All bugReport* fields are read/written ONLY on the UI thread (the worker posts
    // its result back via mainThreadDispatcher), so no extra synchronisation is needed.
    bool showBugReport = false;
    bool bugReportOpenLatch = false;         // set when newly opened — drives first-frame focus
    bool bugReportInclScreenshot = true;     // attach-screenshot checkbox (seeded from config)
    bool bugReportPreviewOpen = false;       // egress-preview collapsible open
    bool bugReportInFlight = false;          // submit in progress (capture handshake or worker)
    bool bugReportShotPending = false;       // requested a screenshot; waiting for the capture to land
    bool bugReportShotArmed = false;         // submit pressed — arm capture on the NEXT (modal-suppressed) frame
    bool bugReportShotReady = false;         // standalone capture path signals completion here (UI-thread only)
    double bugReportShotDeadline = 0.0;      // ImGui::GetTime() deadline — frees the modal if capture never lands
    bool bugReportCrashMode = false;         // phase-2: pre-filled crash report
    std::string bugReportCrashContext;       // phase-2: crash summary seeded into the description on next launch
    std::string bugReportCrashDumpPath;      // phase-2: minidump to upload as a Release asset on submit
    bool bugReportPreviewDirty = true;       // rebuild preview text only when inputs changed
    bool bugReportPreviewSeeded = false;     // preview buffer has been built at least once (drives BodyOverride)
    bool bugReportPreviewUserEdited = false; // user edited the preview — stop auto-regenerating from inputs
    bool bugReportPreviewGenerating = false; // preview being built on a worker (GatherContext does disk I/O)
    std::vector<char> bugReportDescBuf;      // lazy multiline description buffer
    std::vector<char> bugReportPreviewBuf;   // editable egress preview = exactly what gets sent
    std::string bugReportStagedShotPath;     // captured screenshot path (post-swap)
    std::shared_ptr<smatchet::diagnostics::SubmitResult> bugReportResult; // posted back from the worker

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
    /// AI Assistant tab keeps its explicit Save flow and does
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

/// Snapshot-on-open helper for the cached-project lists (#767/#892). Re-reads
/// `FieldCatalogCache::ListCachedProjects()` (disk I/O) ONCE on a surface's open-edge so the
/// per-frame draw reads `d.cachedProjectsSnapshot` instead of hitting disk every frame.
/// `wasOpenLatch` is the caller's per-surface closed→open edge tracker: pass the latch's
/// previous value as `isOpenNow`'s complement so the refresh fires exactly on the transition.
/// UI-thread-only.
inline void RefreshCachedProjectsSnapshotOnOpen(UiDrawSession& d, bool isOpenNow, bool& wasOpenLatch) {
    if (isOpenNow && !wasOpenLatch) {
        d.cachedProjectsSnapshot = FieldCatalogCache::ListCachedProjects();
        d.cachedProjectsSnapshotValid = true;
    }
    wasOpenLatch = isOpenNow;
}

extern UiDrawSession g_ui;

/** Blocking join for audit file reload worker (no AppController capture). */
void DrainAuditReloadFuture(UiDrawSession& d);
