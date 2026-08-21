# Plan — stop the MCP plugin restarting on every keystroke in Preferences (#2110)
<!-- plan-date: 2026-08-18 -->

> **Slug**: `fix-2110-mcp-keystroke-restart`
>
> **Status**: `shipped`

## Context

[Issue #2110](https://github.com/alexandrosk0/Smatchet/issues/2110) (`bug`, `P2`, `area:ui`): the
Integrations tab of Preferences tears down and re-binds the MCP HTTP listen socket **once per
character** typed into the "MCP auth token" field. `DrawMcpSectionBody`
(`Source/Core/src/Ui/SmatchetPreferencesUi.cpp`) runs a per-frame state diff of the five MCP
widget buffers against `d.cfg` and, when the diff trips, calls `ph->SyncMcpPluginWithConfig(app,
d.cfg)` **inline on that frame**. The disk write behind `MarkPrefsDirty` is debounced (~100 ms);
the plugin sync is not.

Reported harm: in-flight MCP requests killed mid-typing; a `TIME_WAIT` / port-steal race between
teardown and `bind_to_port` can leave no server listening at all; partial token prefixes reach the
config on disk; config visibly churns underneath concurrent MCP clients.

After this lands: typing a token or a port produces exactly **one** config commit and **one**
plugin sync, when the field stops being edited.

## Approach

Gate the commit on **"this field is not currently being edited"** — a *state* predicate composed
into the existing *state* diff, rather than converting the block to event-driven commits. A text
or numeric field that still holds keyboard focus has a **prefix** in its buffer; that prefix must
not reach `d.cfg`. Checkboxes are one event per change and stay ungated.

Both halves are gated per field: the dirty comparison **and** the assignment into `d.cfg`. Gating
only the comparison would still flush a half-typed token whenever some other field tripped the
diff.

`ImGui::IsItemActive()` reads the **last submitted item**, so it can only be sampled at a call
site whose widget is the last thing submitted. That holds for `ImGui::InputInt` — `EndGroup()`
copies a contained `ActiveId` into `LastItemData.ID` precisely so `IsItemActive()` works on a
whole group (`imgui.cpp`, "If the current ActiveId was declared within the boundary of our
group..."). It does **not** hold for `SmatchetSecretInputText`, which is compound and ends on a
`SmallButton` / `TextDisabled`; its editing state must be captured **inside** the helper,
immediately after the inner `ImGui::InputText`, and returned to the caller.

## Files to modify

| File | Change |
|---|---|
| `Source/Core/include/Ui/SmatchetSecretInput.h` | Add a defaulted `bool* outEditing = nullptr` out-param; set it from `ImGui::IsItemActive()` immediately after the inner `InputText`, before the `SameLine`/`SmallButton`/`TextDisabled` chain. Defaulted, so the other 8 call sites are untouched. |
| `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` | `DrawMcpSectionBody`: capture `tokenEditing` (via the new out-param) and `portEditing` (`IsItemActive()` after `InputInt`); gate the token + port terms of the dirty diff and their `d.cfg` assignments on `!editing`. |
| `tests/ui/mcp_prefs_commit_gate.test.cpp` | **New.** Bucket-E coverage: variant 1 drives the real `SmatchetSecretInputText` and asserts the out-param reports focus while a caller-side `IsItemActive()` does not; variant 2 locks the gate composition (both halves gated) against a replica of the commit block. |
| `tests/ui/CMakeLists.txt` | Enrol the new TU in `_SMATCHET_UI_TEST_SOURCES`. |
| `tests/ui/ui_tests_registry.cpp` | Declare + call `SmatchetRegisterMcpPrefsCommitGateTests` inside the existing `#if defined(SMATCHET_WITH_MCP)` block. |
| `docs/plans/fix-2110-mcp-keystroke-restart.md` | This plan. |

## Existing utilities reused

- `SmatchetSecretInputText` (`Source/Core/include/Ui/SmatchetSecretInput.h`) — extended, not forked.
- `ImGui::IsItemActive()` — the established repo commit-gate family; `IsItemDeactivatedAfterEdit()`
  is already used in `AnnotateAnalysisUi_Preferences.cpp` and `SmatchetViewsDashboardUi_widgets.cpp`.
- `MarkPrefsDirty` (`Source/Core/include/Ui/SmatchetUiSession.h`) — unchanged debounced disk write.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — a defaulted parameter plus five gated expressions; extracts and splits nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: strictly positive. Removes an unbounded-by-frame socket teardown + rebind
  from the UI draw path — one per keystroke today, at most one per edit-completion after.
- **Pillar 2 (UI never freezes)**: strictly positive, and the real prize. `SyncMcpPluginWithConfig`
  stops a plugin, closes a listen socket and re-binds it **synchronously on the UI thread**; today
  that runs on the same frame as a keypress. This does not make the call async — it stops it firing
  per character.
- **Pillar 3 (never crash)**: positive. Closes the "half-typed token ends with no MCP server
  listening" window (a bind race per rebind) and stops token prefixes reaching the sealed config.
- **Pillar 4 (accessibility)**: neutral — no layout, no focus order, no contrast change. The
  out-param carries no visual effect; the widget draws exactly as before.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

Diff touches `Source/Core/` (one header, one UI TU), so the gates are declared:

- **Scenario coverage**: no perf scenario drives the Preferences → Integrations tab, so the curated
  diff→scenario map yields no affected subset; a `perf-run.sh` delta would measure noise.
- **Budget**: the change is net-negative work per frame (one `IsItemActive()` bool read added; a
  socket teardown+rebind removed from the typing path). No new allocation, no new I/O, no new
  per-frame string work — `tokenBufStr` is constructed exactly as before.
- **Verdict**: no measurable regression risk against the 6.94 ms steady-state budget; a scenario
  run is not warranted. Re-declare in review if a reviewer disagrees.

## Risks / non-goals

- **Risk: an uncommitted value is lost if the section body stops drawing before the commit
  frame.** Today the buffer commits per keystroke, so a hard quit mid-typing keeps the prefix;
  after this change the value commits on the first frame the field is no longer being edited.
  Traced against the real exit paths rather than assumed — two windows remain, both narrow:
  - The app exits (or dies) while the field still holds focus. Inherent to every
    commit-on-edit-complete gate, and the alternative is the per-keystroke prefix write the Issue
    calls a bug.
  - The user collapses the MCP `CollapsingHeader` (or filters the section out) with an
    uncommitted edit and then closes Preferences. `PrefsSectionBegin` returns false for a
    collapsed / filtered section, so the body — and with it the commit — never runs that frame;
    the close gate in `drawPreferencesWindow` guards only **Tracker** fields, so
    `resetPreferencesWindowState` clears `preferencesBuffersLoaded` and the next open re-hydrates
    the buffers from `d.cfg`. See § Out of scope — closing this means teaching the close gate
    about MCP dirtiness, i.e. the P2-H3 guard-modal state machine, which is not this Issue.
- **Every other exit commits normally** (verified, not assumed). The window's **X** takes
  `ActiveId` on the mouse-down frame while `d.showPreferences` is still true, so the body draws
  that same frame with the field already inactive and the commit fires; `showPreferences` is set
  false only by that `p_open`, and every other writer in the tree only sets it *true*. Clicking a
  checkbox, the Show/Hide toggle, another section's header, or the settings search box likewise
  deactivates the field on a frame the body still draws. Escape is consumed by `InputText`
  (reverts + deactivates) before it reaches the window.
- **Non-goal: debouncing `SyncMcpPluginWithConfig` itself** (the Issue's second suggested-fix
  bullet). See § Deviations.
- **Non-goal: the other 8 `SmatchetSecretInputText` call sites.** Tracker / Assistant / Whisper
  credential fields are Save-button gated and drive no per-frame sync; they keep the default
  `nullptr` and behave identically.
- **Non-goal: the `InputInt` clamp-to-[1,65535] UX** (clearing the port field snaps it to `1`).
  Pre-existing, unrelated to the restart storm, and out of scope.

## Verification

1. `cmake --build` the standalone target — no new warnings (warnings-as-errors).
2. `bash scripts/dev/test-all.sh` — no regression.
3. `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` — all delta gates green.
4. Manual (repro from the Issue): launch with MCP enabled, Preferences → Integrations, type a
   multi-character token — `debug.mcp_status` shows **zero** stop/rebind/start cycles while typing,
   and **exactly one** after focus leaves the field.
5. Manual: same for "MCP Port" — per-digit restarts gone.
6. Manual: toggling each of the three checkboxes still syncs immediately (one cycle per toggle).

## Out of scope (flagged, not designed)

- Making `SyncMcpPluginWithConfig` non-blocking on the UI thread (it stops a plugin and re-binds a
  socket synchronously). Worth a Pillar-2 look independently of the keystroke storm.
- Teaching the Preferences close gate about MCP dirtiness, so collapsing the section (or filtering
  it out) with an uncommitted token/port edit and then closing the window cannot drop it. Today
  that gate keys on `TrackerPrefsFieldsDiffer` alone and routes through the P2-H3 guard modal;
  widening it is a state-machine change with its own test surface. Filed as a follow-up Issue.

## Implementation log

1. `Source/Core/include/Ui/SmatchetSecretInput.h` — added `bool* outEditing = nullptr` and set it
   from `ImGui::IsItemActive()` on the line after the inner `ImGui::InputText`, before the
   `SameLine` / `SmallButton` / `TextDisabled` chain. The doc comment states *why* the sample has
   to live in the helper (the widget is compound, so a caller's own `IsItemActive()` reads the
   Show/Hide button or the char-count label). Defaulted, so the other 9 call sites
   (`SmatchetPreferencesUi.cpp:365,398,422,471`, `SmatchetPreferencesUi_Assistant.cpp:511,538,570`,
   `SmatchetPreferencesUi_Whisper.cpp:358`) compile and behave unchanged.
2. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp`, `DrawMcpSectionBody` — declared `portEditing` /
   `tokenEditing` (both `false`, so a field the settings filter hid — never submitted, no item
   state — stays committable); captured `portEditing` from `IsItemActive()` after `InputInt` and
   `tokenEditing` through the new out-param; gated the port and token terms of the dirty diff
   **and** their `d.cfg` assignments on `!editing`. Checkboxes stay ungated.
3. Corrected the banner above `DrawMcpSectionBody`, which still claimed the block commits "as soon
   as a widget changes" — now distinguishes the checkboxes from the two typed fields.
4. Verified the second commit path, `SmatchetUI::onPreferencesSaveAndSync`
   (`SmatchetPreferencesUi.cpp:1002`), needs no change: an explicit Save button must commit
   whatever is in the buffer, and the click itself moves `ActiveId` off the input. It is the
   safety net for the deferred-commit windows in § Risks, not a hole in the gate.

## Deviations from plan

- **The Issue's first suggested fix was narrowed, not taken verbatim.** It proposes
  `IsItemDeactivatedAfterEdit()` or `ImGuiInputTextFlags_EnterReturnsTrue`. Both are *event*
  predicates and the surrounding block is a *state* diff, so adopting them would have meant
  rewriting the whole MCP commit path to event-driven — a much larger diff for the same outcome.
  `!IsItemActive()` is a state predicate that composes into the existing diff, which is why the
  final shape gates the diff terms instead of replacing them.
- **The Issue's second suggested fix — debouncing `SyncMcpPluginWithConfig` — was rejected.**
  Deferring the sync inside a draw path that can stop being called (collapsed section, filtered
  section, closed window) would leave the config written to disk while the plugin ran unsynced
  until the next app start: a worse failure than the one being fixed, and silent. Making that call
  non-blocking is a real Pillar-2 item, kept in § Out of scope on its own merits.
- **The § Risks claim about safe exits was rewritten after tracing the code**, not left as
  drafted. The original wording asserted every ordinary exit commits; the collapsed/filtered
  section plus window-close combination does not, and now says so, with the close-gate widening
  moved to § Out of scope and filed as a follow-up Issue.

## Verification (actual)

1. **Build — PASS.** `bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc
   --target SmatchetStandalone SmatchetCore_DX12` — 1023 steps, both targets linked
   (`SmatchetCore_DX12.lib` + `Smatchet.exe`), warnings-as-errors clean. Dual-target, so the new
   defaulted parameter compiles in the Unreal/DX12 world too. Re-run clean after rebasing onto
   develop, which had moved four commits (including a `ScopedFileLock` → `FileIo` extraction in
   Core, #2122).
2. **`scripts/dev/test-all.sh` — no regression.** `AGGREGATE Passed: 3253 Failed: 36 Scripts: 200`,
   exit **2** — the suite's *missing binary / build* code, not the assertion-failure code (1). All
   36 are environmental and pre-existing, attributed two independent ways rather than assumed:
   - **Unbuilt bucket-E presets.** Every `scripts/dev/test-ui-*` / screenshot-diff failure is
     `build/ninja-ui-test-msvc/Smatchet.exe not found` (also `-asan-msvc`, `-asan-clang`).
     `build/ninja-ui-test-msvc/` does not exist in this worktree — the preset was never configured
     here. Those scripts reported `Passed: 0`, i.e. they never executed a test.
   - **Toolchain gaps.** `test-verifier-preship-wiring-bats` (5) fails on `Python was not found; run
     without arguments to install from the Microsoft Store` (the Windows App-Execution-Alias stub);
     `test-mutation-smoke` (1) on the same stub; `test-p4-mirror-bootstrap-bats` (2) on `p4` absent
     from PATH with no `apt-get`; `test-docs` on a `.claude/hooks/agent-token-log.py` harness-shim
     drift.
   - **Cross-worktree control.** The sibling worktree `optimistic-carson-c7a325` — different branch,
     different exe, separate session — reproduces the same 10 agent-infra bats suites with identical
     Passed/Failed counts and the same missing-preset + Python-stub classes.
   - **Direct attribution.** No failing script references `SmatchetPreferencesUi.cpp`,
     `SmatchetSecretInput.h`, or any MCP-preferences symbol. The preset this change *was* built and
     linted against (`build/ninja-iter-msvc/Smatchet.exe`) exists and is current.
3. **`bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` — exit 0.** Two
   advisory WARNs, both pre-existing properties of the touched TU rather than new violations:
   - `[tu-line-ceiling] Source/Core/src/Ui/SmatchetPreferencesUi.cpp:1332` — the TU was already over
     the 1,200-line advisory ceiling; this change adds 23 lines to it and splits nothing.
   - `[func-size] SmatchetPreferencesUi.cpp:868 DrawMcpSectionBody/2 — 104 lines > 100 (soft tier)`
     — was 81 lines, now 104. Soft tier, not blocking. Not split: the two gate booleans have to be
     declared in the same scope as both the widget calls and the commit block, so a split would
     have to thread them through a struct for no readability gain.
4.–6. **Manual (repro from the Issue) — user-verified.** No bucket-C screenshot golden or bucket-E
   ImGui-Test-Engine scenario covers this widget (`tests/ui/mcp_live_http_auth.test.cpp` and
   `tests/ui/mcp_resources.test.cpp` set `cfg.McpAuthToken` directly and never drive Preferences),
   so the ship-loop's visual-validation exception fired and the built exe was handed to the user.
   Verdict returned clean on all three steps: zero stop/rebind/start cycles while typing a token
   and exactly one on focus loss; the same for "MCP Port"; and one cycle per checkbox toggle,
   unchanged. Steps 4-5 are now regression-gated by the bucket-E coverage in step 7; step 6 (the
   checkboxes) is covered by variant 2's ungated-checkbox assertions.
7. **Bucket-E — PASS, both variants.** `tests/ui/mcp_prefs_commit_gate.test.cpp`, run against a
   freshly-built `ninja-ui-test-msvc` as
   `Smatchet.exe cmd ui_test.run --name=<variant> --spawn --yes` → `{"passed":1,"failed":0,
   "tested":1}` for each of `McpPrefs / SecretInput_ReportsEditingState` and
   `McpPrefs / CommitGate_OneSyncPerBurst`.
   - Variant 1 binds the **real** production header: it asserts `outEditing` reports keyboard focus
     *and* that a caller-side `ImGui::IsItemActive()` sampled after the compound widget returns is
     **false** mid-typing (it reads the trailing `SmallButton` / `TextDisabled`). Deleting the
     out-param and "simplifying" to a call-site sample reintroduces #2110 and fails here first.
   - Variant 2 is a **replica** of the commit block (`SmatchetPreferencesUi.cpp:920-951` —
     `DrawMcpSectionBody` is anonymous-namespace and needs a live `AppController` + `PluginHost`,
     so it is not callable from a test TU). It carries a drift warning naming the exact production
     lines that invalidate it. It asserts a typing burst syncs zero times and writes no prefix into
     cfg, focus loss commits exactly once, idle frames do not re-sync, and — moving the checkbox
     state *without* stealing focus — that gating only the comparison (the tempting simplification)
     would flush the half-typed token, while the gated assignment does not.
   - Residual: the "MCP Port" `InputInt` is not driven. Its gate is the same composition, but the
     test engine cannot address the inner input of an `InputInt`-with-step-buttons by a stable item
     path, so driving it would be a flaky assert rather than coverage. It stays on step 5's manual
     verification.
   - Two environment notes for whoever re-runs this: the `--spawn` child adopts the parent's
     configured MCP token, so a profile holding one returns HTTP 401 — point `SMATCHET_USER_DATA`
     at a scratch dir for a clean profile — and `SmatchetCore_DX12` currently fails to build with
     `SMATCHET_BUILD_UI_TESTS=ON` for a **pre-existing** reason unrelated to this change
     (`tests/ui/ai_chat_panel.test.cpp` guards only on `SMATCHET_BUILD_UI_TESTS` while the
     `UiDrawSession` members it uses are under `SMATCHET_WITH_AI`, which that target omits by
     design). This TU's own DX12 object compiles clean; the gap is filed separately.

## Archive (post-ship — DO IN THIS PR, never a follow-up)

Flip § Status to `shipped` and `git mv` this file to `docs/plans/shipped/` in the same PR that
fills the post-ship sections.
