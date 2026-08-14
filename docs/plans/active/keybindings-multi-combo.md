<!-- plan-date: 2026-08-14 -->
# Plan — Multi-combo keybindings (several alternative combos per action)

> **Slug**: `keybindings-multi-combo` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Every rebindable shortcut carries **exactly one** key combo today: `Keybinding` holds a single
`std::string Hotkey`, and the whole editor / display / mutator surface is built on that assumption.
[`docs/guides/keyboard-shortcuts.md`](../../guides/keyboard-shortcuts.md) states the policy outright —
*"One combo per command"* — a deliberate v1 scope call in [`docs/plans/keyboard-shortcuts-rebindable.md`](../keyboard-shortcuts-rebindable.md).

**User request (verbatim):** *"extend the keybindings to have multiple keys. I want among others to
bind the font size increase to ctrl = and to ctrl +"*

Neither half works today:

1. There is no way to attach a second combo to `ui.zoom.in`.
2. `ParseImGuiHotkey` **cannot parse `Ctrl++` at all** — `+` is the token separator and empty tokens
   are dropped, so `"Ctrl++"` tokenizes to `["ctrl"]` and the parse fails. Worse, on a US layout the
   character `+` **is** `Shift`+`=` — ImGui reports `ImGuiKey_Equal` with `io.KeyShift` set — which
   `MatchHotkey`'s exact-modifier rule can never match against a `Ctrl+=` binding.
3. The numpad `+`/`-` keys are absent from the grammar entirely (`ImGuiKey_KeypadAdd` /
   `KeypadSubtract` exist in the pinned ImGui `v1.92.7-docking`, just never mapped).
4. `FirstBindableKeyPressedThisFrame` accepts only A–Z, 0–9, F1–F12, Comma, Space, Enter — so a user
   **cannot capture `Ctrl+=` in the UI even today**, only receive it as a default or hand-edit
   `smatchet_config.json`.

**Intended outcome — after this lands**: one action (`CommandId` + `ArgsJson`) carries N alternative
combos, any of which fires it; the grammar can express a literal `+` and the numpad keys; the
Preferences editor manages the set as chips; `ui.zoom.in` ships bound to `Ctrl+=`, `Ctrl+Shift+=` and
`Ctrl+NumAdd`.

**Scope locked with the user (do NOT re-ask):** alternate combos only — press-then-press **chords stay
out of scope**; both main-row and numpad keys; full editor support.

## Approach

Promote `Keybinding::Hotkey` (one string) to `Keybinding::Hotkeys` (a vector), keeping the
`(CommandId, ArgsJson)` pair as the *action key* it already is everywhere in the codebase. The
alternative — several rows sharing one `(CommandId, ArgsJson)` — was rejected because that pair is
load-bearing: the editor's row identity `rowKey = CommandId + "\x1f" + ArgsJson` would collide (arming
capture on two rows at once), the three pair-keyed mutators would each become ambiguous, `Enabled`
would fragment into one checkbox per alias, and `BoundHotkeyDisplay*` would return whichever row
happened to be first. The vector member keeps every one of those contracts intact and makes the
compile break loud and mechanical.

The grammar gains two narrowly-scoped extensions. A `+` that would produce an empty token **and is the
final character of the right-trimmed spec** becomes a literal plus main key — the "final character"
restriction (rather than the looser "any empty token") guarantees byte-identical tokenization for every
currently-parseable spec. And a small `kShiftedKeys` table normalizes `+` → `{shift, Equal}` and `_` →
`{shift, Minus}` at parse time, because `NamedKey` carries no modifier and the exact-modifier matcher
demands the shift flag be right. `Stringify` deliberately renders the canonical `Ctrl+Shift+=` form, so
`Ctrl++` is an accepted *input* spelling that canonicalizes — asymmetric on purpose, and test-pinned.

**Non-obvious trade-off that shaped the design**: the dispatch loop deliberately does not `break` on a
match (distinct actions sharing a keystroke all fire). Flattening N combos into that loop therefore
opens a *new* hazard the single-combo model could not have — one action firing twice when two of its
own aliases land in the same frame (`Ctrl` held while `=` and numpad `+` both arrive). A per-action
frame stamp, sized at cache-rebuild time and written only on a match, closes it without touching the
steady-state no-match path.

## Files to modify

### Grammar + capture (commit 1 — data-model-neutral, green standalone)

