# Plan — input UX Wave 2 piece 4.2: a shared text field

> **Slug**: `input-ux-w2-text-field` (matches this file's basename without `.md`).
>
> **Status**: `active` — driving the Wave 2 piece 4.2 PR (stacked on PR #2433 until it merges, then retargeted to `develop`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 4.2 found that every text field in Smatchet is a raw `InputText` over a fixed `char` buffer. The localization wrapper (`SmatchetLocalizedImGui.h`) adds the label translation and the dictation hook, nothing else. So focus on open, select-all, trimming and length limits differ per call site, and several buffers clip pasted text with no cue. Wave 1 (PR #2433) fixed the worst single-file cases and added `Ui/DialogKeysPure.h`. Wave 2 builds the seven shared pieces in parallel, one PR each. This plan is piece 4.2: a `SmatchetInput` family backed by `std::string` with a resize callback, carrying the § 3 keyboard-contract rules for single-line fields and multiline composers.

After this lands: one helper owns text-field behaviour. A migrated field selects its text on click when it holds a replaceable value, trims on commit when asked, shows its example as a placeholder, never clips (std::string storage), and follows the § 3 Enter / Esc / Ctrl+Enter rules.

Targets the audit's 4.2 findings: #1, #2, #4, #5, #9, #10, #13, #36, #102, #112, #125, #132 (§ 7). What ships here and what is deferred is listed under § Files to modify and § Out of scope; the per-finding verdict goes in § Deviations after ship.

## Approach

One header, `Source/Core/include/Ui/SmatchetInput.h`, with a matching TU `Source/Core/src/Ui/SmatchetInput.cpp`, exposes:

- **Storage primitives** hoisted from the toolbar editor (`SmatchetToolbarUi.cpp` `InputTextStdStringResize` / `InputTextStdString` / `InputTextMultilineStdString`): a `std::string`-backed `InputText` / `InputTextMultiline` with `ImGuiInputTextFlags_CallbackResize`. The toolbar editor calls the hoisted copies, so there is one resize callback in the tree.
- **`SmatchetInput::Text(label, std::string&, const TextOptions&) -> TextResult`**, the single-line field. Options: `hint` (placeholder), `focusOnAppear`, `selectAllOnFocus`, `trimOnCommit`, `refocusAfterEnter`, `flags`. The result reports `edited`, `active`, `committed` (left the field any way but Esc), `enterCommitted` (a bare Enter, the existing `SmatchetTextInputCommittedWithEnter` rule), and `reverted` (Esc; ImGui restores the text the field had when it was activated).
- **`SmatchetInput::TextBuf(label, char*, size_t, const TextOptions&)`**, the same contract over a caller-owned fixed buffer. It exists only for fields whose storage sits in a header another branch has plan-locked (`SmatchetUiSession.h`). It keeps the dictation hook and adds a language-neutral `n / max` byte counter once the buffer is ≥ 90 % full, so a full buffer is no longer silent. Each such call site moves to `Text` when its storage can change.
- **`SmatchetInput::Multiline(label, std::string&, ImVec2, const MultilineOptions&) -> MultilineResult`**, the § 3 multiline-composer rules. Enter types a newline. Ctrl+Enter submits (`submitted`). The first Esc leaves the field and keeps the text (`leftField`): the widget releases the active id before ImGui's own Esc handler can revert the edit. The second Esc is the caller's (dialog-level, piece 4.1).
- **Pure decisions** in `Source/Core/include/Ui/SmatchetInputPure.h` (namespace `smatchet::textinput`, no ImGui): the resize growth step, the edge-whitespace trim (std::string and fixed buffer), the field-appeared-this-frame test behind `focusOnAppear`, the single-line end-of-edit classification, the multiline submit / leave-field classification and the near-capacity counter threshold. `focusOnAppear` takes or waits on the caret through the existing `smatchet::dialogkeys::ResolveCaretRequest` (Enter held, drag in progress, window focus), the same rule the conflict resolver uses.

Migration order: Preferences first, then pickers, then composers. It stops at the plan-lock boundary described in § Risks: every surface whose storage or file is held by another branch's lock is listed in § Out of scope and § Deviations, not crossed.

Trade-off: `std::string` fields do **not** register with the dictation router. `DictationInsertionRouter` splices into a registered `char*` up to a registered capacity and keeps a sticky shadow of that pointer. A `std::string`'s storage moves when it grows, so registering it would let the shadow splice into freed memory (`DictationInsertionRouter.h` § shadow, CPP_CODE_AUDIT #21). Dictation into `std::string` fields needs a string-target API on the router first. That is a follow-up (§ Out of scope), and it is why composers, where dictation matters, are not moved to `std::string` here.

## Interface contracts

New shared header under `Source/Core/include/Ui/`. This widens a shared UI contract (`Source/Core/src/Ui/AGENTS.md` § Before you edit). No tracker interface, command, Lua / MCP binding, config, cache or SQLite format changes.

```cpp
// Source/Core/include/Ui/SmatchetInput.h  (include after imgui.h; ::ImGui only)
namespace SmatchetInput {

/// std::string-backed InputText / InputTextMultiline (CallbackResize). Hoisted from the toolbar editor.
bool InputTextStdString(const char* label, std::string& str, ImGuiInputTextFlags flags = 0);
bool InputTextMultilineStdString(const char* label, std::string& str, const ImVec2& size,
                                 ImGuiInputTextFlags flags = 0);

struct TextOptions {
    const char* hint = nullptr;     // placeholder; source text, translated like InputTextWithHint's
    bool focusOnAppear = false;     // caret in the field the first frame it is drawn (not inside a clipper)
    bool selectAllOnFocus = false;  // ImGuiInputTextFlags_AutoSelectAll: a click selects the value
    bool trimOnCommit = false;      // strip edge whitespace when the edit ends, except on Esc
    bool refocusAfterEnter = false; // keep the caret after a bare-Enter commit (add-another fields)
    ImGuiInputTextFlags flags = 0;  // extra flags (CharsNoBlank, ...)
};

struct TextResult {
    bool edited = false;         // text changed this frame
    bool active = false;         // the field holds the keyboard after this frame's input
    bool committed = false;      // the edit ended this frame by anything but Esc (Enter, Tab, click away)
    bool enterCommitted = false; // ... and it ended on a bare Enter / keypad Enter
    bool reverted = false;       // the edit ended on Esc; the text is back to its value at activation
};

TextResult Text(const char* label, std::string& value, const TextOptions& opts = TextOptions());
TextResult TextBuf(const char* label, char* buf, std::size_t bufSize, const TextOptions& opts = TextOptions());

struct MultilineOptions {
    const char* hint = nullptr;  // drawn inside the empty field
    bool focusOnAppear = false;
    bool submitOnCtrlEnter = true;
    ImGuiInputTextFlags flags = 0;
};

struct MultilineResult {
    bool edited = false;
    bool active = false;
    bool submitted = false; // Ctrl+Enter (no other modifier) ended the edit
    bool leftField = false; // Esc released the field; the text is kept
};

MultilineResult Multiline(const char* label, std::string& value, const ImVec2& size,
                          const MultilineOptions& opts = MultilineOptions());

} // namespace SmatchetInput
```

`SmatchetInputPure.h` (namespace `smatchet::textinput`) is internal to the widget but header-visible for the doctest rig:

```cpp
char* GrowStringForEdit(std::string& s, int newLength);           // resize callback body; negative -> 0
bool TrimEdgeWhitespace(std::string& s);                          // true when it changed s
bool TrimEdgeWhitespace(char* buf, std::size_t bufSize);          // NUL-safe, bounded
bool FieldAppearedThisFrame(int lastDrawnFrame, int frame);       // not drawn on the previous frame
struct SingleLineEnd { bool committed; bool enterCommitted; bool reverted; };
SingleLineEnd ResolveSingleLineEnd(bool deactivated, bool escPressed, bool bareEnterPressed);
bool MultilineEscLeavesField(bool fieldActive, bool escPressed);
bool MultilineSubmitted(bool deactivated, bool enterPressed, bool ctrlHeld, bool shiftHeld, bool altHeld,
                        bool superHeld, bool submitEnabled);
bool ShowCapacityCounter(std::size_t used, std::size_t bufSize);  // >= 90 % of usable bytes
```

Plumbing: the widget reads keys from `ImGui::GetIO()` / `ImGui::IsKeyPressed` and item state from `ImGui::IsItemDeactivated` / `IsItemActive`, immediately after its own `InputText`. It keeps `focusOnAppear` bookkeeping in the current window's `ImGuiStorage`, keyed off the widget id (the `SmatchetSecretInputText` reveal-state pattern). No new global state.

## Files to modify

Write set (plan-lock claim). Every path below is outside every other branch's live lock as of 2026-11-16 (`locks-show.sh`: `shared-dropdown-recents`, `bulk-edit-rows`, `input-ux-wave1-palette`).

**New: the family**

1. `Source/Core/include/Ui/SmatchetInputPure.h`: pure decisions (above).
2. `Source/Core/include/Ui/SmatchetInput.h`: the widget API (above).
3. `Source/Core/src/Ui/SmatchetInput.cpp`: widget bodies; picked up by `file(GLOB_RECURSE CORE_SOURCES …)`, no list edit.

**Hoist**

4. `Source/Core/src/Ui/SmatchetToolbarUi.cpp`: drop the TU-local `InputTextStdStringResize` / `InputTextStdString` / `InputTextMultilineStdString`; call `SmatchetInput::InputTextStdString` / `InputTextMultilineStdString`.

**Preferences (slice 2)**

5. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp`: `CredentialInputText` routes through `SmatchetInput::TextBuf` with `selectAllOnFocus` + `trimOnCommit` (#9). Example values move from hover tooltips into placeholders (#13): Jira Domain `companyname.atlassian.net`, Email `name@company.com`, Plane URL `https://api.plane.so`, GitHub Base URL `https://api.github.com`, Linear Base URL `https://api.linear.app/graphql`. Example values are data, not prose: no new localization keys. The Jira add-domain row gets the same field.
6. `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp`: the "add suggestion" and "add template" fields become `std::string` + `SmatchetInput::Text` with `refocusAfterEnter` (#4's pattern, which the audit names for these fields) and `trimOnCommit`.
7. `Source/Core/src/Ui/AnnotateAnalysisUi_Internal.h`: `p4Exe` / `p4vcExe` / `timeTpl` / `changeTpl` / `aiUrl` and `RemapEditBuf::from` / `to` become `std::string` (#132).
8. `Source/Core/src/Ui/AnnotateAnalysisUi.cpp`: seed the strings from config (was `CopyToBuffer`).
9. `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp`: the Perforce / AI URL / remap rows draw `SmatchetInput::Text` (`selectAllOnFocus`, `trimOnCommit` on paths and URLs; templates keep spaces), commit on `committed`.
10. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp`: read `aiUrl` / `timeTpl` / `changeTpl` as strings.
11. `Source/Core/src/Ui/AnnotateAnalysisUi_Worker.cpp`: `RunAnnotateProcessFromBuffers` copies strings (UI thread, before the worker starts; unchanged thread ownership).

**Pickers (slice 3)**

12. `Source/Core/include/Ui/SmatchetIconPickerUi.h`: `search_` becomes `std::string`.
13. `Source/Core/src/Ui/SmatchetIconPickerUi.cpp`: the filter takes the caret when the picker opens (#3's icon-picker case), selects all, never clips. A bare Enter picks the first match and does nothing on an empty list. Esc in the filter closes the picker and changes nothing (§ 3 picker row).
14. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`: the view-title rename field takes the caret when Rename shows it, with the name selected (#5), via `TextBuf` + `focusOnAppear` (storage `d.viewNameBuf` is in the locked session header).

**Tests**

15. `tests/Core/SmatchetWorklogSubmitPure.test.cpp`: doctest cases for every `SmatchetInputPure.h` function. It is the listed doctest TU that already holds the `DialogKeysPure.h` cases. A new `tests/Core/*.test.cpp` would need `tests/CMakeLists.txt`, which three other branches have locked.
16. `tests/ui/tracker_first_run_setup.test.cpp`: bucket-E on the migrated Jira Domain field in the real Preferences window: caret on open, Esc reverts, trim on Enter-commit. Plus a self-contained probe window that draws `SmatchetInput::Text` with the production option set: mouse click selects all, a 600-byte paste is kept whole (no clip), the Enter / Esc / click-away classification, and refocus after Enter.
17. `tests/ui/toolbar_append_cache_cue.test.cpp`: bucket-E on the icon picker (a real `SmatchetIconPickerUi` drawn by the test): caret in the filter on open with no click, type-to-filter, Enter picks the first match, Enter on an empty list picks nothing, Esc closes without a pick. It is the registered toolbar-surface UI suite; a new `tests/ui/*.test.cpp` would need `tests/ui/CMakeLists.txt` + `ui_tests_registry.cpp`, both locked.

**Docs**

18. `Source/Core/src/Ui/AGENTS.md`: one Before-you-edit bullet: new text fields use `SmatchetInput`. It also carries the dictation caveat for `std::string` fields.
19. `docs/plans/active/input-ux-w2-text-field.md`: this plan.
20. `docs/self-improvement/categories/debt/2026-11-16-text-field-migration-blocked-surfaces.md`: the deferred surfaces and the test relocation owed once the locks release.
21. `docs/self-improvement/categories/debt/2026-11-16-dictation-router-string-target.md`: the router string-target follow-up.

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Ui/SmatchetToolbarUi.cpp` | `InputTextStdStringResize` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` | `CredentialInputText` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp` | `s_prefNewSuggestionBuf` | `CORE_SOURCES` |
| `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp` | `CommitTextField` | `CORE_SOURCES` |
| `Source/Core/src/Ui/AnnotateAnalysisUi_Worker.cpp` | `RunAnnotateProcessFromBuffers` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetIconPickerUi.cpp` | `SmatchetIconPickerUi::Draw` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` | `drawViewsEditorHeader` | `CORE_SOURCES` |

## Existing utilities reused

- `InputTextStdStringResize` / `InputTextStdString` / `InputTextMultilineStdString`, `Source/Core/src/Ui/SmatchetToolbarUi.cpp:41-63`: hoisted, not rewritten.
- `SmatchetTextInputCommittedWithEnter`, `Source/Core/include/Ui/SmatchetSecretInput.h:24`: the bare-Enter rule behind `enterCommitted`.
- `smatchet::dialogkeys::ResolveCaretRequest`, `Source/Core/include/Ui/DialogKeysPure.h:41`: when `focusOnAppear` takes, waits on or drops the caret.
- `TrimCopyAsciiWhitespace`, `Source/Core/include/StringUtil.h:9`: the whitespace set `TrimEdgeWhitespace` strips (space, tab, CR, LF).
- `SmatchetLocalization::LabelFromSource` / `TranslateSource`: label and hint translation, the same calls `SmatchetLocalizedImGui::InputTextWithHint` makes.
- `SmatchetLocalizedImGui::HookDictationOnLastItem`: kept for `TextBuf` (fixed buffers only).
- `SmatchetSecretInputText`, `Source/Core/include/Ui/SmatchetSecretInput.h:44`: the in-tree model (state storage keyed off the widget id, item state sampled before trailing items).

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — the plan hoists three short helpers (~25 lines) out of one TU. Nothing is split to clear a cap.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: per field per frame the widget adds one `GetID` hash, two `ImGuiStorage` lookups when `focusOnAppear` is set, and key reads. No allocation on the draw path; trim allocates only on the commit frame. The icon picker already lowercases its query each frame; that stays one copy.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no I/O, no locks, no waits. Annotate config saves stay on `ScheduleAnnotateConfigSaveDetached`.
- **Pillar 3 (never crash)**: the resize callback clamps a negative length. `TrimEdgeWhitespace(char*, size)` is bounded by the buffer size and NUL-safe. `std::string` fields are never handed to the dictation router (dangling-shadow hazard). `GetID` / storage use the current window only.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: the improvement this plan exists for: caret where the user will type, Esc / Enter that mean the same thing everywhere, visible placeholders instead of hover-only examples. No colour changes.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires on the fixed `perf-pr-fast-set.json` set. No scenario exercises Preferences / pickers; the map's `fallback` (`idle`) measures the always-drawn shell, which this diff touches only through the shared widget.
2. **Pillar 2 static scanner** — N/A: no sync I/O added to a render path.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` added.

**Pre-push local check**: not runnable in the linux-container tier (no Windows exe). The PR-fast lane runs on CI.

**Override**: none planned.

## Risks / non-goals

- **Plan-lock collision on the hot shared files.** `SmatchetLocalization.cpp`, `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`, `SmatchetUiSession.h`, `SpreadsheetState.h`, `TicketFieldEditor.cpp`, `TrackerLabelsEditor.cpp`, the project picker, the grid header, the new-issue draft, Quick Create and `SmatchetViewsDashboardUi_widgets.cpp` are held by other branches. Mitigation: no override. The write set avoids them, new tests go in already-registered suites, no new localization keys are added, and the blocked surfaces are deferred. Accepted cost: the tests live in neighbouring files until the locks release (debt entry, item 20).
- **Esc behaviour change in multiline fields.** ImGui reverts a multiline edit on Esc. `Multiline` keeps the text instead. Only fields migrated to `Multiline` change; none of this PR's call sites use it. It ships tested so composers can adopt it.
- **`selectAllOnFocus` on fields where a click should place the caret.** Applied only to replace-the-value fields (domains, URLs, paths, names, filters), never to composers or the grid cell editor (audit #9).
- **Trim on commit changes a stored value.** Only on fields whose value cannot legally carry edge whitespace (domain, URL, path, workspace, id). Command templates keep it.
- **Dictation regression on migrated `std::string` fields.** The Annotate path / URL rows, the Templates add fields and the icon filter no longer take dictated text. Accepted (dictating a path or URL is not a real use). Fixed-buffer `TextBuf` fields keep the hook. Composers are deferred until the router can target a string (item 21).
- **Dual-target compile.** `SmatchetCore_DX12` compiles `Source/Core/`: the new header includes only `imgui.h` + `<string>` / `<cstddef>`, with no GLFW / GL.
- Save formats, MCP wire format, tracker interfaces: unchanged.
- **Non-goals:** a dialog scope (piece 4.1 owns focus restore, Esc-to-cancel and global-shortcut muting); Ctrl+Enter wiring and key hints on dialog buttons (piece 4.4); drafts / undo (4.3); fuzzy matching (4.7); a suggestion popup generalisation (#10, effort L).

## Implementation handoff

Single owner subsystem: `Source/Core/src/Ui/` (leaf `AGENTS.md`). One branch, three slices, one PR.

- **Slice 1 — family.** Write set: items 1-4, 15, 18. Pre-resolved: `::ImGui` only inside the widget TU (no `#define ImGui` alias); labels go through `LabelFromSource` exactly once.
- **Slice 2 — Preferences.** Write set: items 5-11, 16. Pre-resolved: `CredentialInputText` keeps `FocusNextItemIfOpenTarget` ahead of the widget (Wave 1's open-focus latch), and its return stays the bare-Enter signal that runs Test connection. Annotate's `RunAnnotateProcessFromBuffers` stays on the UI thread.
- **Slice 3 — pickers.** Write set: items 12-14, 17. Pre-resolved: the icon picker's Esc only acts while the filter holds the keyboard. A modal-wide Esc is the dialog scope's (4.1).
- **Order:** slice 1 first (everything calls it). Composers are not in this PR (§ Out of scope).

## Verification

- **Bucket A (pure-logic doctest)**: `tests/Core/SmatchetWorklogSubmitPure.test.cpp` cases for every `SmatchetInputPure.h` function. They are compiled and run in the container ad hoc (clang++ + doctest single header) and on CI in `SmatchetTests`. Each one is mutation-checked: break the function, see the case fail.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: the `TrackerFirstRun` and toolbar suites above. They run on CI's `Bucket-E UI tests (Mesa headless GL)` lane (a `workflow_dispatch` run on this branch while the PR is stacked, because `build-and-test.yml` runs `pull_request` only against `develop`). Each test is mutation-checked on CI: one dispatch run with the targeted mutations applied together, read per test.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no scenario or pixels change (placeholders draw only in empty fields).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on CI's `Windows + MSVC` jobs. In the container: a clang syntax pass of every touched TU against the fetched ImGui.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps; defer to the script).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome.
- **Manual residue**: none planned. Any step that ends manual gets a deferred-automation note + a `docs/self-improvement/categories/` entry.

## Open questions

- When the sibling locks release, do the relocated tests move to their own suites in this PR (if it is still open) or in a follow-up? Answer: the user. The default is a follow-up, tracked in the debt entry.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Surfaces behind another branch's plan-lock** (follow-up once released; debt entry item 20): grid inline edit 512 B (#36, `SpreadsheetState.h` + `TicketFieldEditor.cpp`); labels add-and-keep-focus (#4, `TrackerLabelsEditor.cpp`); project-picker and labels filter focus (#3, project picker + labels editor); JQL-style suggestion popup for other fields (#10, `SmatchetViewsDashboardUi_widgets.cpp`); the Preferences search box and every `SmatchetUiSession.h`-backed buffer moving from `TextBuf` to `std::string` (#2's `prefsSearchBuf[64]`); conflict resolver growable buffer (#112, `conflictResolveBuf` in `SmatchetUiSession.h`).
- **Composers** (comment box, worklog description #2, AI composer #102, Quick Create, new-issue description, long-text editor): wait on the dictation string target (item 21). They also overlap pieces 4.3 (drafts) and 4.4 (Ctrl+Enter + key hints). `Multiline` ships ready for them.
- **Toolbar icon-name feedback (#125)**: needs a new user-facing string, so a `SmatchetLocalization.cpp` row, which is locked. The name field moves to `std::string` with the hoist; the "No such icon" message is deferred.

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
