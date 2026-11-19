# Plan — Input UX Wave 2, piece 4.3: drafts and undo

> **Slug**: `input-ux-w2-drafts-undo` (matches this file's basename without `.md`).
>
> **Status**: `active` — Wave 2 piece 4.3 of `docs/audits/INPUT_UX_AUDIT.md` § 4.3, stacked on the Wave 1 branch (PR #2433).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

`docs/audits/INPUT_UX_AUDIT.md` § 4.3: typed work lives in memory only. Esc, a closed window, a failed save or a crash discards it, and destructive actions rely on confirm dialogs because nothing can be undone. The piece closes, per the audit, #7, #14, #28, #31, #33, #39, #50, #51, #56, #74, #87, #90, #103, #110, #116 and #141 (finding detail in § 7).

Wave 1 (PR #2433, this branch's base) already closed part of that list, and PR #2435 (branch `claude/input-ux-wave1-stack`) carries more:

| Finding | State before this plan | Where |
|---|---|---|
| #14 palette failure keeps the form | closed | #2433 `6b32a10` |
| #74 Preferences Assistant close guard | closed | #2433 `964c9ca` |
| #87 long-text Esc asks first | closed | #2433 `9908f71` |
| #39 long-text Esc / X / Cancel | the confirm half closed with #87; no draft survives a close or a crash | #2433 `9908f71` |
| #110 conflict resolver Cancel | kept per queued edit, in memory, until the app closes | #2433 `8e51e3b` |
| #7, #50 Quick Create Esc | dirty guard + Enter submit | #2435 `6e08cfe` (not on this branch) |
| #56, #51, #103, #90, #116, #141, #28, #31, #33 | open | — |

After this lands: typed text in Comments, the long-text editor, Log work, the assistant composer and the conflict resolver is saved as it is typed, survives a close, a restart and a crash, and comes back when the same surface reopens on the same issue. Clearing the chat keeps its confirm and then offers an Undo button (and Ctrl+Z) on the toast (deleting a view was dropped from this piece, see Deviations). No Esc drops typed text without either asking or keeping it.

## Approach

**A draft store** (`DraftStore`, new, root of `Source/Core`): an in-memory map keyed by *(surface, scope)* — scope is the issue (plus the tracker site where the surface knows it) — whose entries hold the typed text, an optional *base* (what the field held when the draft started), and a timestamp. The UI thread writes it on every edit (a map assignment under a short mutex; no I/O). A single worker thread owns the file: it loads `smatchet_drafts.json` from the user-data directory once at start (bounded read, bounded JSON parse), merges it under any draft typed meanwhile, and writes the whole map back through `ConfigManager::AtomicWriteTextFile` (temp + fsync + rename, mode 0600) when the map changes, debounced. A crash loses at most the last debounce window; a clean exit flushes. The pure part (the bounded map, LRU eviction, the JSON round-trip, the debounce decision) is header-only (`DraftStorePure.h`) so it is unit-tested on both the Windows suite and the Linux subset.

**An undo action on the existing toast**: `SmatchetToastManager` gains `PushUndoable(title, message, durationMs, undo, commit)`. The toast draws an Undo button and names Ctrl+Z; Ctrl+Z (no text field active, no modal open) undoes the newest live undoable toast. The pending action lives in a small pure ledger (`Ui/UndoLedgerPure.h`) keyed by toast id: Undo runs the undo callback and drops the commit; every other way the toast leaves the screen (expiry, dismissal, eviction at the live cap, `DismissAllLive`, shutdown) runs the commit exactly once. Chat clear keeps its confirm dialog (piece 4.1 moved it onto `ConfirmFooter` in #2437 and its tests assert it) and gains an Undo on the toast of the confirmed action; view delete would do the same but is out of this piece's file set (see Deviations). Dropping the confirms, as the audit suggests for soft-deletable local state, is then a one-line follow-up for whoever owns the dialog contract.

**Esc guards**: Comments and the long-text editor keep their Discard / Keep question (the field viewer's model), and their drafts now persist; Discard erases the draft. Log work's second Esc (and Cancel) closes and keeps what was typed, restored on the next open with a line that says so and a Clear button — the "kept" branch of the § 3 contract, without building a second confirm (that is piece 4.1's). Esc during an armed hide-drag of an edge panel disarms the hide (#141).

## Interface contracts

New shared contracts (each needs this entry per `Source/Core/src/Ui/AGENTS.md` § Before you edit):

1. **`Source/Core/include/DraftStorePure.h`** (new, header-only, namespace `smatchet::drafts`):
   - `struct DraftEntry { std::string Text; std::string Base; std::int64_t UpdatedUnixMs = 0; };`
   - `struct DraftLimits { std::size_t MaxEntries = 200; std::size_t MaxEntryBytes = 160 * 1024; std::size_t MaxTotalBytes = 4 * 1024 * 1024; std::size_t MaxSurfaceBytes = 64; std::size_t MaxScopeBytes = 512; };`
   - `class DraftMap` — `PutResult Put(surface, scope, text, base, nowMs)` (empty text erases; an entry over `MaxEntryBytes` or a key over its cap is `Rejected` and any older entry under that key is erased, so a stale shorter draft never comes back); `bool Get(surface, scope, DraftEntry&) const`; `bool Erase(surface, scope)`; `bool EraseIfText(surface, scope, text)`; `std::size_t EraseSurface(surface)`; `Size()`, `TotalBytes()`; `std::string ToJson() const`; `static bool FromJson(const std::string&, const DraftLimits&, DraftMap&, std::string& err)` through `json_safe::ParseBounded`; `void MergeUnder(const DraftMap& loaded)` (keys already present win). Eviction: oldest `UpdatedUnixMs` first until both caps hold.
   - `bool DraftWriteDue(bool dirty, steady time lastChange, steady time firstUnsavedChange, steady time now)` — quiet ≥ 750 ms, or unsaved ≥ 2 s.
   - File format v1: `{"version":1,"drafts":[{"surface":…,"scope":…,"text":…,"base":…,"updated":<unix ms>}]}`. An unknown version or a malformed file loads nothing and is left on disk untouched until the next write replaces it (logged once).
2. **`Source/Core/include/DraftStore.h`** (new; `Source/Core/src/DraftStore.cpp`), free functions in `smatchet::drafts` over one file-static store, mirroring `ConfigSaveWorker.h`: `Start(path)`, `Stop()` (write what is pending, then join; idempotent), `IsLoaded()`, `Put(surface, scope, text, base = "")`, `Get(surface, scope, DraftEntry&)`, `Erase`, `EraseIfText`, `EraseSurface`, `DefaultPath()` (`ConfigManager::GetUserDataDirectory() + "smatchet_drafts.json"`), and `ResetForTests()`. Without `Start` (unit tests, CLI) it is an in-memory store. Thread-safety: every function may be called from any thread; the UI thread never waits on file I/O (the mutex is never held across I/O: the worker copies the map under the lock, then serializes and writes outside it).
3. **`Source/Core/include/Ui/UndoLedgerPure.h`** (new, header-only, `smatchet::undo::UndoLedger`): `std::uint64_t Add(std::function<void()> undo, std::function<void()> commit)`; `bool Undo(id)`; `bool Commit(id)`; `void CommitAll()`; `bool Contains(id) const`; `std::size_t Size() const`. Callbacks run after the entry is removed, so a callback may add, undo or commit others. Bounded at 32 entries: an overflow commits the oldest.
4. **`Source/Core/include/Ui/SmatchetToast.h`** (widened): `void PushUndoable(const std::string& title, const std::string& message, int durationMs, std::function<void()> undo, std::function<void()> commit)`; `bool UndoNewest()` (what Ctrl+Z calls); `void CommitPendingUndos()` (shutdown); `std::size_t LiveUndoableCount() const` (tests). `ToastNotification` gains `std::uint64_t UndoId = 0`. Existing `Push` / `PushClickable` / `HasLive` behave as before.
5. **`Source/Core/include/Ui/SmatchetFieldViewerModalUi.h`** (widened): `IFieldViewerContent::OnDiscard()` (default no-op), called by the shell when the user picks the discard button, before `OnClose()`. Comments uses it to erase its draft; a plain close or a replace keeps it.
6. **`Source/Core/include/Ui/SmatchetBottomPanelDragPure.h`** (widened): `bool ShouldHideOnDragEnd(bool armed, bool released, bool escCancelled)`.

Draft keys (surface → scope → base), fixed here so later surfaces reuse the shape:

| Surface | Scope | Base | Erased when |
|---|---|---|---|
| `comments` | `<backend key>\|<issue id>` (`PendingActionTarget::BackendKey`) | — | posted (`EraseIfText` with the posted body), or Discard |
| `longtext` | `<issue id>\|<field id>` | the seed shown in the editor | Save, or Discard |
| `worklog.time`, `worklog.description` | `<issue id>` | — | saved, or Clear, or emptied |
| `assistant` | (empty) | — | sent (the box is emptied) |
| `offline.conflict` | `<queue row id>` | the resolver's seed | saved, Use Mine / Theirs, Start over |

## Files to modify

**New**
1. `Source/Core/include/DraftStorePure.h` — the bounded map, JSON round-trip, write-due decision (header-only so the Linux subset runs its tests).
2. `Source/Core/include/DraftStore.h`, `Source/Core/src/DraftStore.cpp` — the store service and its file worker (picked up by `CORE_SOURCES` glob).
3. `Source/Core/include/Ui/UndoLedgerPure.h` — the pending-undo ledger (header-only).

**Undo toast**
4. `Source/Core/include/Ui/SmatchetToast.h`, `Source/Core/src/Ui/SmatchetToast.cpp` — `PushUndoable`, the Undo button, Ctrl+Z, commit on every removal path.

**Surfaces**
5. `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` — save on edit, restore on open (no prefill), "draft restored" line, erase on post / Discard (#56, #51 comments part).
6. `Source/Core/include/Ui/SmatchetFieldViewerModalUi.h`, `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp` — `OnDiscard()`.
7. `Source/Core/src/TicketFieldEditor_Modal.cpp` — long-text draft save / restore / erase, restored banner with "Use saved text" (#39).
8. `Source/Core/src/TicketFieldEditor_Worklog.cpp`, `Source/Core/src/TicketFieldEditor_Worklog.h` — worklog draft; Cancel and the second Esc keep it; restored line + Clear; snapshot field for tests.
9. `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp` — assistant draft save / restore at hydrate (#103); chat clear: confirm, then Undo on the toast; the delete is committed early when a new message is sent, and Clear is disabled while a reply is coming in (#90).
10. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` — conflict drafts move from the in-memory map to the store (#110 survives a restart).
11. ~~`Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` — view delete: confirm, then Undo on the toast~~ **Dropped from this piece** (plan-lock `pane-picker-jira-sites` took the file on 2026-11-19; see Deviations). The branch has no change to it.
12. `Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp` — Esc during the drag disarms the hide (#141).

**Lifecycle**
13. `Source/Core/src/Ui/SmatchetUI.cpp` — `drafts::Start(DefaultPath())` in `drawInitConfigOnce`.
14. `Source/Core/src/AppController.cpp` (`~AppController`, reached by both the standalone and the Unreal host) — `CommitPendingUndos()` then `drafts::Stop()`, before `chat_persist::Stop()`.

**Tests** (hosts chosen so no locked list file changes; see § Risks)
15. `tests/Core/OfflineFirstPure.test.cpp` — `DraftMap` cases (both suites).
16. `tests/ui/field_viewer_modal.test.cpp` also hosts the `DraftStore` worker round-trip (bucket E): `SmatchetTests` links an explicit source list, `DraftStore.cpp` is not in it, and `tests/CMakeLists.txt` is held by sibling plan-locks.
17. `tests/Core/ToastHistoryPure.test.cpp` — `UndoLedger` cases; `tests/Core/ToastManagerLiveCap.test.cpp` — toast commit-on-eviction / undo cases.
18. `tests/Core/SmatchetBottomPanelDragPure.test.cpp` — `ShouldHideOnDragEnd`.
19. `tests/ui/field_viewer_modal.test.cpp` — comments and long-text restore-on-reopen; Esc dirty guard keeps the draft.
20. `tests/ui/ai_chat_panel.test.cpp` — chat clear Undo button and Ctrl+Z (replaces the confirm-modal case).
21. `tests/ui/duration_inline_edit_commit.test.cpp` — Log work Esc keeps and restores the draft.

**Docs**
22. `docs/guides/keyboard-shortcuts.md` — Ctrl+Z on an undo toast; Log work's kept draft.
23. This plan.

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Ui/SmatchetToast.cpp` | `SmatchetToastManager::Push` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `OpenCommentsModal` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp` | `RenderFieldViewerModal` | `CORE_SOURCES` |
| `Source/Core/src/TicketFieldEditor_Modal.cpp` | `OpenLongTextEditor` | `CORE_SOURCES` |
| `Source/Core/src/TicketFieldEditor_Worklog.cpp` | `OpenWorklogDialog` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp` | `HydrateFromConfigOnce` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` | `KeepConflictDraft` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp` | `TickHideGesture` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetUI.cpp` | `drawInitConfigOnce` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetImGuiHost.cpp` | `SmatchetImGuiHost::Shutdown` | `CORE_SOURCES` |

## Existing utilities reused

- `ConfigManager::AtomicWriteTextFile` (`Source/Core/src/Config/ConfigManager_PathUtils.cpp`) — temp + fsync + rename at 0600; the drafts file can hold anything the user typed.
- `ConfigManager::GetUserDataDirectory` — the same directory as `smatchet_views.json`; UI tests isolate it through `ui_test_isolate_home`.
- `smatchet::json_safe::ParseBounded` (`Source/Core/include/Json/BoundedJsonParse.h`) — the loaded file is untrusted bytes.
- `smatchet::config_save` (`ConfigSaveWorker.h`) — the Start / Stop / coalesce / not-running-fallback shape the store copies.
- `smatchet::dialogkeys::ResolveEscape` (`Ui/DialogKeysPure.h`) — Log work's Esc decision, unchanged; only what Cancel does with typed text changes.
- `SmatchetToastManager` (`Ui/SmatchetToast.h`) — the existing toast; no second notification surface.
- `smatchet::ai::chat_persist::Enqueue(ClearAll)` — the deferred commit of a chat clear.
- `AppController::LatchPendingActionTarget` — the tracker site for the comments draft key.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted or split; the conflict-draft map is replaced in place.

## UX Pillar callouts

- **Pillar 1 (perf)**: a draft write on the UI thread is one map assignment of the changed text (≤ 64 KiB) under an uncontended mutex, only on frames where the field reported an edit; nothing per frame otherwise. The toast adds one button per undoable toast.
- **Pillar 2 (UI never freezes)**: all file I/O (load, serialize, write) runs on the store's worker; the UI thread never holds the mutex across I/O and the worker holds it only to copy the map. Shutdown is the one place the store waits: `Stop` writes what is pending and joins the worker (one small file).
- **Pillar 3 (never crash)**: bounded read (16 MiB cap) and bounded parse of an untrusted file; malformed input loads nothing; every callback in the ledger runs once; the worker owns a `std::thread` joined in `Stop()` and in the destructor; no detach.
- **Pillar 4 (accessibility)**: Undo has a key (Ctrl+Z) and the toast prints it; the restored-draft lines are text, not colour.
- **Pillar 6 (offline-first)**: the user's typed work survives a crash, a restart and an offline session; nothing here writes to the tracker. The comments draft is namespaced by tracker site so two sites never share one.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires; the grid scenarios are unaffected (no per-cell path touched); the toast and modal paths draw only while open.
2. **Pillar 2 static scanner** — fires; the only file I/O is in `DraftStore.cpp`'s worker, marked `/* PILLAR2_WORKER_ONLY */ // est-latency: <5ms`.
3. **Dispatcher drain** — N/A; nothing is posted to `MainThreadDispatcher` (the store's state is read under its own mutex).
4. **Visible-cue bucket-E harness** — N/A; no new sync-stall path.
5. **Marker inventory** — N/A; no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: the Linux container cannot run the perf scenarios (Windows exe); CI's Perf PR-fast lane is the check.

## Risks / non-goals

- **Sibling plan-locks.** `SmatchetLocalization.cpp` (held by `bulk-edit-rows`, `input-ux-wave1-palette`, `shared-dropdown-recents`), `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`, `Ui/SmatchetUiSession.h`, `TicketFieldEditor.cpp`, `SmatchetGridFieldEditPipeline.cpp`, the grid cell / table TUs, `SmatchetQuickCreateIssueUi.cpp` and `SmatchetNewIssueDraftUi.cpp` are claimed by open sibling PRs. Mitigation: this write set avoids all of them — new tests are hosted in already-listed test files, new UI cases go into already-registered suites, state that would sit on `UiDrawSession` is file-static in the owning TU, and new strings use `SmatchetLocalization::T` with English fallbacks (the fr-FR rows follow once the lock lifts; Wave 1 did the same).
- **A restored draft lands on a changed field.** The long-text and conflict drafts carry their base; the long-text banner says when the saved text changed since, and "Use saved text" drops the draft. Accepted for comments and the worklog (they append; nothing is overwritten).
- **Test isolation.** A draft one UI case leaves behind would prefill the next case. Each touched suite calls `drafts::ResetForTests()` in its preamble.
- **Chat clear during the undo window, then the app exits.** `CommitPendingUndos()` runs before `chat_persist::Stop()`, so the clear is written. A crash inside the window leaves the rows on disk: the history comes back on the next start (safe direction, accepted).
- **Dual-target.** `DraftStore.cpp` uses only `<thread>`, `<mutex>`, `<condition_variable>`, `<fstream>` and `ConfigManager`; no GLFW / GL includes in `Source/Core` headers.
- Non-goals: grid edit undo and paste (#28, #31, #33 — locked files, and § 5.3 Wave 3), Quick Create / new-issue drafts (#7, #50 in #2435; #51's new-issue half needs the locked draft-row files), a second confirm-dialog helper (piece 4.1).

## Implementation handoff

Single slice, owner `Source/Core/src/Ui/` (the store is a root TU with no leaf; its persistence follows `Source/Core/src/Persistence/AGENTS.md` § Invariants "off the UI thread"). Order: `DraftStorePure.h` + `DraftStore` → `UndoLedgerPure.h` + toast → surfaces → tests → docs. Pre-resolved: no SQLite (a draft is not a lookup, a queued action or cached tracker data, and must exist when the local cache is off); no new `kind` in `pending_actions`.

## Verification

- **Bucket A (doctest)**: `DraftMap` — save, restore, key isolation (surface and scope), empty-text erase, size bounds (entry, key, total, count) with LRU eviction, `EraseIfText`, JSON round-trip, malformed / wrong-version / oversized input, `MergeUnder` precedence, `DraftWriteDue`. `DraftStore` worker (bucket E, see Deviations): a draft reaches the file with no `Stop` (a crash keeps it), and `Stop` then `Start` brings it back. The remaining worker cases (session text wins over the file, malformed file, in-memory use, restart) ran in a local harness linked against the project objects, not in CI. `UndoLedger` — undo runs undo only, commit runs commit only, each once, `CommitAll`, bound. Toast — eviction at the live cap commits, `UndoNewest` undoes the newest, `DismissAllLive` commits. `ShouldHideOnDragEnd`. Each test is mutation-checked (the guarded line broken on purpose, the test seen failing).
- **Bucket E (ImGui Test Engine)**: comments draft comes back after a replace and on reopen; long-text draft comes back on reopen after the editor is closed by a replace, and Discard leaves nothing to restore; chat clear shows an Undo toast, Undo restores, Ctrl+Z restores; Log work: Esc Esc with typed text closes, reopening restores the text.
- **Bash-driver scenario / screenshot / sanitizer**: `scripts/dev/test-ui-offline-first.sh` (CI) runs the OfflineFirst group; the AiChat and Duration cases run in CI's `ui_test.run --all`.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) runs in CI; this container compiles `Source/Core` with `posix-core-check` and runs the Linux subset (`ninja-test-linux`).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome.
- **Manual residue**: none planned.

## Open questions

None — the lock-driven scope cuts are recorded above and listed in the PR.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Grid cell edit undo on the save toast (#28 single-cell, #33) and keeping a rejected value (#31): need `SmatchetGridFieldEditPipeline.cpp` / `TicketFieldEditor.cpp`, held by `bulk-edit-rows` and `shared-dropdown-recents`. Follow-up once those PRs merge; the toast API here is what it plugs into.
- Quick Create and new-issue row drafts (#51's other half): `SmatchetQuickCreateIssueUi.cpp`, `SmatchetNewIssueDraftUi.cpp` held by `shared-dropdown-recents`.
- Undo for discarding queued offline rows and for the conflict resolver's "Discard my edit" (#116): a row held for undo could replay during the window, so a safe undo needs a queue-service entry point (archive to dead letters, restore by original id) in `Sync/`. Follow-up.
- Reset layout keeps its confirm: the ImGui layout cannot be put back cheaply.
- fr-FR rows for the new strings: once `SmatchetLocalization.cpp` is free.

## Implementation log
*(commit shas are added once the branch is pushed; the single implementation commit carries the whole slice)*

- `141c34f` · `wip(plan)`: this plan.
- implementation commit · `DraftStorePure.h` + `DraftStore.{h,cpp}` (bounded map, JSON file, worker); `UndoLedgerPure.h` + `SmatchetToast` (`PushUndoable`, Undo button, Ctrl+Z, `CommitUndo`, `CommitPendingUndos`); kept drafts for Comments (#56), the long-text editor (#39), Log work (#51), the assistant message box (#103) and the conflict resolver (#110, now across restarts); chat clear with Undo after its confirm (#90; view delete was dropped, see Deviations); Esc during an armed hide-drag cancels the hide (#141); `~AppController` commits pending undos and stops the store before the chat-persist worker.

## Deviations from plan

- **Plan-lock not claimed.** `lock-claim.sh` could not push `refs/locks/input-ux-w2-drafts-undo`: the first attempt was refused by the pre-push hook (the branch's diff against `origin/develop` includes #2433's files, which overlap `input-ux-wave1-palette`), and a retry from a clean worktree on `origin/develop` got HTTP 403 from the session's git proxy, which only lets this session push its own branch. No override was used. The write set was kept off every sibling claim instead (see § Risks); the plan-lock gate on the PR is the check, and the claim can be filed from a normal checkout.
- **Base branch.** #2433 was not on `develop` when this started, so the branch stays stacked on `claude/frictionless-user-input-ux-ea174b` and the PR targets it; retarget to `develop` once #2433 merges.
- **Write set grew by four files**: `SmatchetCommentsModalUi.h` (test snapshot field), `TicketFieldEditor_detail.h` (test snapshot field), `AppController.cpp` (shutdown ordering: the undo commit and `drafts::Stop()` must run before `chat_persist::Stop()`, and both targets reach it through `~AppController`, so it replaced the two planned host edits in `StandaloneAppBootstrap.cpp` and `SmatchetImGuiHost.cpp`) and `SmatchetBottomPanelDragPure.h`. None is in a sibling lock.
- **Localization.** No French rows were added (`SmatchetLocalization.cpp` is held by three siblings). New strings fall back to English. One string changed meaning: the conflict resolver's "kept until Smatchet closes" note is now `offline.conflict.draft_saved`; the old key's row stays in the table unused until the lock lifts.
- **Chat clear keeps its confirm.** Planned as an immediate action with Undo; #2437 (piece 4.1, merged while this was in review) put the confirm on the dialog contract and added tests that assert it, so this piece adds Undo after the confirm instead of removing the question. The merge into `develop` conflicted in `SmatchetAiAssistantUi.cpp` and was resolved that way.
- **View delete Undo dropped (file lock).** The branch first carried a view-delete Undo in `SmatchetViewsDashboardUi.cpp` (re-creating the captured definition through the create latch; refused if the tracker changed; it came back last and active). On 2026-11-19 plan-lock `pane-picker-jira-sites` (branch `claude/plus-dropdown-two-jira-261a42`) added that file to its write set and the pre-push gate refused the overlap. No override was used: the file's change was reverted to `develop`'s, so the view delete keeps its confirm and its plain toast. The toast API is unchanged; re-adding it is one function plus a call once the lock lifts (`PushViewDeletedUndoToast` in commit history before this revert).
- **`DraftStore` worker tests moved to bucket E.** CI's first run failed to link `SmatchetTests`: it compiles an explicit list of Core sources, and `DraftStore.cpp` is not in it, so the worker cases in `IssueDraft.test.cpp` had unresolved externals. Adding the source means editing `tests/CMakeLists.txt`, which `bulk-edit-rows` and `shared-dropdown-recents` hold, so no edit was made. The cases left that file (restored to `develop`'s version); one bucket-E case in `field_viewer_modal.test.cpp` (`DraftStore_PersistsWithoutStopAndReloads`) covers write-without-Stop and the restart. The pure `DraftMap` cases (save, restore, isolation, bounds, eviction, file format, debounce) stay in `OfflineFirstPure.test.cpp`, which `SmatchetTests` does build. When the locks lift, adding `DraftStore.cpp` to the unit target and moving the worker cases back is a small follow-up.
- **Review fixes (round 1).** The code-review gate found: the delayed chat-delete could take a reply that landed inside the undo window (fixed: the Clear button is disabled while a reply is coming in, the empty-history clear is immediate as before, and a refused Undo says so); Ctrl+Z reached an undo toast while a non-modal `TextEditor` had the keyboard (fixed: the key is noted in `Render` and decided in an ImGui `RenderPre` hook, once every window has set `io.WantTextInput`); `DraftStore` left itself looking alive after its worker died and let a `Start` racing a `Stop` reset `stopRequested` (fixed: the worker clears `running` as its last act, `Start` reaps a dead worker, `Stop` keeps `running` until the join returns); the plan promised a bounded `Stop` (reworded: shutdown is the one place the store waits).
- **Dictation into the long-text editor** changes the buffer without a text-changed event, so only the next typed edit (or none) stores it. Not covered; follow-up if it shows.
- **Long-text save that the tracker rejects** erases the draft at Save, as the edit pipeline owns the failure (#31 territory, locked files).
- **Worklog Esc.** Cancel and the second Esc close and keep the entry instead of asking, as planned (a second confirm is piece 4.1's).

Findings left open (reason): **#28, #31, #33** (grid edit undo, rejected value kept, blur-commit: `SmatchetGridFieldEditPipeline.cpp`, `TicketFieldEditor.cpp` and the grid cell TUs are held by `bulk-edit-rows` / `shared-dropdown-recents`; § 5.3 Wave 3 for paste and the rest); **#116** (conflict "Discard my edit" undo: needs an archive/restore entry point in `Sync/`, since a held row could replay during the window); **#51** new-issue half and **#7 / #50** are in #2435 / the locked draft-row files; **#90** view delete (file held by `pane-picker-jira-sites`), offline-row discard and reset-layout keep their confirms. Closed earlier in Wave 1 (#2433): #14, #74, #87, and the confirm half of #39 and #110.

## Verification (actual)

*(filled below once the full cycle has run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.**
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md`.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