1. **[`Source/Core/include/Ui/ImGuiHotkey.h:26`](../../../Source/Core/include/Ui/ImGuiHotkey.h) + [`Source/Core/src/Ui/ImGuiHotkey.cpp:113`](../../../Source/Core/src/Ui/ImGuiHotkey.cpp)** — trailing-literal-`+` tokenizer; `kShiftedKeys` table; numpad rows in `kNamedKeys`; new `SameCombo` + `BindableImGuiKeys()`. The header's key-set doc comment (`:26-32`) enumerates the grammar and goes stale otherwise.
2. **[`Source/Core/include/Ui/SmatchetHotkeyCapture.h:22`](../../../Source/Core/include/Ui/SmatchetHotkeyCapture.h) + [`Source/Core/src/Ui/SmatchetHotkeyCapture.cpp:20`](../../../Source/Core/src/Ui/SmatchetHotkeyCapture.cpp)** — `FirstBindableKeyPressedThisFrame` loops `BindableImGuiKeys()` instead of its own hand-written list (which would otherwise duplicate `kNamedKeys` and rot).
3. **[`tests/Core/ImGuiHotkey.test.cpp`](../../../tests/Core/ImGuiHotkey.test.cpp)** — § Verification Bucket A.

### Data model + dispatch + editor (commit 2)

4. **[`Source/Core/include/Config/KeybindingsConfig.h:19`](../../../Source/Core/include/Config/KeybindingsConfig.h) + [`Source/Core/src/Config/KeybindingsConfig.cpp:25`](../../../Source/Core/src/Config/KeybindingsConfig.cpp)** — `Hotkeys` vector + four `Keybinding` accessors; dual-write/dual-read JSON; `SetBindingHotkey` redefined replace-all; new `AddBindingHotkey`/`RemoveBindingHotkey`; shared `FindDisplayBinding` + new `BoundHotkeyDisplayAll`; `MakeBindingMulti` + the three zoom `Defaults()` rows. **STRICT lint zone.** Header stays `json_fwd`-only (dual-target DX12 discipline) — all bodies in the `.cpp`.
5. **[`Source/Core/include/Ui/SmatchetUI.h:189`](../../../Source/Core/include/Ui/SmatchetUI.h) + [`Source/Core/src/Ui/SmatchetUI.cpp:908`](../../../Source/Core/src/Ui/SmatchetUI.cpp)** — `ParsedKeybinding::actionIndex` + `actionLastFiredFrame_`; `rebuildKeybindingCache` emits one entry per combo; `dispatchKeybindings` de-dups per action per frame. Also replaces the hand-rolled `BoundHotkeyDisplay` equivalent at `:1254-1258` with the real call (DRY).
6. **[`Source/Core/src/Ui/SmatchetHotkeyCapture.cpp:133`](../../../Source/Core/src/Ui/SmatchetHotkeyCapture.cpp)** — `FindKeybindingConflict` gains an inner combo loop; new `FindKeybindingConflictForRow`; `DrawHotkeyRebindControl` gains a **defaulted** `armLabel` param; `QuickBindPopup` seeds from `PrimaryHotkey()` and shows an "Also bound" line.
7. **[`Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp:169`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp)** — chip row (§ Editor row); two file-local extractions (§ Extraction sizing); `RowMatchesFilter` vector overload; Clear-all.
8. **[`Source/Core/include/Config/ConfigManager.h:284`](../../../Source/Core/include/Config/ConfigManager.h), [`Source/Core/src/Config/ConfigManager_Save.cpp:177`](../../../Source/Core/src/Config/ConfigManager_Save.cpp), [`Source/Core/src/Config/ConfigManager_Load.cpp:332`](../../../Source/Core/src/Config/ConfigManager_Load.cpp)** — `MigratedMultiHotkeyZoomV1` field + wire key + `MigrateZoomHotkeyAliasesV1` body, invoked 5th at `:470`. **STRICT lint zone.**
9. **[`Source/Core/src/Ui/SmatchetToolbarUi.cpp:277`](../../../Source/Core/src/Ui/SmatchetToolbarUi.cpp)** — the hover tooltip is roomy enough to surface every combo → `BoundHotkeyDisplayAll`.
10. **[`Source/Core/src/SmatchetLocalization.cpp:191`](../../../Source/Core/src/SmatchetLocalization.cpp)** — four new EN/FR `kEntries` rows beside the existing `keybindings.editor.*` block.
11. **[`tests/Core/KeybindingsConfig.test.cpp:26`](../../../tests/Core/KeybindingsConfig.test.cpp)**, **[`tests/Core/ConfigMigration.test.cpp:361`](../../../tests/Core/ConfigMigration.test.cpp)**, **[`tests/ui/keybindings_editor_rebind.test.cpp`](../../../tests/ui/keybindings_editor_rebind.test.cpp)** — § Verification.
12. **[`docs/guides/keyboard-shortcuts.md:46`](../../guides/keyboard-shortcuts.md)** — the policy line, two new sections, and the defaults table.

