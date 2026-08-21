# Plan — Open any .md file in the Plan Docs viewer (drag-and-drop + Open… dialog)
<!-- plan-date: 2026-08-21 -->

> **Slug**: `drag-drop-md-viewer`.
>
> **Status**: `shipped`

## Context

User request: dragging a markdown file onto the Smatchet window should open it in the Plan Docs viewer, rendered like the plan docs; plus the ability to open any `.md` on the PC. Before this change no `glfwSetDropCallback` existed anywhere, and the viewer could only display files from its scanned index (`docs/design` + `docs/adr`) — selection was an index into that list, with no "open this arbitrary path" entry point. After this lands, dropping a `.md`/`.markdown` file on the app (or picking one via the viewer's "Open..." button) opens it in the Plan Docs viewer, focused and rendered through `MarkdownPreviewRender`.

## Approach

Add one Core entry point, `smatchet::PlanDocViewerOpenExternalFile(UiDrawSession&, const std::string&)`, that appends the path to a new session-lifetime `droppedFiles` group in the viewer's state, selects it in a combined picker (scanned files first, then dropped), and raises the existing `showPlanDocViewer` + `requestPlanDocViewerFocus` flags. The function does no file I/O (Pillar 2) — the viewer's existing per-frame async load (`StartLoadSelected` → `ReadCapped` on the `AsyncLoadGatePure` slot) reads the file and its existing error/oversize bodies cover unreadable paths.

Two producers feed it: (1) a GLFW file-drop callback in `StandaloneBoot_detail` (shared by both boot paths; fires on the main thread inside `glfwPollEvents`, so writing `g_ui` directly is safe; ImGui's GLFW backend installs no drop callback, so nothing is shadowed), filtering to `.md`/`.markdown`; and (2) an "Open..." button in the viewer that sets a one-frame `requestPlanDocOpenFileDialog` flag consumed at the `DrawPlanDocViewer` call site in `SmatchetUI.cpp` via `AppController::RequestOpenFilePaths` — the request-flag indirection keeps the viewer TU off the AppController fan-in list (`app-controller-fan-in` gate).

Selection is preserved by path across index rescans (`pendingReselectPath`), because the scanned-block size shifting would otherwise re-point a combined index at a different document — worst for dropped files, which sit after the scanned block.

## Files to modify

1. `Source/Core/include/Ui/SmatchetPlanDocViewerUi.h` — declare `PlanDocViewerOpenExternalFile`; refresh the stale (TextEditor-era) surface-contract comment.
2. `Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp` — `droppedFiles` + `pendingReselectPath` state, combined-index helpers (`CombinedCount`/`PathAtIndex`/`FindCombinedIndex`/`EntryLabel`), path-preserving reselect in `PollIndexResult`, combo over the combined list, "Open..." button, the new entry point.
3. `Source/Core/include/Ui/SmatchetUiSession.h` — `requestPlanDocOpenFileDialog` one-frame flag.
4. `Source/Core/src/Ui/SmatchetUI.cpp` — consume the flag at the `DrawPlanDocViewer` call site via `app.RequestOpenFilePaths`.
5. `Source/Standalone/StandaloneBoot_detail.h` / `.cpp` — `MarkdownFileDropCallback` (+ extension filter helper).
6. `Source/Standalone/main.cpp`, `Source/Standalone/StandaloneAppBootstrap.cpp` — `glfwSetDropCallback` registration after window creation.
7. `tests/ui/plan_doc_viewer_external_open.test.cpp` (new) + `tests/ui/CMakeLists.txt` + `tests/ui/ui_tests_registry.cpp` — bucket-E coverage.

## Existing utilities reused

- `async_load::LaunchIntoSlot/TakeFromSlot/ShouldKickLoad/ResultIsCurrent` — `Source/Core/include/Ui/AsyncLoadGatePure.h` — the untouched off-thread read path does all file I/O.
- `MarkdownPreviewRender::Render` — `Source/Core/src/Ui/MarkdownPreviewRender.cpp` — untouched rendering path.
- `AppController::RequestOpenFilePaths` — `Source/Core/src/AppController_HostIntegration.cpp:231` — native picker seam, host handler already installed in `SmatchetUI.cpp` (same recipe as the attachment picker in `SmatchetNewIssueDraftUi.cpp:325`).
- `boot_detail::KeypadEnterBridgeCallback` precedent — `Source/Standalone/StandaloneBoot_detail.cpp` — the shared-callback home the drop callback joins.

## Extraction sizing

N/A — nothing extracted or split.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — no new per-frame work while idle; the combined-list helpers run only inside the viewer window's draw.
- **Pillar 2 (UI never freezes)**: no new sync I/O — the entry point and drop callback do zero file I/O; reads stay on the existing `std::async` worker with the existing "Loading..." cue. The "Open..." dialog is the same system-modal stall the attachment picker already accepts.
- **Pillar 3 (never crash)**: combined-index accessors bounds-check and return empty on miss; null/empty drop paths are skipped; dedup prevents unbounded growth on repeat drops of the same file.
- **Pillar 4 (accessibility)**: no impact — reuses existing combo/button widgets (keyboard-navigable like the rest of the window).

## Perf-review-system gates

1. **PR-fast CI** — N/A: no steady-state hot path touched; the viewer draws only when open, and the new code runs on user gestures (drop/click).
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`; the existing `/* PILLAR2_WORKER_ONLY */` read path is unchanged.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new >100 ms sync-stall path (the file dialog is the pre-existing accepted modal).
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- **Risk**: dropped-file selection lost across rescans → mitigated by `pendingReselectPath` path-preserving reselect.
- **Risk**: two dropped files sharing a filename produce identical combo labels → mitigated for ImGui id purposes with `PushID(index)`; visual ambiguity accepted (rare; full path visible nowhere else in the picker today either).
- **Non-goal**: persisting dropped files across sessions (session-only by design).
- **Non-goal**: markdown-type filter in the native dialog (`Win32PickFiles` has no filter param; an explicit pick of a non-.md text file renders harmlessly as text).
- **Non-goal**: drop-to-open on the Mobile/Unreal shells — GLFW drop is standalone-only; the Core entry point is shell-agnostic for future reuse.

## Verification

- **Bucket A**: N/A — no pure-logic seam added (state lives in the viewer TU's file-static singleton).
- **Bucket E**: `tests/ui/plan_doc_viewer_external_open.test.cpp` — `PlanDocViewerOpenExternalFile` on a temp `.md` must open + focus the window and render `##plan_doc_picker` despite an empty scan (pre-feature this state showed only the empty-state text). Existing `PlanDocViewer_RendersEmptyStateAndRescanButton` guards the both-empty branch.
- **Bash-driver scenario / screenshot / sanitizer**: none added.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — grill-with-docs**: sharpened during design review — the AppController fan-in gate forced the request-flag indirection, and the first-scan selection shift (drop before the initial index lands) forced path-based reselect in `PollIndexResult`, not just the rescan latch.
- **Manual residue**: OS drag-drop gesture itself is not synthesizable by the test engine; covered by the thin-callback design (filter + delegate) and a manual drag check on a desktop build.

## Out of scope (flagged, not designed)

- Recent-files list / favourites for dropped docs — viewer stays list-of-now per its R6 scope guard.
- A command-palette "open markdown file" command — the viewer button + drop cover the request; add on demand.

## Implementation log

- `856294ab` · feat: external-open entry point, combined picker, drop callback in both boot paths, "Open..." dialog request flag, bucket-E test, this plan doc.
- `f57780ca` · fix: code-review findings — batch `PlanDocViewerOpenExternalFiles` (de-dups the reopen-first trick in both producers), droppedFiles purge on scan landing, picker rendered under indexError when docs exist, `openFileDialogAvailable` gate hides "Open..." where no dialog handler exists, test temp-file uniqueness + no-delete, dead `<cctype>` include dropped.

## Deviations from plan

- "Open..." button is hidden (not shown-but-dead) on platforms without a `RequestOpenFilePaths` host handler, via a new `UiDrawSession::openFileDialogAvailable` flag — the plan had accepted a silent no-op; review flagged it as a broken-looking button.
- The single-path `PlanDocViewerOpenExternalFile` remains (test + convenience) but delegates to the batch form; producers call the batch form directly — the plan's "re-open the first pick" pattern was dropped as duplicated logic.
- Picker visibility decoupled from the index-error branch (plan implied the pre-existing branch structure): dropped docs stay reachable after a failed scan.

## Verification (actual)

- `agents/scripts/project/test-lint-rules.sh --diff origin/develop` — all gates green (initial run's `function-too-long` on `BootInitGlfwAndWindow` and a `duplication` clone vs `WhisperPlugin.cpp` were fixed, not exempted). Residual WARNs are pre-existing (tu-line-ceiling on `SmatchetUI.cpp`, comment-ratio on two heavily documented headers).
- `scripts/dev/test-docs.sh` — 19/19 green (includes plan-index + ref-integrity for this doc).
- Per-TU C++14 `clang++ -fsyntax-only` of every modified/new TU against the pinned third-party headers (imgui docking @329c5a6b, ghc 1.5.14, SQLiteCpp, nlohmann, GLFW 3.3.8, imgui_test_engine @8568767a) — clean; the drop callback's signature type-checked against `GLFWdropfun`.
- MSVC dual-target build + ctest + bucket-E run on PR #2152's CI (this container has no Windows toolchain).
- Manual residue: the OS drag gesture itself (not synthesizable by the test engine) — desktop drag check tracked on the PR's test plan.
