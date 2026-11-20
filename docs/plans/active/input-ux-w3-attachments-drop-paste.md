# Plan — input UX Wave 3: attachments by drop and paste on drafts and comments

> **Slug**: `input-ux-w3-attachments-drop-paste` (matches this file's basename without `.md`).
>
> **Status**: `active` — plan only; drives nine PRs (S1 → S8, with S7 in two parts) against `develop`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 5.3 lists "attachments by drop and paste on drafts and comments" as one Wave 3 structural item (#54, **#137**). § 7 has the evidence. Today:

- **A drop goes to one place.** The standalone host's `MarkdownFileDropCallback` (`Source/Standalone/StandaloneBoot_detail.cpp:92`, installed by `glfwSetDropCallback` at `main.cpp:613` and `StandaloneAppBootstrap.cpp:567`) forwards every dropped path to `PlanDocViewerOpenDroppedPaths` (`Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:641`). It ignores where the cursor is. Markdown opens the Plan Docs viewer and takes focus; anything else gets a "not opened" toast. The Unreal plugin has no file-drop hook at all.
- **A file reaches a draft only through the picker.** "Add attachment..." (`SmatchetNewIssueDraftUi.cpp:455`) opens the OS dialog and `TryAppendStagedFromAbsPath` (`:193`) records the path in `IssueDraft::StagedAttachments`. It stats the file on the UI thread and has no size or count cap.
- **Ctrl+V of an image does nothing.** No code reads an image or a file list from the clipboard.
- **Comments cannot carry files.** `SubmitOrQueueComment` sends text only.
- **A failed upload loses the file.** `ApplyPostIssueAttachmentStep` (`Source/Core/src/Tracker/IssueCreatePipeline.cpp:157`) reports per-file failures in `IssueCreateResult::AttachmentFailures`, the UI warns, and the draft is cleared. A hard error after the issue exists leaves the list empty. A create queued offline keeps only the original path, so a file moved before reconnect is gone.
- **Only Jira uploads.** `PlaneClient::AttachFilesToIssue` returns "not implemented"; GitHub and Linear inherit the interface's "not supported" default. Nothing tells the UI, so the button offers an upload that fails after the issue is created.

Two changes this plan depends on are not on `develop` yet:

- **Wave 2 piece 4.3 drafts and undo** (#2439, open; its plan `input-ux-w2-drafts-undo` lands with it) adds `smatchet::drafts` (`DraftStore.h`), which keeps the comment text across a close and a restart. It edits `SmatchetCommentsModalUi.cpp`, `SmatchetOfflineQueueUi.cpp` and `SmatchetUI.cpp`. It does not persist the new-issue draft.
- **Wave 3 siblings** run in parallel: grid as a spreadsheet (#2461, plan `input-ux-w3-grid-spreadsheet`), filter without JQL, and description templates / preview / mentions (#55). § Implementation handoff keeps this plan off their files.

After this lands, a file dropped on the new-issue draft row or on the comments window, or an image or copied files pasted with Ctrl+V into the draft's text fields or the comment box, is staged: it shows as a chip the user can remove. It is uploaded when the draft or comment is submitted. A file staged while offline is kept by the app, queued with its create or comment, and uploaded on reconnect. A failed upload stays in the Offline Queue as a restorable row instead of disappearing.

**Terms.** The glossary already uses "pending" for a write waiting for replay (`Source/Core/src/Persistence/CONTEXT.md`, **Pending write**) and says an `IssueDraft` holds "staged attachments" (`Source/Core/src/Tracker/CONTEXT.md`). This plan keeps both. A **staged attachment** is a file the app holds its own copy of, shown as a chip on a draft or in the comment composer, not yet sent. The **staging store** is where those copies live. An **attachment upload** is the pending action (one queue row) that sends one staged attachment to one issue. "Pending attachment" is not used.

## Approach

Three concerns stay apart: where a gesture lands (Ui), what the app holds (a staging store), and how a file reaches the tracker (one queue row per file).

**Gesture to Core.** Hosts call one GLFW-free Core function, `smatchet::ui::PostFileDrop(paths, x, y)`. The drop waits one frame. During that frame's draw, each drop zone asks whether the drop point is inside its rect and whether its window is the topmost window at that point, using ImGui's own hit test, so z-order and modals behave as they do for the mouse. The first zone to claim takes every path in the drop. A drop no zone claims goes to `PlanDocViewerOpenDroppedPaths`, which is today's behaviour. Ctrl+V is observed, not intercepted: when the clipboard holds no text but holds files or an image, a worker reads it; ImGui's own paste inserts nothing because there is no text, and a text paste is untouched.

**Staging store.** Every file the UI stages (drop, paste and the existing picker) is copied on a worker into an app-owned, path-confined directory, `<user data>/staged_attachments/<id>/<sanitized name>`, with every cap checked before any byte is read. The draft or queue row carries the staged id. A pasted image has no source path, so a store is unavoidable; using it for dropped files too means a queued upload survives the source being moved or edited, and what is uploaded is exactly what was staged. The trade-off is a bounded disk copy per file.

**Upload, one row per file.** On create, the pipeline uploads file by file. Whatever is not uploaded once the issue exists becomes a `PendingActionKind::AttachmentAdd` row per file in the existing pending-action queue; a final rejection is archived to the dead table at once, so it is data the user can restore. On comment, the files go first. If any file had to be queued, the comment is queued too and waits, without spending attempts, until none of its files is still live in the queue. A file whose send may have landed is checked against the issue's attachment list before any resend. The queue gains a new `kind` value only: no SQLite schema change.

## Interface contracts

All C++14. No new header includes GLFW or `imgui.h` unless stated. This section is the approved widening that `Source/Core/src/Tracker/AGENTS.md` (Before you edit) and `Source/Core/src/Ui/AGENTS.md` (Before you edit) require; the host API (`SmatchetImGuiHost.h`, `SmatchetImGuiHostC.h`) and `HostCallbacks` do not change.

1. **`Source/Core/include/Tracker/TrackerAttachmentTypes.h` (new, plain data).**
   ```cpp
   struct TrackerIssueAttachment { std::string Id; std::string FileName; std::uint64_t SizeBytes = 0; std::int64_t CreatedAtSec = 0; };
   ```
2. **`ITrackerIssueMutations`** (`Source/Core/include/ITrackerIssueMutations.h`, after `AttachFilesToIssue`). Backend-agnostic, default off, per the capability-flag rule.
   ```cpp
   virtual bool SupportsAttachments() const { return false; }
   virtual Result<TrackerIssueAttachment, TrackerError> UploadIssueAttachment(const std::string& issueKey, const std::string& absolutePath);  // default: Err InvalidRequest
   virtual Result<std::vector<TrackerIssueAttachment>, TrackerError> ListIssueAttachments(const std::string& issueKey);                      // default: Err InvalidRequest
   ```
   `AttachFilesToIssue` keeps its signature and its "Ok = per-file failures" meaning (the Lua `create_issue` binding returns them as `attachment_failures`). Only `JiraClient` overrides the three new calls; its `AttachFilesToIssue` becomes a loop over `UploadIssueAttachment`. `ITrackerCollaboration` does not change.
3. **`StagedAttachment`** (`Source/Core/include/Tracker/IssueDraft.h:13`) gains `std::string StagedId;`. `IssueDraftHelpers::ToJson` writes `"stagedId"`; `FromJson` reads it and keeps an entry when `AbsPath` or `StagedId` is non-empty. `AbsPath` is still written (it is the staging path), so an older build can still upload a queued create, and rows queued before this plan parse unchanged.
4. **`IssueCreateResult`** (`Source/Core/include/Tracker/IssueCreatePipeline.h:23`), additive:
   ```cpp
   struct AttachmentUploadLeftover { StagedAttachment Attachment; TrackerError Error; };
   std::vector<AttachmentUploadLeftover> AttachmentLeftovers; // not uploaded although the issue exists
   std::vector<std::string> UploadedStagedIds;
   ```
   Hops: `IssueCreatePipeline::Run` → `ApplyPostIssueSteps` → `ApplyPostIssueAttachmentStep(client, issueKey, draft, result)` (signature unchanged; it fills the new members and still fills `AttachmentFailures`). When `!client.SupportsAttachments()`, every staged file becomes a leftover with `InvalidRequest`.
5. **`PendingActionKind`** (`Source/Core/include/PendingActionTypes.h`) gains `AttachmentAdd`, wire value `"attachment_add"`, added to `PendingActionKindWire` and to the `known[]` list in `ParsePendingActionKind`. New plain structs beside it:
   ```cpp
   struct PendingAttachmentRef { std::string StagedId; std::string FileName; std::uint64_t SizeBytes = 0; };
   struct AttachmentSubmitOutcome { std::string StagedId; PendingActionSubmitResult::Kind K = PendingActionSubmitResult::Kind::Failed; std::string Error; };
   ```
   Persistence: a new `kind` value in `pending_actions` / `pending_actions_dead`, as `Source/Core/src/Persistence/AGENTS.md` prescribes. No column, table or `ISyncCache` method is added. Storage shape, checked: both tables already have `kind TEXT`, `payload_json TEXT` and `state TEXT`, and the dead table has `terminal_reason TEXT` (`Source/Core/src/Persistence/LocalCacheManager_PendingActions.cpp:79`–`:88`); a queued create keeps its draft JSON in `pending_creates.payload TEXT` (`LocalCacheManager.cpp:353`). A build without this plan archives an `attachment_add` row as `unknown_kind` (`PendingActionQueueService.cpp:280`), which is restorable.
6. **`PendingActionPolicyPure.h`** (`namespace smatchet::pendingaction`):
   ```cpp
   struct AttachmentActionPayload { std::string StagedId; std::string FileName; std::uint64_t SizeBytes = 0; std::int64_t CreatedAtSec = 0; };
   std::string BuildAttachmentActionPayload(const AttachmentActionPayload& p);   // {"stagedId","fileName","size","created"}; never a source path
   bool ParseAttachmentActionPayload(const std::string& json, AttachmentActionPayload& out); // bounded; false unless IsStagedId(out.StagedId)
   bool AttachmentAlreadyUploaded(const std::vector<TrackerIssueAttachment>& listed, const AttachmentActionPayload& p);
   std::string BuildCommentActionPayload(const std::string& body, std::int64_t createdAtSec, const std::vector<std::string>& awaitStagedIds); // empty list == today's bytes
   bool ParseCommentAwaitStaged(const std::string& json, std::vector<std::string>& outIds);
   bool CommentWaitsForAttachments(const std::vector<std::string>& awaitIds, const std::vector<PendingActionRecord>& liveRows, const std::string& backendKey);
   ```
   `StateAfterFailedSend` gains the `AttachmentAdd` case with the comment rules: `pending` when the tracker provably did not apply it, `ambiguous` after a timeout or 5xx, final otherwise. `AttachmentAlreadyUploaded` matches the sanitized name, the size, and `CreatedAtSec ≥ p.CreatedAtSec − kCommentDedupeWindowSec`. `CommentWaitsForAttachments` is true only while an `attachment_add` row with the same backend key and one of the awaited ids is `pending`, `sending` or `ambiguous`. A file that is `needs_review`, dead or gone from the queue does not hold the comment: the comment then posts and the file stays where the user can see it.
7. **`PendingActionQueueService`** (`Source/Core/include/Sync/PendingActionQueueService.h`, `.cpp`). Today it only ever holds an `ITrackerCollaboration`; an attachment needs `ITrackerIssueMutations` at every hop:
   - `Dispatch` (`.cpp:43`) takes `ITrackerCollaboration* collab, ITrackerIssueMutations* mutations, IOfflineQueueDeps& deps` (both pointers nullable). `AttachmentAdd` resolves its path through `deps.ResolveStagedAttachment`; a missing file or a size that differs from the payload is a final error.
   - `RunReplayPass` (`:245`) and `ReplayOne` (`:276`) take both interfaces. `Tick` latches `deps_.MutationsShared()` beside `CollaborationShared()` (`:197`) and runs when either is present.
   - The gate in `SubmitOrQueue` (`:79`–`:105`) becomes per kind: `AttachmentAdd` needs `mutations && mutations->SupportsAttachments()`.
   - `ReplayOne`: an `ambiguous` or interrupted `AttachmentAdd` goes through a new `ResolveAmbiguousAttachment`, modelled on `ResolveAmbiguousComment` (`:341`): `ListIssueAttachments` then `AttachmentAlreadyUploaded`; a backend that cannot list sends the row to `needs_review`, never a blind resend. A `CommentAdd` for which `CommentWaitsForAttachments` holds is skipped before the claim, with no attempt spent. A sent attachment calls `deps.RemoveStagedAttachment`. A final failure on replay is archived with the existing reason `replay_rejected`.
   - `Discard` (`:446`) and `DeleteDead` (`:463`) are not changed. A staged file whose row was discarded is no longer referenced, and the orphan sweep removes it; there is one deleter, so a discard cannot race an upload that is reading the file.
   - New, both worker-only:
     ```cpp
     void QueueAttachmentLeftovers(const std::string& backendKey, const std::string& issueKey, const std::vector<AttachmentUploadLeftover>& leftovers);
     SubmitOutcome SubmitOrQueueCommentWithAttachments(const PendingActionTarget& target, const std::string& issueKey, const std::string& body,
                                                     const std::vector<PendingAttachmentRef>& files, std::vector<AttachmentSubmitOutcome>* outPerFile);
     ```
     `QueueAttachmentLeftovers` serves a create, where the draft is already gone: each leftover gets the state `StateAfterFailedSend(AttachmentAdd, error)` names, and a final one is enqueued and archived at once with the new terminal reason `upload_rejected`. `SubmitOrQueueCommentWithAttachments` submits each file with `SubmitOrQueue(AttachmentAdd)`. A final failure there returns `Failed` without a row, as it does for a comment today (`:113`–`:118`); the call stops, the comment is not posted, the result names the file, and the composer keeps its text and chips. If any file was queued, the comment is enqueued `pending` with those ids in `awaitStaged`, even while online; otherwise it is sent live as today.
8. **`IOfflineQueueDeps`** (`Source/Core/include/IOfflineQueueDeps.h`; implemented by `GridContextDepsAdapter` and by `tests/support/FakeOfflineQueueDeps.h`):
   ```cpp
   virtual bool ResolveStagedAttachment(const std::string& stagedId, std::string& outAbsPath, std::uint64_t& outSizeBytes) = 0;
   virtual void RemoveStagedAttachment(const std::string& stagedId) = 0;
   virtual void SettleCreateAttachments(const std::string& backendKey, const std::string& issueKey, const IssueCreateResult& result) = 0;
   ```
   `SettleCreateAttachments` is the one chokepoint both create paths call after `IssueCreatePipeline::Run` — the live launcher `LaunchIssueCreate_` (`Source/Core/src/AppController_IssueCreateOffline.cpp:123`) and the replay `OfflineQueueService::ReplayOneCreate` (`Source/Core/src/Sync/OfflineQueueService.cpp:1408`). It removes the staged files named in `UploadedStagedIds` and hands `AttachmentLeftovers` to `QueueAttachmentLeftovers`. A leftover with no staged id (a path from Lua or bulk import) is copied into the store first, on that worker. Before `Run`, replay re-resolves every `StagedId` through the store and ignores the stored `AbsPath`.
9. **`IAppPendingActions`** (`Source/Core/include/Interfaces/IAppPendingActions.h`) gains `SubmitOrQueueCommentWithAttachments` with the signature of contract 7; `AppController` forwards it. `IAppTicketMutations` and the `comment.add` command stay text-only.
10. **`Source/Core/include/Interfaces/IAppAttachmentStaging.h` (new facet, implemented by `AppController`).**
    ```cpp
    struct ClipboardAttachmentProbe { bool HasText = false; bool HasFiles = false; bool HasImage = false; };
    struct StagedAttachmentEvent { std::string StagedId; std::string AbsPath; std::string FileName; std::uint64_t SizeBytes = 0; bool Ok = false; std::string Error; };
    class IAppAttachmentStaging {
    public:
        virtual ~IAppAttachmentStaging() = default;
        virtual std::uint64_t StageFilesAsync(const std::vector<std::string>& sourcePathsUtf8, const char* origin) = 0; // UI thread; no disk access
        virtual std::uint64_t StageClipboardAsync() = 0;                                                               // UI thread; a worker reads
        virtual bool PollStaged(std::uint64_t ticket, std::vector<StagedAttachmentEvent>& out) = 0;                    // UI thread; true once the ticket is done
        virtual void DiscardStaged(const std::vector<std::string>& stagedIds) = 0;                                     // deletes on a worker
        virtual ClipboardAttachmentProbe ProbeClipboard() const = 0;                                                   // UI thread; trivial
        virtual bool FocusedBackendSupportsAttachments() const = 0;
    };
    ```
    The comments modal decides support from the backend of its latched `PendingActionTarget`, not from the focused pane.
11. **`Source/Core/include/ClipboardAttachmentSource.h` (new).**
    ```cpp
    struct ClipboardReadLimits { std::size_t MaxFiles; std::uint64_t MaxImageBytes; std::uint32_t MaxImageSide; std::uint64_t MaxImagePixels; };
    struct ClipboardReadResult { enum class Kind : unsigned char { None, Files, Image, Error } K = Kind::None;
                                 std::vector<std::string> FilePaths; std::vector<unsigned char> PngBytes; std::string Error; };
    class IClipboardAttachmentSource {
    public:
        virtual ~IClipboardAttachmentSource() = default;
        virtual ClipboardAttachmentProbe Probe() const = 0;                         // UI thread: format-availability checks only
        virtual ClipboardReadResult Read(const ClipboardReadLimits& limits) = 0;    // worker only
    };
    std::unique_ptr<IClipboardAttachmentSource> MakePlatformClipboardAttachmentSource(); // Win32, or a stub whose Probe() is all false
    ```
    `AppController::SetClipboardAttachmentSourceForTests(std::unique_ptr<IClipboardAttachmentSource>)` swaps it in tests. The clipboard can change between `Probe()` and `Read()`; `Read()` reports what it finds, and `Kind::None` adds no chip. `Read()` retries `OpenClipboard` a bounded number of times, because the UI thread or another app may hold it.
12. **`Source/Core/include/ClipboardImagePure.h` (new, pure).** `ParseDibHeader`, `DibToRgba`, and `enum class PasteRoute { Text, Files, Image, None }` with `PasteRoute DecidePasteRoute(const ClipboardAttachmentProbe&)`: text wins whenever the clipboard holds text.
13. **`Source/Core/include/AttachmentStagingPure.h` (new, pure, `namespace smatchet::staging`).** `kStagingSubdir = "staged_attachments"`, `kMetaFileName`, `kStagedOrphanGraceSec` (7 days), `IsStagedId`, `StagedIdFromBytes`, `enum class SourcePathVerdict { Ok, Empty, NotAbsolute, DeviceNamespace, EmbeddedNul, Unc }`, `ClassifySourcePath`, `StagedMeta` with `Serialize` / `Parse`, `SelectOrphans(metas, referencedIds, now, grace)`, `PastedImageFileName(epochSec)`, `FitsStore(used, add, cap)`. File names go through the existing `smatchet::attachments::SanitizeFileName` (`AttachmentCachePure.h:38`).
14. **`Source/Core/include/AttachmentStagingStore.h` (new; every method worker-only).** `StageFromPath(srcUtf8, origin)`, `StageFromBytes(bytes, fileName, origin)`, `Resolve(id, outAbs, outSize)`, `Remove(id)`, `SweepOrphans(referencedIds, now)`. Layout mirrors the attachment disk cache: one directory per 128-bit random hex id, holding the file and `staged.json` (id, file name, size, SHA-256, origin, created).
15. **`Source/Core/include/Ui/SmatchetFileDrop.h` (new, `namespace smatchet::ui`).** Plain floats, main-viewport client coordinates (the space of `io.MousePos`; multi-viewport is not enabled anywhere in `Source/`). UI thread only.
    ```cpp
    void PostFileDrop(std::vector<std::string> pathsUtf8, float x, float y);
    void PostFileDragOver(float x, float y);
    void PostFileDragLeave();
    void FileDropBeginFrame();
    std::vector<std::string> FileDropEndFrameTakeUnclaimed();
    bool FileDropZone(const char* zoneId, float minX, float minY, float maxX, float maxY, std::vector<std::string>& outPaths); // claims; draws the hover outline
    ```
    The rules are in the pure `Ui/FileDropRoutingPure.h` (new): `ZoneMayClaim(rect, x, y, zoneWindowIsTopAtPoint, blockedByModal)`. "Top at the point" means the window ImGui would hover at that point has the same root window as the zone's window, so a child region inside the zone's window does not hide the zone. Nothing is cached between frames, so a zone that is not drawn cannot claim. The Unreal C ABI pair (`SmatchetHost_PostFileDrop` / `SmatchetHost_PostFileDragOver`) is not added here; it lands with its only caller (§ Out of scope).
16. **`TunableLimits.h`** (after the attachment download rows, `:71`–`:72`):
    ```cpp
    X(AttachmentUploadMaxMiB, "attachment_upload_max_mib", 100, 1, 2048)
    X(AttachmentUploadMaxFiles, "attachment_upload_max_files", 20, 1, 200)
    X(AttachmentUploadTimeoutMs, "attachment_upload_timeout_ms", 300000, 5000, 3600000)
    X(AttachmentStagingStoreMaxMiB, "attachment_staging_store_max_mib", 2048, 64, 65536)
    X(ClipboardImageMaxMegapixels, "clipboard_image_max_megapixels", 64, 1, 1024)
    ```
17. **`Source/Core/include/Tracker/TrackerHttpUtils.h`** gains one helper so the upload stops calling `cpr::Post` directly (the Tracker leaf's "HTTP only through `TrackerHttpClient` / `TrackerHttpUtils`" rule, which the current upload breaks):
    ```cpp
    cpr::Response TrackerPostMultipartFileLogged(const char* clientName, const std::string& url, const cpr::Header& headers, const std::string& partName,
                                                 const std::string& absolutePath, std::int32_t overallTimeoutMs, const std::shared_ptr<std::atomic<bool>>& cancel);
    ```
    Single attempt, no retry wrapper (an attachment POST is not idempotent). It applies `MakeTrackerRedirectPolicy()` and `MakeTrackerSslOptions()`, records `NetworkUsageTracker`, logs through `LogTrackerHttpResult`, and aborts through a `cpr::ProgressCallback` when `cancel` is set, so shutdown does not wait out the upload timeout.
18. **Not yet merged: the #2439 draft store, as this plan uses it.** `Source/Core/include/DraftStore.h`, `namespace smatchet::drafts`: `bool Put(surface, scope, text, base = "")`, `bool Get(surface, scope, DraftEntry&)`, `Erase`, `EraseIfText`, `EraseSurface`; text only, bounded (200 entries, 160 KiB each), written to `<user data>/smatchet_drafts.json` by its own worker. The comment text lives under surface `"comments"`, scope `CommentsDraftScope(backendKey, issueId)` (`backendKey + "|" + issueId`). This plan stores the composer's staged attachments under surface `"comments_attachments"` with the same scope, `Text` being a JSON array of `{stagedId, fileName, size}`.
19. **No automation surface is added.** No `RegisterCommand`, MCP tool schema or Lua binding. One already exists and is not widened: Lua `create_issue` and bulk import can stage any local path through `IssueTableSerializer::ApplyKeyValueToDraft` (`Source/Core/src/Tracker/IssueTableSerializer.cpp:76`–`:97`). Its one behaviour change is that the upload size cap now applies to it.

## Files to modify

Core `.cpp` files are picked up by `file(GLOB_RECURSE CORE_SOURCES …)` in `Source/Core/CMakeLists.txt`, so no source-list edit is needed and no `SMATCHET_WITH_*` gating is touched. Every new Core TU therefore compiles into both `SmatchetStandalone` and `SmatchetCore_DX12`. "New" names were checked: a search for `FileDrop`, `DropZone`, `ClipboardAttachment`, `AttachmentStag`, `UploadIssueAttachment`, `ListIssueAttachments`, `SupportsAttachments`, `AttachmentAdd` and `TrackerPostMultipart` under `Source/` and `tests/` finds only the standalone `MarkdownFileDropCallback`.

**Tracker (S1)**
1. `Source/Core/include/Tracker/TrackerAttachmentTypes.h` (new): contract 1.
2. `Source/Core/include/ITrackerIssueMutations.h`: contract 2.
3. `Source/Core/include/Tracker/TrackerHttpUtils.h` and `Source/Core/src/Tracker/TrackerHttpUtils.cpp`: contract 17.
4. `Source/Core/include/Tracker/JiraClient.h`, `Source/Core/src/Tracker/JiraIssueMutation.cpp`: the three overrides. The per-file block of `AttachFilesToIssue` (`:557`–`:612`) moves into `UploadIssueAttachment` and calls the helper of item 3. It enforces `AttachmentUploadMaxMiB`, classifies failures with `ClassifyRejectedHttpStatus` / `TrackerErrorFromHttpStatus` (status 0 is `Transport`), and emits the `issue_attach_file` audit pair with file name and size, never the path. `ListIssueAttachments` is `GET …/issue/{key}?fields=attachment`.
5. `Source/Core/include/Tracker/JiraIssueMappingPure.h`, `Source/Core/src/Tracker/JiraIssueMappingPure.cpp`: new pure `ParseJiraAttachments(json)` used by both the upload response and the list.
6. `Source/Core/include/Tracker/IssueDraft.h`, `Source/Core/src/Tracker/IssueDraft.cpp`: contract 3.
7. `Source/Core/include/Tracker/IssueCreatePipeline.h`, `Source/Core/src/Tracker/IssueCreatePipeline.cpp`: contract 4.
8. `Source/Core/include/Tracker/TrackerFixtureBackendBase.h` and the deterministic Jira fixture: a recorded `SupportsAttachments` / upload / list, so bucket E can assert each upload happened exactly once. `tests/support/FakeTrackerClient.h`: the same three overrides.

**Staging store (S2; no leaf — Persistence rules apply, as for `AttachmentDiskCache`)**
9. `Source/Core/include/AttachmentStagingPure.h`, `Source/Core/src/AttachmentStagingPure.cpp` (new): contract 13.
10. `Source/Core/include/AttachmentStagingStore.h`, `Source/Core/src/AttachmentStagingStore.cpp` (new): contract 14. Marked `/* PILLAR2_WORKER_ONLY */ // est-latency: copy ≤ ~2 s per 100 MiB local; a network path is unbounded`.
11. `Source/Core/include/Config/TunableLimits.h`: contract 16.
12. `Source/Core/include/AppController.h`, `Source/Core/src/AppController.cpp`: `AppController` owns a `std::unique_ptr<AttachmentStagingStore>`, created with the user data directory and released in the destructor's existing join order.
13. `CONTEXT-MAP.md` § Code without a leaf: add the two staging TUs to the Persistence row that already lists `AttachmentDiskCache.cpp`. A new ADR under `docs/adr/` (next free number), "Attachments are staged by copy": the store is hard to undo once queue rows carry staged ids, and copying a file the user only dropped is surprising without the reason.

**Sync (S3)**
14. `Source/Core/include/PendingActionTypes.h`: contract 5.
15. `Source/Core/include/PendingActionPolicyPure.h`, `Source/Core/src/PendingActionPolicyPure.cpp`: contract 6.
16. `Source/Core/include/Sync/PendingActionQueueService.h`, `Source/Core/src/Sync/PendingActionQueueService.cpp`: contract 7.
17. `Source/Core/include/IOfflineQueueDeps.h`, `Source/Core/include/GridContextDepsAdapter.h`, `Source/Core/src/GridContextDepsAdapter.cpp`, `tests/support/FakeOfflineQueueDeps.h`: contract 8.
18. `Source/Core/src/Sync/OfflineQueueService.cpp`: `ReplayOneCreate` resolves staged ids before `Run` and calls `SettleCreateAttachments` after it. A new worker-side `CollectStagedReferences` reads the staged ids referenced by `pending_creates`, `pending_actions` and both dead tables; the startup orphan sweep runs after the cache opens and passes that set to `SweepOrphans`. The sweep is the only code that deletes a staged file whose row or queued create was discarded; such a file stays on disk until it is older than the grace.

**Clipboard (S4; no leaf — beside `Win32PickFiles.cpp`)**
19. `Source/Core/include/ClipboardAttachmentSource.h` (new): contract 11.
20. `Source/Core/include/ClipboardImagePure.h`, `Source/Core/src/ClipboardImagePure.cpp` (new): contract 12.
21. `Source/Core/src/Win32ClipboardAttachments.cpp` (new): the Win32 source under `#if defined(_WIN32)`, a stub in the `#else` branch. Format order: text present → route as text; else `CF_HDROP`; else the registered `"PNG"` format (signature and IHDR dimensions checked through the existing `Ui/ImageDimensionsPure.h`); else `CF_DIBV5`, then `CF_DIB`, converted by the pure helpers and PNG-encoded with WIC, which Core already links in both targets (`Source/Core/CMakeLists.txt:373`, `:407`; `SmatchetImGuiPlugin.Build.cs:112`). `Read` is marked `/* PILLAR2_WORKER_ONLY */ // est-latency: ≤ 1 s typical; unbounded when the owning app renders on demand`.

**App facets (S5; `AppController_IssueCreateOffline.cpp` follows the Sync leaf)**
22. `Source/Core/include/Interfaces/IAppAttachmentStaging.h` (new): contract 10. `Source/Core/include/Interfaces/IAppPendingActions.h`: contract 9.
23. `Source/Core/include/AppController.h`, `Source/Core/src/AppController_Attachments.cpp`: the facet, the staging tickets (results held under a short lock and polled, the pattern of the attachment-preview queue in `SmatchetUI.cpp`), the clipboard source and its test hook.
24. `Source/Core/src/AppController_IssueCreateOffline.cpp`: `LaunchIssueCreate_` calls `SettleCreateAttachments` after `Run`; `SubmitOrQueueCommentWithAttachments` is a thin delegator beside `SubmitOrQueueComment`. Queue logic stays in the services.

**Drop seam (S6; Ui + the standalone host)**
25. `Source/Core/include/Ui/SmatchetFileDrop.h`, `Source/Core/src/Ui/SmatchetFileDrop.cpp`, `Source/Core/include/Ui/FileDropRoutingPure.h`, `Source/Core/src/Ui/FileDropRoutingPure.cpp` (new): contract 15. The claim test uses `ImGui::FindHoveredWindowEx` and `ImGui::GetTopMostPopupModal` (`imgui_internal.h`, present in the pinned ImGui). The hover outline uses the theme's existing `ImGuiCol_DragDropTarget`; no palette token is added.
26. `Source/Core/src/Ui/SmatchetUI.cpp`: `FileDropBeginFrame()` at the top of `SmatchetUI::Draw`, and after all panels `FileDropEndFrameTakeUnclaimed()` hands leftovers to `PlanDocViewerOpenDroppedPaths`.
27. `Source/Standalone/StandaloneBoot_detail.cpp`: `MarkdownFileDropCallback` reads `glfwGetCursorPos(window, …)` and calls `PostFileDrop`. Its name and declaration stay, so the two `glfwSetDropCallback` install sites are not edited.

**Draft row and shared chips (S7a; Ui)**
28. `Source/Core/include/Ui/SmatchetAttachmentChips.h`, `Source/Core/src/Ui/SmatchetAttachmentChips.cpp` (new): one chip strip for both surfaces. A chip shows name, size and state (Preparing…, ready, failed with the reason) and a remove button reachable with Tab; its label is built once when it is staged, not per frame.
29. `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp`: `TryAppendStagedFromAbsPath` and `DrawDraftAttachmentChipStrip` are replaced by `StageFilesAsync` and the shared strip. `DrawDraftIdColumnCell` registers the drop zone (the draft row: the table's horizontal extent by the ID cell's height, which is the row's tallest cell) and hides the attachment controls behind `FocusedBackendSupportsAttachments()`. A new `HandleDraftRowPaste`, called beside `HandleDraftRowKeys`, watches Ctrl+V and Shift+Insert while `s_draftTextActiveId` is the active item. Create is disabled, with a tooltip, while a file is still being prepared. `DrawDraftTextFieldInput` (`:541`) is not edited. Also in this slice, the standalone host's two `glfw_error_callback` definitions (`Source/Standalone/StandaloneAppBootstrap.cpp:109`, `Source/Standalone/main.cpp:115`) stop reporting `GLFW_FORMAT_UNAVAILABLE` as an error: ImGui's own paste asks GLFW for clipboard text, and GLFW 3.3.8 raises that error when the clipboard holds no text, which would write one error line per pasted image.

**Comments (S7b; Ui, after #2439)**
30. `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp`: `DrawCommentsPostBox` registers the drop zone and draws the strip. The zone is the whole comments window, so a drop anywhere on it attaches to the comment being written and never opens Plan Docs behind the window; the outline is drawn around the composer. The footer height reserved before the thread pane (`footerH`, `:677`) grows by the strip's height, which is capped at two rows. The paste watch sits after the `SubmitMultilineInput` call in `DrawCommentsPostInput`. `KickCommentPost` calls `SubmitOrQueueCommentWithAttachments`. `ApplyCommentPostResult` keeps the text and the unsent chips on a failure and names the file. `HasUnsavedInput` (`:687`) also counts staged chips. The chip list is saved and restored through the draft store (contract 18). A restored chip whose staged file is gone shows "file no longer available" and can only be removed.
31. `Source/Core/src/Ui/SmatchetOfflineQueueUi_Actions.cpp`: `KindLabel` returns "Attachment" for the new kind and `ActionPreview` returns the file name and size. The panel's Discard, Restore and Delete actions need no change: they act on row ids through `IAppPendingActions`.

**Tests, guides and maps (S8, and per slice where noted)**
32. New bucket-A files under `tests/Core/`: `FileDropRoutingPure.test.cpp`, `AttachmentStagingPure.test.cpp`, `AttachmentStagingStore.test.cpp`, `ClipboardImagePure.test.cpp`. Extended: `PendingActionPolicyPure.test.cpp`, `PendingActionQueueService.test.cpp`, `IssueDraft.test.cpp`, `IssueCreatePipelineIntegration.test.cpp`, `JiraIssueMappingPure.test.cpp`. Each new file is added to `add_executable(SmatchetTests …)` in `tests/CMakeLists.txt` (configure fails on an unlisted test), and the Sync and staging cases to the `SmatchetTsanTests` subset.
33. New bucket-E files under `tests/ui/`: `attachment_drop_zones.test.cpp`, `attachment_paste.test.cpp`, registered in `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`; `plan_doc_viewer_external_open.test.cpp` and `scripts/dev/test-ui-offline-first.sh` are extended.
34. [`scripts/dev/perf-scenario-map.json`](../../../scripts/dev/perf-scenario-map.json): a rule mapping `SmatchetNewIssueDraftUi.cpp`, `SmatchetCommentsModalUi.cpp`, `SmatchetFileDrop.cpp` and `SmatchetAttachmentChips.cpp` to `["idle", "priority-grid-scroll"]`.
35. `docs/guides/keyboard-shortcuts.md`: Ctrl+V / Shift+Insert attach an image or copied files in the draft's text fields and the comment box; Tab reaches a chip's remove button.
36. `Source/Core/src/SmatchetLocalization.cpp`: the new strings, each through `SmatchetLocalization::T(key, englishFallback)`.
37. `Source/Core/src/Ui/AGENTS.md` (Before you edit, re-check sites): the upload call is now `TrackerPostMultipartFileLogged`, reached from `UploadIssueAttachment`. Glossaries, each with the slice that ships the concept (the glossary records shipped concepts only): `Source/Core/src/Tracker/CONTEXT.md` — **Staged attachment**, and the `IssueDraft` entry points at it (S1); `Source/Core/src/Persistence/CONTEXT.md` — **Staging store** (S2), and **Pending action** with its kinds including the **attachment upload**, an entry that is missing today although comments, worklogs and watches already queue this way (S3).

### File-level changes

Existing symbols this plan edits or calls. New symbols are named in the list above.

| File | Symbol |
|---|---|
| `Source/Standalone/StandaloneBoot_detail.cpp` | `MarkdownFileDropCallback` |
| `Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp` | `PlanDocViewerOpenDroppedPaths` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `TryAppendStagedFromAbsPath` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `DrawDraftAttachmentChipStrip` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `DrawDraftIdColumnCell` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `HandleDraftRowKeys` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `s_draftTextActiveId` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `DrawCommentsPostBox` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `DrawCommentsPostInput` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `KickCommentPost` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `ApplyCommentPostResult` |
| `Source/Core/src/Tracker/JiraIssueMutation.cpp` | `AttachFilesToIssue` |
| `Source/Core/src/Tracker/IssueCreatePipeline.cpp` | `ApplyPostIssueAttachmentStep` |
| `Source/Core/include/Tracker/IssueCreatePipeline.h` | `AttachmentFailures` |
| `Source/Core/include/Tracker/IssueDraft.h` | `StagedAttachment` |
| `Source/Core/include/Tracker/TrackerHttpUtils.h` | `MakeTrackerRedirectPolicy` |
| `Source/Core/include/Tracker/TrackerHttpUtils.h` | `MakeTrackerSslOptions` |
| `Source/Core/include/PendingActionTypes.h` | `ParsePendingActionKind` |
| `Source/Core/include/PendingActionPolicyPure.h` | `StateAfterFailedSend` |
| `Source/Core/include/PendingActionPolicyPure.h` | `BuildCommentActionPayload` |
| `Source/Core/include/PendingActionPolicyPure.h` | `kCommentDedupeWindowSec` |
| `Source/Core/src/Sync/PendingActionQueueService.cpp` | `RunReplayPass` |
| `Source/Core/src/Sync/PendingActionQueueService.cpp` | `ReplayOne` |
| `Source/Core/src/Sync/PendingActionQueueService.cpp` | `CollaborationShared` |
| `Source/Core/src/Sync/PendingActionQueueService.cpp` | `ResolveAmbiguousComment` |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi_Actions.cpp` | `KindLabel` |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi_Actions.cpp` | `ActionPreview` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `HasUnsavedInput` |
| `Source/Standalone/StandaloneAppBootstrap.cpp` | `glfw_error_callback` |
| `Source/Core/include/IOfflineQueueDeps.h` | `MutationsShared` |
| `Source/Core/src/Sync/OfflineQueueService.cpp` | `ReplayOneCreate` |
| `Source/Core/src/AppController_IssueCreateOffline.cpp` | `LaunchIssueCreate_` |
| `Source/Core/src/AppController_IssueCreateOffline.cpp` | `SubmitOrQueueComment` |
| `Source/Core/include/AttachmentCachePure.h` | `SanitizeFileName` |
| `Source/Core/include/Config/TunableLimits.h` | `AttachmentDownloadMaxMiB` |
| `Source/Core/src/Tracker/IssueTableSerializer.cpp` | `ApplyKeyValueToDraft` |

## Existing utilities reused

- `smatchet::attachments::SanitizeFileName` (`Source/Core/include/AttachmentCachePure.h:38`): the staging store and the upload name use it; no second sanitizer.
- `AttachmentDiskCache` / `AttachmentCachePure` (`Source/Core/include/AttachmentDiskCache.h`): the layout model (one directory per id, a metadata file, temp-then-rename writes, path confinement) the staging store mirrors.
- `PendingActionQueueService` and its `pending_actions` / `pending_actions_dead` tables: persistence, at-most-once states (`pending` / `sending` / `ambiguous` / `needs_review`), the attempt cap and the dead-letter twin, all reused by the new kind.
- `StateAfterFailedSend`, `StateAfterFailedDedupeCheck`, `kCommentDedupeWindowSec` (`Source/Core/include/PendingActionPolicyPure.h:28`, `:35`, `:21`): the comment rules, applied to an attachment.
- `LatchPendingActionTarget` (`Source/Core/src/AppController_IssueCreateOffline.cpp:347`): the pane a write is bound to, already latched by `KickCommentPost`.
- `MakeTrackerRedirectPolicy` / `MakeTrackerSslOptions` (`Source/Core/include/Tracker/TrackerHttpUtils.h:31`): no cross-host credential forwarding and the shared trust anchor, kept for uploads.
- `SetMutationCancelToken` (`Source/Core/include/ITrackerIssueMutations.h:28`): the cancel token the upload's progress callback reads.
- `Ui/ImageDimensionsPure.h`: PNG signature and dimension checks for a pasted PNG.
- WIC, already used to decode attachment previews (`SmatchetAttachmentPreviewUi.cpp`): PNG encoding of a pasted bitmap. stb_image_write is deliberately not pulled into Core.
- `PlanDocViewerOpenDroppedPaths` (`Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:641`): the unchanged fallback, with its "not opened" toast.
- `SmatchetToastManager`, `SmatchetLocalization::T`, `ImGuiCol_DragDropTarget`: feedback, strings and the hover outline.
- `smatchet::drafts::Put` / `Get` (#2439): persistence of the composer's chip list.
- `AppController::RequestOpenFilePaths`: the existing picker, now feeding `StageFilesAsync`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted from an over-cap file. The draft's chip strip (about 35 lines) is replaced by the shared `SmatchetAttachmentChips`, and the new behaviour lands in new TUs, so `SmatchetNewIssueDraftUi.cpp` (780 lines) and `SmatchetCommentsModalUi.cpp` (763 lines) grow by call sites only. The Jira per-file block moves within `JiraIssueMutation.cpp`.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: with no drop or drag waiting, `FileDropZone` returns after one flag read and allocates nothing. Chip labels are built once at stage time, which removes the per-frame string building the current strip does.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: copying, hashing, clipboard reads, PNG encoding, SQLite and HTTP all run on workers. The UI thread only asks which clipboard formats exist. This also moves the picker's `is_regular_file` / `file_size` / `equivalent` calls off the UI thread. A chip shows "Preparing…" while its copy runs, and Create / Post are disabled with a tooltip until it finishes.
- **Pillar 3 (never crash)**: clipboard bytes come from another process and dropped paths from the OS; both are parsed bounded, every Win32 failure is handled, and the clipboard is closed by an RAII guard on every path. A staging failure is a chip state, never an exception across the worker boundary.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: paste is a keyboard path to attaching; each chip's remove button is in Tab order and named with its file; the hover outline reuses a themed colour, so no new contrast check is needed.
- **Pillar 6 (offline-first, ADR-0026)**: a staged file is the app's own copy. A create or comment made offline queues its files with it and uploads them on reconnect. An upload that fails becomes a queue row or a dead letter, both visible in the Offline Queue panel. Toasts say what happened: "Queued" only after a real enqueue.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

This PR is the plan only (docs), so no gate fires on it. For the implementation slices:

1. **PR-fast CI** — fires. `idle` (the drop router and the chip strip run every frame) and `priority-grid-scroll` (the draft row is drawn inside the grid). None of the four UI files is in [`scripts/dev/perf-scenario-map.json`](../../../scripts/dev/perf-scenario-map.json) today; S8 adds the rule (item 34), and S6 / S7a name the two scenarios in their PRs until it lands.
2. **Pillar 2 static scanner** — fires. New synchronous I/O exists but only on workers: `AttachmentStagingStore::*`, `IClipboardAttachmentSource::Read`, `TrackerPostMultipartFileLogged`. Each carries `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` (items 10, 21 and contract 17). Nothing new is reachable from `ImGui::*` except `Probe()`, which is four `IsClipboardFormatAvailable` calls.
3. **Dispatcher drain** — N/A. `MainThreadDispatcher::Drain()` is not touched; staging results are polled under a short lock.
4. **Visible-cue bucket-E harness** — fires. Preparing a large file takes longer than 100 ms; the cue is the chip's "Preparing…" state plus the disabled Create / Post, covered in `attachment_drop_zones.test.cpp`.
5. **Marker inventory** — N/A. No permanent `SMATCHET_UI_PERF_SCOPE` marker is added.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `idle` and `priority-grid-scroll` before opening S6, S7a and S7b.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — intentional regression + baseline-bump PR queued only. None is expected.

## Risks / non-goals

**Risks**

- **Trust boundary.** Paths, sizes and clipboard bytes come from outside the app. Mitigation: the checklist below, a `security-review` pass on S1, S2, S4 and S6 before each merges, and bucket-A tables for every rule.
- **Downgrade.** An older build archives an `attachment_add` row as an unknown kind (still restorable) and ignores `awaitStaged`, so a comment may post before its files. Accepted: no data is lost.
- **Disk use.** The store can reach `AttachmentStagingStoreMaxMiB`. When it is full, staging is refused with a message that says so; nothing is evicted, because every entry is a pending upload.
- **Large uploads time out.** A 100 MiB file on a slow link can exceed the timeout and become `ambiguous`. Mitigation: its own `AttachmentUploadTimeoutMs`, and the list check that stops a duplicate upload.
- **Lua and bulk import change slightly.** Their per-file failures are now queued as rows (after a copy into the store) instead of only reported, and the size cap applies. Mitigation: `AttachmentFailures` is still returned unchanged; the change is named in the S1 and S3 PR descriptions.
- **Shutdown.** A long upload would delay the worker join. Mitigation: the progress-callback cancel in contract 17.
- **Strict lint zones (Tracker, Sync).** Mitigation: the upload goes through `TrackerHttpUtils`, so no direct `cpr` call is added or moved.
- **Dual target.** Every new Core TU compiles in `SmatchetCore_DX12`. Mitigation: no GLFW or host type in any Core header; the Win32 TU has a stub branch for Linux, macOS and Android; each slice runs the dual-target build gate.
- **Save formats.** SQLite: a new `kind` value only. Draft JSON: one additive key. Config: five new tunables with defaults. Views, the C ABI and the MCP wire format are untouched.
- **The duplicate check is a heuristic.** `AttachmentAlreadyUploaded` matches name, size and a 5-minute window, the rule comments already use. A tracker clock more than 5 minutes behind can cause a second upload; a different file with the same name and size, attached by someone else inside the window, can make the queued one count as sent. Accepted, as for comments; both cases need an ambiguous send first.
- **Non-ASCII paths on Windows.** The upload hands libcurl a path (`cpr::File`), as it does today, and libcurl opens it with the ANSI code page. The store sits under the user data directory and keeps the sanitized name, either of which can hold non-ASCII characters. Mitigation: an S1 probe (OQ6); if the open fails, the helper streams the file through a read callback instead of passing a path.
- **Disk held after a discard.** A discarded row's file waits for the orphan sweep (startup, older than the grace). Accepted for one deleter and no race.
- **Open-PR overlap.** #2439 edits `SmatchetCommentsModalUi.cpp`, `SmatchetUI.cpp`, `AppController.cpp` and `keyboard-shortcuts.md`. Mitigation: § Implementation handoff orders S7b behind it and has S2, S6 and S8 rebase.
- **Unsupported tracker.** On Plane, GitHub and Linear the zones still claim a drop and answer with a toast ("This tracker does not accept attachments from Smatchet yet"), so the file is not sent to the Plan Docs viewer by surprise.
- **Localization.** New keys with English fallbacks; other languages follow the usual table update.

**Security checklist (carried by the `security-review` pass)**

1. **Source paths.** Reject empty paths, an embedded NUL, relative paths and the `\\?\` / `\\.\` device namespaces. Regular files only: no directory, symlink, junction or other reparse point. Open first, then check size and type on the open handle. Copy in a bounded loop that stops at the cap plus one byte, hashing as it goes.
2. **Caps before any read.** Per file, files per submit (also the limit on `CF_HDROP` entries) and the store total. The Jira upload re-checks the size.
3. **Clipboard images.** A DIB is accepted only with a known header size, sides ≤ 16384, pixels within `ClipboardImageMaxMegapixels`, 24 or 32 bits, `BI_RGB` or `BI_BITFIELDS`, and a size that fits the clipboard block. A PNG is accepted on signature, IHDR dimensions and the byte cap. Anything else is "unsupported image" and is never decoded.
4. **Names.** File names are sanitized; directory names are random ids with no user data; the uploaded name is the stored name.
5. **Confinement.** Replay resolves a staged id through the store and ignores a stored path. A queue payload never holds a source path. Deletion touches only id-shaped directories under the store root.
6. **Logs and audit.** Staged ids and sanitized names only; no source path beyond what is logged today. The audit trail records the `issue_attach_file` pair (name, size, issue key) and the queue's existing rows.
7. **No new automation surface** (contract 19).
8. **Transport.** The tracker redirect policy and TLS options are reused; single attempt.

**Non-goals** are listed under § Out of scope.

## Implementation handoff

Nine PRs in order. Each slice claims a plan-lock on its write set (`lock-claim.sh`) before coding and ships its own bucket-A tests. Owner is the leaf `AGENTS.md` that governs the write set (`CONTEXT-MAP.md`).

- **S1 — tracker upload and list.** *Owner:* Tracker. *Write set:* items 1–8, their tests. *Decisions:* `AttachFilesToIssue` keeps its signature and meaning; single attempt, no retry; the capability defaults to false and only Jira overrides it; HTTP through the new `TrackerHttpUtils` helper; every failure keeps its error kind. *Inventory:* `JiraIssueMutation.cpp:521` (function), `:557`–`:612` (per-file block); `IssueCreatePipeline.cpp:157`, `:173`, `:184`; `IssueCreatePipeline.h:37`; `IssueDraft.h:13`–`:17`; `IssueDraft.cpp:236`–`:245`, `:282`–`:292`; `ITrackerIssueMutations.h:28`, `:69`–`:73`; `FakeTrackerClient.h` (`AttachFilesToIssue` override). *Order:* first; nothing depends on an open PR. Three probes at the start (OQ6): the Jira upload response, the cpr progress callback, a non-ASCII path.
- **S2 — staging store.** *Owner:* Persistence rules (no leaf). *Write set:* items 9–13, tests. *Decisions:* path-confined; only id-shaped directories are ever deleted; caps before any read; the pure TU includes no `Logger.h`. *Inventory:* `AttachmentCachePure.h:18`, `:38`; `TunableLimits.h:71`–`:72`. *Order:* parallel with S1. `AppController.cpp` overlaps #2439 by a few lines; rebase.
- **S3 — queue rows and replay.** *Owner:* Sync. *Write set:* items 14–18, tests. *Decisions:* one row per file; no schema or `ISyncCache` change; a waiting comment spends no attempt and waits only on `pending` / `sending` / `ambiguous` files; a create's final leftover is enqueued then archived as `upload_rejected`; never a blind resend; `Discard` and `DeleteDead` stay as they are and the sweep deletes; replay reuses `IssueCreatePipeline::Run`. *Inventory:* `PendingActionTypes.h` (enum, `PendingActionKindWire`, `ParsePendingActionKind`); `PendingActionPolicyPure.h:21`, `:28`, `:35`, `:49`; `PendingActionQueueService.cpp:43`, `:79`–`:118`, `:197`, `:218`, `:245`, `:260`, `:276`, `:324`, `:341`, `:398`; `IOfflineQueueDeps.h:57`, `:62`; `OfflineQueueService.cpp:1408`. *Order:* after S1 and S2.
- **S4 — clipboard reader.** *Owner:* no leaf (Core root, beside `Win32PickFiles.cpp`); Ui rules for the thread contract. *Write set:* items 19–21, tests. *Decisions:* `Probe()` is the only UI-thread call; `Read()` is worker-only; WIC encodes; the stub branch compiles on Linux and Android. *Order:* parallel with S1–S3. Probe at the start: an Explorer file copy carries no text format (OQ6).
- **S5 — app facets.** *Owner:* Sync rules for `AppController_IssueCreateOffline.cpp`; otherwise no leaf. *Write set:* items 22–24. *Decisions:* the TU only delegates; the comments surface asks its latched target for support. *Inventory:* `AppController_IssueCreateOffline.cpp:123`, `:188`–`:198`, `:383`–`:390`; `IAppPendingActions.h:30`. *Order:* after S2, S3 and S4.
- **S6 — drop seam.** *Owner:* Ui (plus the standalone host file). *Write set:* items 25–27, tests. *Decisions:* no GLFW in Core; UI thread only; first claimant wins; a modal blocks the windows behind it; the fallback is today's Plan Docs behaviour; the C ABI is untouched. *Inventory:* `StandaloneBoot_detail.cpp:92`–`:102`; `SmatchetPlanDocViewerUi.cpp:641`. *Order:* any time; independent of S1–S5. `SmatchetUI.cpp` overlaps #2439 by four lines; rebase.
- **S7a — draft row and shared chips.** *Owner:* Ui. *Write set:* items 28–29. *Decisions:* `DrawDraftTextFieldInput` (`:541`) is not edited — it belongs to the description-templates plan (#55); the paste watch keys on `s_draftTextActiveId` (`:43`, set at `:557` and `:578`), so it covers every draft text field; no `SmatchetActiveProjectGrid*` or `SmatchetGrid*` file is edited. *Order:* after S5 and S6.
- **S7b — comments.** *Owner:* Ui. *Write set:* items 30–31. *Decisions:* the zone is the whole comments window; the chip list is persisted through the draft store under its own surface; the composer's text handling is not changed. *Inventory:* `SmatchetCommentsModalUi.cpp:495`, `:540`, `:583`, `:605`, `:616`, `:677`, `:687`; `SmatchetOfflineQueueUi_Actions.cpp:48`, `:92`; #2439's `CommentsDraftScope`. *Order:* after S5, S6 and after #2439 merges.
- **S8 — bucket E, perf map, guides.** *Owner:* tests and docs. *Write set:* items 33–37. *Order:* last; `tests/CMakeLists.txt` and `SmatchetLocalization.cpp` are edited per slice as hunks appended to their lists.

**Sibling Wave 3 plans.** Grid as a spreadsheet (#2461) handles Ctrl+V only when the grid is focused and `!io.WantTextInput`; this plan handles it only while a text item is active, so the two never see the same key press, and no grid file is in any write set here. Filter without JQL works in `SmatchetGridHeaderUi`: no overlap. Description templates / preview / mentions (#55) edits `DrawDraftTextFieldInput` and the comment composer's text features in the same two files as S7a and S7b but different functions; whichever lands second rebases.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**:
  - `FileDropRoutingPure.test.cpp`: rect containment and edges, the top-window rule, the modal block, first claimant, fallback.
  - `AttachmentStagingPure.test.cpp`: the `ClassifySourcePath` table, `IsStagedId`, metadata round trip and tamper rejection, `SelectOrphans` (referenced, inside and past the grace, malformed).
  - `AttachmentStagingStore.test.cpp` (temp directory): copy, the size cap including a file that grows while copied, the store cap, a symlink rejected (POSIX lane), `Resolve` confinement, the sweep.
  - `ClipboardImagePure.test.cpp`: a DIB header table (top-down, bottom-up, bitfields, oversize, truncated), `DecidePasteRoute`.
  - `PendingActionPolicyPure.test.cpp`: the attachment payload round trip, the `StateAfterFailedSend(AttachmentAdd)` matrix, `AttachmentAlreadyUploaded`, the comment wait rule, and a legacy comment payload that stays byte-identical.
  - `PendingActionQueueService.test.cpp` with `FakeOfflineQueueDeps` and `FakeNetworkMode::TransportDown`: queued offline then sent once; ambiguous then found in the list, so no resend; a waiting comment spends no attempt; a restored dead row keeps the order; a rejected file is archived and its staged file kept.
  - `IssueDraft.test.cpp`, `IssueCreatePipelineIntegration.test.cpp`, `JiraIssueMappingPure.test.cpp`: the `stagedId` round trip and an old payload; per-file leftovers, a hard error after create, an unsupported backend; the attachment parse.
  - Windows unit lane: one serialized case writes a `CF_DIB` with `SetClipboardData` and reads it back through the real Win32 source.
- **Bucket D (sanitizer)**: the new Sync and staging cases join the `SmatchetTsanTests` subset; the clang-cl ASan / UBSan lanes cover the DIB parsing.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**:
  - `attachment_drop_zones.test.cpp`: `PostFileDrop` at the draft row and on the comments window (over the composer and over the thread pane) adds a chip with name, size and state; `PostFileDragOver` outlines the zone and `PostFileDragLeave` clears it; a drop under a modal is not claimed; Tab reaches a chip's remove button and Enter removes the chip; Create / Post are disabled while a file is preparing; a tracker without attachments answers with the toast.
  - `plan_doc_viewer_external_open.test.cpp` (extended): a `.md` dropped outside every zone still opens Plan Docs.
  - `attachment_paste.test.cpp`: a fake clipboard source. An image adds a chip; text pastes as text with no chip; copied files add chips.
  - `scripts/dev/test-ui-offline-first.sh` (extended) with the deterministic Jira fixture: a create and a comment with a file while offline, the queue rows, the replay, and the fixture recording each upload exactly once.
  - Windows only: a synthesized `WM_DROPFILES` posted to the GLFW window proves the real standalone callback forwards the drop position.
- **Bash-driver scenario / screenshot / sanitizer**: none beyond the offline script above. No CLI command is added, so there is no CLI probe.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on every slice; `cmake --preset ninja-test-linux` for the stub branch of `Win32ClipboardAttachments.cpp`.
- **Review gates**: `security-review` on S1, S2, S4 and S6 (the trust-boundary slices); `pre-merge-review` on every slice.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required. Also `bash agent-layer/agents/scripts/core/plan-doc-table-probe.sh` on this file.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete. *Outcome (2026-11-20, run without a live interview: each question was settled from the code and the glossaries, and what only a person can decide is in § Open questions):*
  - *Terms.* "Pending" already means "waiting for replay" in the Persistence glossary, so the chips are **staged attachments**, never "pending attachments"; the queue row is an **attachment upload**. The glossary has no **Pending action** entry although three kinds exist; S3 adds it. No glossary file is edited by this plan-only PR, because the glossary records shipped concepts.
  - *Storage shape.* Checked against the table definitions (contract 5): a new `kind` value needs no schema change, and an older build archives it as `unknown_kind`.
  - *Wrong file.* Queue-row labels are drawn in `SmatchetOfflineQueueUi_Actions.cpp` (`KindLabel`, `ActionPreview`), not in `SmatchetOfflineQueueUi.cpp`; item 31 was corrected, which also removes an overlap with #2439.
  - *Scenario: a comment whose file is rejected or needs review.* The first draft would have held the comment for ever. The wait rule now names the three states that hold it (contract 6).
  - *Scenario: discard a queued upload while it is being sent.* Deleting the file from `Discard` could race the upload. The sweep is now the only deleter (contract 7, item 18).
  - *Scenario: a drop on the comments window but outside the composer.* It would have opened Plan Docs behind the window. The zone is now the whole window, and the claim rule compares root windows so the thread pane's child region does not hide it (contract 15, item 30).
  - *Scenario: paste an image in the standalone app.* From the pinned GLFW source, ImGui's own paste makes GLFW raise "format unavailable", which the host logs as an error; S7a lowers it (item 29).
  - *Scenario: close the comments window with a chip and no text.* `HasUnsavedInput` looks at text only; item 30 adds the chips.
  - *Cross-checks that held.* A live final rejection returns `Failed` without a row (`PendingActionQueueService.cpp:113`–`:118`), which is what the comment path relies on; the create path uses the focused pane's backend, so `FocusedBackendSupportsAttachments()` is the right question for the draft row.
  - *ADR.* Staging by copy meets the bar (hard to undo, surprising, a real trade-off); S2 writes it (item 13).
- **Manual residue**: two items, each with a deferred-automation plan and a `docs/self-improvement/categories/tooling.md` entry filed by S8.
  - A real Explorer drag with the OS hover cursor: there is no OS drag injection in the rig. The seam and the real callback are covered; native hover arrives with the `IDropTarget` follow-up, which brings its own test.
  - Ctrl+V reaching the Unreal plugin's input processor: there is no Unreal CI lane. A probe by the Unreal follow-up's owner (OQ6).

## Open questions

1. **Should a comment name its files?** Jira's REST API cannot embed an uploaded attachment in a comment body. The plan's default adds nothing to the user's text: the files appear in the issue's attachment list. The alternative is one appended line, "Attached: a.png, b.log", which can name a file whose upload later failed. *Answers:* user.
2. **Is 100 MiB the right default upload cap, and should Jira's own limit refine it?** `GET /rest/api/3/attachment/meta` reports whether attachments are enabled and the site's limit. *Answers:* user for the default; the Tracker owner for the meta call, at S1.
3. **Network (UNC) paths: allow or reject?** The plan allows them, because the user picked the file and the copy runs on a worker. *Answers:* user, then the `security-review` pass on S2.
4. **Native hover highlight in this plan or after it?** #137 expects the target to be highlighted while a file is dragged over it. GLFW reports only the drop, so that needs an OLE `IDropTarget` on the standalone window. The plan builds and tests the seam for it and leaves the host part as a follow-up. *Answers:* user.
5. **Can #2439's draft store list a surface?** A `ListSurface(surface, out)` call would let the orphan sweep see files referenced by a saved comment draft. Without it, such a file is protected only by the 7-day grace, and a draft older than that restores a chip marked "file no longer available". *Answers:* the #2439 owner; S7b proceeds either way.
6. **Probes, each run at the start of its slice.** S1: the Jira upload response includes `size` and `created`; `cpr::ProgressCallback` in the pinned cpr 1.9.2 can abort an upload (nothing in the tree uses it yet); a staged file under a non-ASCII path uploads on Windows. S4: an Explorer file copy carries no text format, so it routes as files. S7a: ImGui's own Ctrl+V with no text on the clipboard inserts nothing, and GLFW raises "format unavailable" once per paste. Unreal follow-up: Ctrl+V reaches `SmatchetImGuiInputProcessor`. Two probes are already answered from the pinned sources: GLFW 3.3.8 moves the cursor to the drop point before it fires the drop callback (`win32_window.c`, `WM_DROPFILES`), and the pinned ImGui has `FindHoveredWindowEx` and `GetTopMostPopupModal`.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **File drop in the Unreal plugin.** The input processor never sees an external drag; it needs a Slate drop target and the C ABI pair. Follow-up plan. Paste works in Unreal with this plan, pending the probe in OQ6.
- **Native hover highlight in the standalone app** (`IDropTarget`). Follow-up, unless OQ4 pulls it in.
- **Upload on Plane and Linear.** Both use a presigned-upload flow; each is a follow-up that implements `UploadIssueAttachment` and turns the flag on. GitHub has no attachment API: no action.
- **Attachments in Quick Create** (`SmatchetQuickCreateIssueUi.cpp`). Follow-up; it reuses the shared strip and the facet.
- **Adding a file to an existing issue's attachment list** (the "attachment area" in #137). Follow-up; the `AttachmentAdd` row makes it small.
- **Inline images in descriptions and comments.** Belongs with the description plan (#55).
- **Keeping the new-issue draft across a restart** (#51, the new-issue half). Until that lands, a file staged on an unsent new-issue draft is kept on disk for the 7-day grace but the draft itself is gone after a restart.
- **A markdown drop taking focus** (#139): unchanged outside the two zones. No action here.
- **A command, MCP tool or Lua binding that attaches a file.** No action: it would be a new way to send a local file to a tracker.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