All new test cases live in existing test TUs → **no** `tests/CMakeLists.txt` / `tests/ui/CMakeLists.txt` /
`ui_tests_registry.cpp` edits. `README.md` needs no change (it names the palette/assistant keys, not zoom).

## Existing utilities reused

- `ParseImGuiHotkey` / `StringifyImGuiHotkey` / `MatchHotkey` / `FindShortcutConflict` — [`Source/Core/include/Ui/ImGuiHotkey.h:33`](../../../Source/Core/include/Ui/ImGuiHotkey.h) — the grammar + matcher core; extended in place, not replaced.
- `DrawHotkeyRebindControl` / `CaptureImGuiHotkeyThisFrame` / `HotkeyNeedsModifier` / `HotkeyCaptureArmedRecently` / `QuickBindPopup` — [`Source/Core/include/Ui/SmatchetHotkeyCapture.h`](../../../Source/Core/include/Ui/SmatchetHotkeyCapture.h) — the `[+ Add]` control reuses the existing capture widget via one defaulted param rather than a second widget.
- `FindBindingIndex` / `RemoveBinding` / `BoundHotkeyDisplay` / `BoundHotkeyDisplayForArgs` — [`Source/Core/include/Config/KeybindingsConfig.h:43`](../../../Source/Core/include/Config/KeybindingsConfig.h) — contracts preserved verbatim; only the storage behind them changes.
- `MigrateMenuShortcutKeybindingsV1` — [`Source/Core/src/Config/ConfigManager_Load.cpp:256`](../../../Source/Core/src/Config/ConfigManager_Load.cpp) — the one-shot-flag migration pattern the new migration copies (with two documented divergences, § Risks).
- `smatchet::json_safe::ParseBounded` — the mandatory JSON entry point; a bare `nlohmann::json::parse(` is a blocking lint rule.
- `MarkKeybindingsDirty()` [`Source/Core/include/Ui/SmatchetUI.h:108`](../../../Source/Core/include/Ui/SmatchetUI.h) + `SmatchetUiTestMarkKeybindingsDirty()` — cache-invalidation seam + the bucket-E hook.
- `SmatchetLocalization::T` / `::Format` — all new user-visible strings.

## Editor row — chip layout (in-place rebind preserved)

The naive "chips are read-only labels + an `[+ Add]` button" shape **regresses the existing UX**: today
a row's single combo is rebound in one click via `DrawHotkeyRebindControl`, and Add-only would make
that remove-then-add. So each chip's **label is itself the arm affordance**:

```text
[ Ctrl+= ] x   [ Ctrl+Shift+= ] x   [ Ctrl+NumAdd ] x   [ + Add ]
Ctrl+Shift+= conflicts with Toggle Performance
```

- `DrawHotkeyRebindControl` gains a **defaulted** 5th param `const char* armLabel = nullptr`:
  `nullptr` keeps today's exact rendering (display text + a `"Click to rebind"` button), so
  `QuickBindPopup` and bucket-E Test E/F are untouched; non-null renders a single `SmallButton(armLabel)`
  as the arm affordance. A chip passes the combo string as `armLabel`; `[+ Add]` passes the new
  `keybindings.editor.addCombo` string. One control, two renderings — no second capture widget.
- `capturingKey` gains a slot discriminator: `rowKey + "\x1f" + <slot>`, where `<slot>` is the combo
  index or the literal `add`. The "one control captures at a time" invariant behind
  `DrawHotkeyRebindControl`'s function-scoped `s_showNeedsModifierWarning` static still holds.
- Committing on a chip **replaces that slot** (dedup against the row's other combos); committing on
  `[+ Add]` appends via `AddHotkey`. Both set `b.Enabled = true`.
- `x` removes that slot; **removal is deferred** — latch `int pendingRemove = -1` and erase *after* the
  loop, since erasing mid-loop invalidates both the iteration and the ImGui id sequence.
- **Clear** (column 2) now clears *all* combos (`b.Hotkeys.clear()`), label unchanged, with a new
  `keybindings.editor.clearTooltip` explaining the widened scope.

Free structural coverage: [`tests/ui/funcsize_preferences_tabs.test.cpp`](../../../tests/ui/funcsize_preferences_tabs.test.cpp)
live-ticks every Preferences page and traps any in-frame `IM_ASSERT` — it will catch a `PushID`/`PopID`
or `Begin`/`End` imbalance in the chip loop without a new test.

## Extraction sizing

`DrawKeybindingsSectionBody` ([`SmatchetPreferencesUi_Keybindings.cpp:169-329`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp)) is **161 lines** today and the chip UI adds to it. Two file-local extractions in the same TU:

1. `DrawBindingCombosCell(app, allBinds, b, rowKey, capturingKey) -> bool` — **EXTRACT**: chips + deferred remove + `[+ Add]` capture + conflict warning (~45 lines out, ~4 back as the call).
2. `DrawResetDefaultsConfirm(d, capturingKey) -> bool` — **EXTRACT**: the confirm modal at `:291-310` (~20 out, ~3 back).

What **STAYS** in `DrawKeybindingsSectionBody`: the filter input, table setup/headers, the row loop skeleton, the label + `Enabled` + `Clear` cells, and the "Add shortcut for a command…" / "Reset all to defaults" buttons.

**Target: 161 − 65 + 7 ≈ 101 lines** — clears the 200-line ImGui-draw hard cap with room for the chip logic's own growth, and lands at the 100-line soft-warn tier. `DrawKeybindingsPreferencesTab` (22 lines) is untouched.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: dispatch-loop entries go 27 → ~32 with the shipped defaults, each still 4 bool compares + `IsKeyPressed` against a pre-parsed POD. **No per-frame parsing, allocation, or string work is added**; the de-dup writes a single `int` and only on a match. `actionLastFiredFrame_` is sized in `rebuildKeybindingCache`, never per frame. The `kNamedKeys` scan (30 → ~48 rows) runs only at config load / editor mutation.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new sync I/O; the editor persists through the existing off-thread `EnqueueTrackerConfig`.
- **Pillar 3 (never crash)**: an unparseable combo is skipped with `LOG_WARN` (already the contract, now per-combo); the migration uses `j.value` only — no `json::parse`, no new `bare-json-parse-untrusted` exposure; the chip remove is **deferred past the loop**, avoiding the mid-loop container-invalidation class the `ViewState` rule in [`Source/Core/src/Ui/AGENTS.md`](../../../Source/Core/src/Ui/AGENTS.md) exists for.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: net **positive**. Alias combos let a user reach an action from whichever key is physically comfortable, and widening the capture set finally makes `=`, `-` and the numpad bindable at all. No new contrast surfaces (chips reuse existing prefs styling).

## Perf-review-system gates (diff touches `Source/Core/` — MANDATORY)

Per [`docs/plans/pillar-1-2-perf-review-system.md`](../pillar-1-2-perf-review-system.md).

1. **PR-fast CI** — **fires**: the changed hot path is `dispatchKeybindings`, called every frame from `drawPreWindowOverlays` ([`SmatchetUI.cpp:616`](../../../Source/Core/src/Ui/SmatchetUI.cpp)). Run the steady-state UI scenario that the curated diff→scenario map in `agents/core/perf-gatekeeper.md` binds to `Source/Core/src/Ui/SmatchetUI.cpp`; if that path is absent from `scripts/dev/perf-pr-fast-set.json`, add it in this PR.
2. **Pillar 2 static scanner** — **N/A**: no new sync I/O (`cpr`/`SQLite`/`p4`/file) reachable from `ImGui::*`; the only write is the existing off-thread `EnqueueTrackerConfig`.
3. **Dispatcher drain** — **N/A**: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — **N/A**: adds no sync-stall path > 100 ms.
5. **Marker inventory** — **N/A**: no new `SMATCHET_UI_PERF_SCOPE` markers (reuses the `drawPreWindowOverlays` scope).

**Pre-push local check**: run [`docs/guides/perf-workflow.md`](../../guides/perf-workflow.md) § Gate-check vs baseline (Step 7) against the named scenario before opening the PR.

**Override**: none expected.

## Risks / non-goals

- **Risk — `Ctrl+Shift+=` is a NEW default combo and may collide with a user's own binding.** Mitigated: the migration widens only *untouched* rows, the editor's live conflict-warn surfaces any collision, and project policy is warn-not-block. Verified none of the new combos collides with an existing `Defaults()` entry (`Ctrl+Shift+=` is unused).
- **Risk — `+` ≡ `Shift+=` is a US/ANSI-layout assumption.** ImGui reports the US-position key. Mitigated: the layout-independent `Ctrl+NumAdd` ships alongside, and capture always records the physically-observed key. Documented in the user guide.
- **Risk — NumLock off may report `Keypad0`–`9` as Home/End on some backends** (`KeypadAdd`/`KeypadSubtract` are unaffected). `Ctrl+Num0` (the zoom-reset alias) is therefore the weakest of the three new keypad aliases (`Ctrl+NumAdd`, `Ctrl+NumSubtract`, `Ctrl+Num0`); **accepted** — harmless when unreported, noted in the guide.
- **Risk — an old build that SAVES drops the `hotkeys` array.** Mitigated by dual-write: `hotkey` still carries the primary combo, so a downgrade degrades to one working combo rather than an unbound action. Full downgrade round-trip is **accepted** as out of scope; documented.
- **Risk — hand-edited `Ctrl++B`-style specs changing meaning.** The trailing-only literal rule makes this a **provable zero-delta** (`Ctrl++B` stays `{ctrl, B}`); pinned by a bucket-A test.
- **Risk — `dup_audit.py` (blocking delta gate) on the new conflict / display / factory helpers.** Pre-empted by three extractions that each *remove* an existing clone: `SameCombo` (today duplicated between [`ImGuiHotkey.cpp:192`](../../../Source/Core/src/Ui/ImGuiHotkey.cpp) and [`SmatchetHotkeyCapture.cpp:152`](../../../Source/Core/src/Ui/SmatchetHotkeyCapture.cpp)), `FindDisplayBinding` (shared by all three `BoundHotkeyDisplay*`, carrying the two existing `SMATCHET_DEVIATION(rule=bare-json-parse-untrusted)` comments once instead of twice), and `MakeBinding` → `MakeBindingMulti`.
- **Risk — bucket-E Test E's `ItemClick("**/Click to rebind##rebindcaptureTest")` breaking.** The `armLabel` param is defaulted `nullptr`; the default button label and ImGui id are untouched. Verified no bucket-E test clicks the *editor row's* rebind control (Test D rebinds programmatically via `SetBindingHotkey`, Test E targets a synthetic replica window, Test F the quick-bind popup), so the chip redesign of the row cell breaks no existing case — and Test D's expectation matches `SetBindingHotkey`'s new replace-all semantics exactly.
- **Risk — losing the one-click in-place rebind** when chips replace the row's single rebind control. Mitigated by making the chip label the arm affordance (§ Editor row) rather than a read-only tag.
- **Risk — visual-validation exception** (the diff touches `Smatchet*Ui*.cpp`). Discharged by the new bucket-E `ComboChipsAddAndRemove` case; if that case has to be dropped, the orchestrator **must** pause with a launched exe per `AGENTS.md` § Autonomous ship-loop default exception 5.
- **Non-goal — press-then-press chord sequences.** Confirmed out of scope with the user. The Zen `Ctrl+M→Z` chord and `Esc-Esc` stay special-cased and non-rebindable. Chords would reuse the flattened cache but need a pending-prefix state machine + timeout — a separate plan.
- **Non-goal — per-tracker / per-view keybindings; mouse or gamepad bindings; keybinding profile import/export.** Unchanged from the v1 scope calls.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`) — [`tests/Core/ImGuiHotkey.test.cpp`](../../../tests/Core/ImGuiHotkey.test.cpp)**:
  - *Zero-regression pin*: `"Ctrl+="`, `"Ctrl+-"`, `"Ctrl+,"`, `"Ctrl+Shift+B"`, `"  CTRL +  B "`, `"Ctrl+A+B"` parse identically to today; `"Ctrl+"` / `"Ctrl+Shift"` still **fail**; `"Ctrl++B"` still yields `{ctrl, B}`.
  - `"Ctrl++"` / `"Ctrl+Plus"` / `"Ctrl+ +"` → `{ctrl, shift, Equal}`; bare `"+"` → `{shift, Equal}`; `"Ctrl+_"` / `"Ctrl+Underscore"` → `{ctrl, shift, Minus}`.
  - *Asymmetry pin*: `StringifyImGuiHotkey(Parse("Ctrl++")) == "Ctrl+Shift+="`.
  - *Collision pin*: `FindShortcutConflict({Parse("Ctrl+Shift+=")}, Parse("Ctrl++")) == 0`.
  - Numpad tokens (`Num0`–`Num9`, `NumAdd`, `NumSubtract`, `NumMultiply`, `NumDivide`, `NumDecimal`, `NumEnter`, `NumEqual`) + the `KeypadAdd`/`KeypadSubtract` aliases resolve; `KeyToToken` renders the canonical spelling.
  - Extend the round-trip table with `"Ctrl+Shift+="`, `"Ctrl+NumAdd"`, `"Ctrl+NumSubtract"`, `"Ctrl+Num0"` — **not** `"Ctrl++"` (non-canonical by design; it would fail the `canonical == spec` assertion, which is the point).
  - `BindableImGuiKeys()`: every entry has a non-empty `KeyToToken` and survives Stringify→Parse; `ImGuiKey_Escape` is absent.
- **Bucket A — [`tests/Core/KeybindingsConfig.test.cpp`](../../../tests/Core/KeybindingsConfig.test.cpp)**:
  - Order-sensitive `Defaults()` parity table (`:26-59`, 27 rows): `ExpectedBinding::hotkey` becomes `const char* hotkeys[4];` (nullptr-terminated; C++14 aggregate init zero-fills the rest), with the three zoom rows updated.
  - `to_json` emits **five** wire fields with `hotkey == hotkeys[0]`; `from_json` legacy-only → 1-combo list, new-only → order preserved, both-duplicate → deduped, both-disjoint → legacy appended last, non-string / empty entries skipped, malformed binding still skipped.
  - `SetBindingHotkey` **replaces** the alias list (3-combo zoom row → 1).
  - `AddBindingHotkey` / `RemoveBindingHotkey` / `Keybinding::{Add,Remove}Hotkey`: dedup, remove-missing → false, removing the last combo leaves the row listed and unbound.
  - `BoundHotkeyDisplay*` returns the **primary** (pinned explicitly, so a future "join all" change breaks a test); `BoundHotkeyDisplayAll` returns the joined form and "" when unbound.
- **Bucket A — [`tests/Core/ConfigMigration.test.cpp`](../../../tests/Core/ConfigMigration.test.cpp)** (mirrors the `:361-429` fixture): legacy `hotkey:"Ctrl+="` → 3-combo list + flag set; user rebind untouched; user-cleared stays empty; an absent row stays absent (no resurrection); flag already `true` → no-op; re-`Load` idempotent; Save→Load round-trip keeps the aliases. Existing assertions at `:387,410,446,469,533,551` move from `.Hotkey ==` to `.PrimaryHotkey() ==`.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`) — [`tests/ui/keybindings_editor_rebind.test.cpp`](../../../tests/ui/keybindings_editor_rebind.test.cpp)**:
  - **Marquee acceptance** `ZoomAliasCombosAdjustFontSize`: `Ctrl|Shift|Equal` raises `g_ui.cfg.FontSizePt`, then `Ctrl|KeypadAdd` raises it again — the user's literal request, end-to-end.
  - `ZoomOutAliasCombo`: `Ctrl|KeypadSubtract` lowers it.
  - Extend the capture test: capture `Ctrl+Shift+=` → the committed string contains `"="`; capture `Ctrl|KeypadAdd` → `"NumAdd"` (proves the widened capture set).
  - New replica-window `ComboChipsAddAndRemove` (mirrors the existing E/F pattern, avoiding the docked-window clipping trap): host `DrawBindingCombosCell` on a synthetic 2-combo row; a chip's `x` drops it to 1; clicking a chip label arms capture and commits **in place** (proving the preserved rebind affordance); `+ Add` + capture appends. **This case discharges the visual-validation exception.**
  - Existing [`tests/ui/funcsize_preferences_tabs.test.cpp`](../../../tests/ui/funcsize_preferences_tabs.test.cpp) already live-ticks the Keybindings page and traps in-frame `IM_ASSERT`s — free `PushID`/`PopID` and `Begin`/`End` balance coverage for the chip loop. Must stay green; no edit needed.
  - Seed the existing conflict-render test's collision on a multi-combo row.
  - Best-effort `NoDoubleFireOnSimultaneousAliases` (`Ctrl` + `Equal` + `KeypadAdd` in one frame → `FontSizePt` rises by exactly 1). If frame granularity makes it flaky, drop it and record that in § Deviations — not silently.
- **Bash-driver scenario / screenshot / sanitizer**: sanitizer build clean (Pillar 3). No new CLI scenario — the zoom commands already dispatch through the registry and are covered by the existing command path.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — the `Hotkeys` rename must compile under DX12, hence the `json_fwd`-only header discipline).
- **Lint gate**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` — watch `function-too-long`, `duplication` (blocking), `bare-json-parse-untrusted` (blocking), `tu-line-ceiling`.
- **Doc validation (blocks plan-doc PRs)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; record the outcome here.
- **Manual residue**: none expected — the marquee acceptance is bucket-E automated. If `ComboChipsAddAndRemove` cannot be landed, that is manual residue → add a `docs/self-improvement/categories/tooling.md` entry naming the deferred automation. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (multi-key chords, per-tracker keybindings) and revise or delete them.

- **Press-then-press chord sequences** — follow-up plan; the flattened cache is a usable substrate but the pending-prefix state machine + timeout is its own design.
- **Per-tracker / per-view keybindings** — unchanged from v1; global only.
- **Mouse-button / gamepad bindings** — no-action; outside the keyboard-shortcut scope.
- **Keybinding profile import/export** — no-action; the config already round-trips, so a future export stays cheap.
- **Full old-build downgrade round-trip** (an old build re-saving preserves `hotkeys`) — no-action; dual-write already prevents the destructive case.

## Implementation log

- **Commit 1 — `feat(hotkeys): parse literal + and keypad tokens; derive the capture key set`**
  (`ImGuiHotkey.{h,cpp}`, `SmatchetHotkeyCapture.{h,cpp}` capture path, `tests/Core/ImGuiHotkey.test.cpp`).
  Trailing-literal-`+` tokenizer; `kShiftedKeys` (`+`/`plus` → Shift+`=`, `_`/`underscore` → Shift+`-`);
  13 keypad rows + 4 alias rows in `kNamedKeys`; `SameCombo` extracted (removes the existing
  `FindShortcutConflict` ↔ `FindKeybindingConflict` clone); `BindableImGuiKeys()` derived from the
  grammar's own tables, which `FirstBindableKeyPressedThisFrame` now scans; `DrawHotkeyRebindControl`
  gained the defaulted `armLabel` param. Data-model-neutral and green standalone.
- **Commit 2 — `feat(keybindings): bind several alternative combos to one action`** (everything else).
  `Keybinding::Hotkeys` + the four accessors; dual-write/dual-read JSON; `SetBindingHotkey` documented
  replace-all plus new `AddBindingHotkey`/`RemoveBindingHotkey`; `FindDisplayBinding` shared by the
  three `BoundHotkeyDisplay*` (new `...All`); `MakeBindingMulti` + the three zoom `Defaults()` rows;
  flattened `keybindingCache_` with `actionIndex` + `actionLastFiredFrame_` de-dup; chip-based editor
  row with `DrawBindingCombosCell` + `DrawResetDefaultsConfirm` extracted; `FindKeybindingConflictForRow`;
  `MigrateZoomHotkeyAliasesV1`; 4 EN/FR locale entries; toolbar tooltip → `BoundHotkeyDisplayAll`;
  dock-debug overlay lookup → the real `BoundHotkeyDisplay`; bucket-A + bucket-E test updates; user doc.

## Deviations from plan

- **Editor row shape refined before implementing** (already folded into § Editor row above, recorded here
  for the reviewer): the approved sketch had read-only chips + an "Add" button, which would have
  *regressed* the existing one-click in-place rebind into remove-then-add. Each chip's label is now its
  own arm button, and `capturingKey` gained a slot discriminator (`rowKey + "\x1f" + <slot|add>`).
- **New bucket-E seam not in the original file list**: `SmatchetPreferencesUiDetail::DrawKeybindingCombosCellForTest`
  (`SMATCHET_BUILD_UI_TESTS`-guarded forwarder in `SmatchetPreferencesUi_Keybindings.cpp` + a declaration
  in `SmatchetPreferencesUi_detail.h`). Hosting the *real* chip cell in the bucket-E replica window beats
  reimplementing the layout in the test. `Source/Core/src/Ui` is not on the UI-test target's include path,
  so the test includes the private companion header by explicit relative path rather than editing CMake.
- **`NoDoubleFireOnSimultaneousAliases` bucket-E case dropped**, per the plan's own instruction to say so
  rather than drop it silently. It could not be run or even compiled in this environment (below), so
  landing an unrunnable, frame-timing-sensitive test would have been noise. The de-dup is covered by the
  `actionLastFiredFrame_` code and review; the residue is one unverified guard.
- **No wholesale `clang-format -i`.** The committed tree is not clang-format-clean under the repo's own
  `.clang-format` (LLVM's `AccessModifierOffset: -2` vs the committed column-0 `public:`), so formatting
  whole files reflowed untouched code. Format-only churn was reverted and new code matched to its
  surroundings by hand.
- Bare `///` separator lines inside the new doc comments tripped `comment-decorative-banner` (blocking);
  the paragraphs were joined instead. No content change.
- **Review round (CodeRabbit on PR #2013): `FindKeybindingConflictForRow` replaced by
  `ComputeKeybindingRowConflicts`.** The per-row lookup this plan named re-parsed every combo of every
  row for each visible row, every frame the editor page was open (~rows × combos string parses against
  the 6.94 ms budget). The whole-table pass parses each combo once per frame and compares PODs; the
  chip cell now receives its row's precomputed conflict strings, which also dropped the cell's `app` +
  `allBinds` parameters (and the seam's). Same review round: the duplicated find-or-append body in
  `SetBindingHotkey`/`AddBindingHotkey` was extracted to a file-local `UpsertBindingRow`; the quick-bind
  "replaces every combo" line now renders only when the resolved display string is non-empty (a
  disabled multi-combo row yielded an empty list); the add-button ImGui id became the locale-stable
  `###kbAddCombo`; the two new capture sub-cases assert the arm step; and the dead single-string
  `RowMatchesFilter` overload (orphaned by the vector overload, a latent `-Werror` unused-function
  break on the Windows build) was deleted.

## Verification (actual)

Read from real command output, not assumed. **This session ran on a Linux container where the MSVC
presets and the ImGui doctest/UI rigs are `[n/a]` (`project.config.json` § environments)**, so the
coverage split below is uneven and stated plainly.

**Ran and green:**
- **Grammar bucket A** — `tests/Core/ImGuiHotkey.test.cpp` via a local harness (the file's owning target
  is MSVC-only; `ImGuiHotkey.cpp`'s single ImGui call is `IsKeyPressed` inside `MatchHotkey`, which the
  test file already excludes, so a stub satisfies the link): **19 cases / 4745 assertions / 0 failed**,
  compiled `-Wall -Wextra -Werror`.
- **Config + migration bucket A** — `tests/Core/KeybindingsConfig.test.cpp` and
  `tests/Core/ConfigMigration.test.cpp` compiled with the preset's own flags and linked against the
  `ninja-test-linux` objects: **279 cases / 2661 assertions / 0 failed** (includes the 234-case
  `SmatchetTsanTests` subset, unchanged and still green).
- **Linux subset build** — `cmake --preset ninja-test-linux` + `ninja SmatchetTsanTests`: `CONFIGURE_EXIT:0`,
  `BUILD_EXIT:0`, no warnings.
- **Lint** — `test-lint-rules.sh --diff origin/develop`: `LINT_EXIT:0`, every hard rule PASS (strict-zone,
  comment-noise, no-raw-new, catch-all, oversized-function, include-cycle, fan-in, agent-size). Remaining
  are advisory WARNs only: `tu-line-ceiling` on two pre-existing whales, two soft-tier branch-count WARNs
  (`ParseImGuiHotkey` 22, `dispatchKeybindings` 21, cap 30 blocking / 20 soft), and comment-ratio WARNs on
  the five headers whose doc comments grew.
- **Docs** — `scripts/dev/test-docs.sh`: `Passed: 19  Failed: 0`.

**NOT run — must be run on a Windows box before merge:**
- **Dual-target MSVC build** (`ninja-iter-msvc` → `SmatchetStandalone` + `SmatchetCore_DX12`). The Linux
  subset builds `Source/Core/src/Config/**` and the grammar, but **not** `SmatchetUI.cpp`,
  `SmatchetPreferencesUi_Keybindings.cpp`, `SmatchetHotkeyCapture.cpp` or `SmatchetToolbarUi.cpp`
  (`SMATCHET_BUILD_APP=OFF` — no GLFW/ImGui). Those four were verified by `clang++ -fsyntax-only` against
  the vendored ImGui headers, which is weaker than a real dual-target link.
- **Bucket E** (`ninja-ui-test-msvc`, group `"Keybindings"`) — **not compiled and not run**: this container
  has no GL headers. The three new/extended cases (`ZoomAliasCombosAdjustFontSize`, the widened-capture
  extension of `CaptureWidgetClickThenKeyCommits`, and the new `ComboChipsAddAndRemove`) are therefore
  **unverified**, including the `DrawKeybindingCombosCellForTest` include path. `funcsize_preferences_tabs.test.cpp`
  live-ticks the Keybindings page and would catch a chip-loop `PushID`/`PopID` imbalance — also unrun here.
- **Perf gate-check vs baseline** for the named steady-state UI scenario (§ Perf-review-system gates).

**Plan stress-test — `grill-with-docs`**: not run (its rig is part of the same Windows-side tooling).

**Manual residue**: the marquee acceptance — pressing `Ctrl+=`, `Ctrl+Shift+=` and `Ctrl`+numpad`+` in the
running app and watching the font grow by exactly 1 each — is automated in bucket E but unrun here, so it
is manual residue **for this session only**. Running the bucket-E "Keybindings" group on Windows clears it.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
