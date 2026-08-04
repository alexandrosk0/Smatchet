# Plan — Preferences IA re-segmentation + global in-place search

> **Slug**: `preferences-ia-resegmentation-and-search` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

User request: *"Propose a better segmentation of the preferences. For example Updates
options in the preferences should don't belong in Appearances. Also search should work on
all tabs."*

The Preferences window is 11 flat tabs in one `BeginTabBar("PreferencesTabs")`
([`SmatchetPreferencesUi.cpp:675`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp)).
Three concrete defects:

1. **Wrong homes.** Update settings (`UpdateCheckEnabled`, `UpdateIncludePrerelease`,
   check-now, skipped-version, `UpdateGithubRepo`) live inside the Appearance tab
   ([`SmatchetPreferencesUi_Local.cpp:396`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp)) —
   the user's cited example. Storage mode + "recreate DB" are stranded in a `Local data`
   tab; `P4Executable` / `P4VcExecutable` / `PathRemaps` / the two command templates are
   buried in `Annotate`; `User Info` is really VCS/activity-feed sources; `Grid` is a
   two-checkbox tab sitting next to a `Fields Inputs` tab that opens a **nested** tab bar
   ([`SmatchetPreferencesUi_Templates.cpp:446`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp)).
2. **No search.** The only filter is Keybindings-tab-local
   ([`SmatchetPreferencesUi_Keybindings.cpp:154-291`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp))
   and it matches command ids / hotkeys, never setting labels. There is no settings
   registry at all — every label, clamp, and config binding is hardcoded inline in its
   draw function, so nothing can enumerate "all settings".
3. **Mobile inherits the strip.** `MobilePage::Settings` calls
   `drawPreferencesWindow(app, d, /*embedded=*/true)`
   ([`SmatchetMobileShellUi.cpp:237`](../../../Source/Core/src/Ui/SmatchetMobileShellUi.cpp)),
   so a phone renders an 11-item horizontal tab strip.

**After this lands**: Preferences is a left category rail over 8 categories / ~22
collapsible sections, driven by a settings-descriptor table that is the single source of
truth for the taxonomy, and one search box filters **live, editable widgets in place**
across every category.

**Sequencing**: implementation starts only after
[`docs/plans/dev-onboarding-first-run-quickstart.md`](dev-onboarding-first-run-quickstart.md)
slice 2 merges — it rewrites `drawPreferencesTrackerTab`, adds `TrackerSetupCompleted` +
its serialization, and adds `UiDrawSession` prefs fields cleared at
`SmatchetPreferencesUi.cpp:155-157`. Its test-connection control + first-run gate then
relocate unchanged into `Tracker › Backend & credentials`.

## Approach

Introduce a **descriptor table** (`PreferencesSchema.{h,cpp}`) as pure data — categories,
sections, settings — with no ImGui dependency, and a **filter** (`PreferencesFilter.{h,cpp}`)
that indexes each setting's translated label + keywords + section + category title into one
haystack. The draw code then asks `st.Filter.ShowSetting("<id>")` before emitting each
widget. The table is deliberately a *second* source of truth alongside the draw code — the
self-registering alternative cannot index a category the user has not opened, which is
exactly what "search should work on all tabs" requires — so the duplication is paid for by
the drift guard below; rationale + rejected alternatives in
[ADR-0023](../../adr/0023-preferences-taxonomy-as-static-descriptor-table.md). Because the
taxonomy lives in data rather than in `BeginTabItem` call order,
re-parenting a section later is a one-field edit rather than a second restructure — which
is what makes the two-step slice 2a/2b split cheap.

The shell replaces `BeginTabBar` with a left `BeginChild` rail (search box on top,
`Selectable` per category) and a scrolling right pane, reusing the shape already shipped in
[`CommandPaletteUi.cpp:284-330`](../../../Source/Core/src/Commands/CommandPaletteUi.cpp).
Below a width threshold the rail collapses to a category `Combo` above the pane — no
navigation stack, and the phone tab-strip defect is fixed as a side effect.

Trade-off taken: the per-tab save-semantics footer switch
([`SmatchetPreferencesUi.cpp:697-736`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp))
cannot survive a category holding two save paths — the new `Connections` category mixes
`MarkPrefsDirty` with Annotate's detached `PersistAnnotateCfg`. So save semantics move onto
the **section** descriptor (`SaveSemantics` enum) and the hint renders per section. The
`Save & Sync` button stays in the footer.

### Target information architecture

8 categories, ~22 sections. Bold = **moved from another tab**.

| Category | Sections | Notable moves |
|---|---|---|
| **General** | Updates · Language & region · Storage · Local database | **Updates** ← Appearance; **UiLanguage / DateFormatOption / DateCompactRelativeThresholdDays** ← Appearance; Storage + Recreate-DB ← `Local data` (tab dissolves) |
| **Appearance** | Theme & font · Layout & density · Display | keeps `SelectedFontName`, `UiMode`, `MobileTouchDensity`, `MobileNavPages`, `MobileHomePage`, `VsyncEnabled` |
| **Tracker** | Backend & credentials · Recent projects | unchanged content (+ onboarding's test-connection & first-run gate) |
| **Connections** | MCP server · Perforce · Activity sources | MCP ← `Integrations`; **P4Executable / P4VcExecutable / PathRemaps / TimelapseCommandTemplate / ChangeCommandTemplate** ← Annotate; **git repos / production keyword / day window / max changes / feed layout** ← `User Info` |
| **AI & Voice** | Assistant · Agent context · Voice dictation · Voice diagnostics | Assistant + Whisper merge; `AgentsMd*` split into own section; Whisper's 3 test buttons become `Voice diagnostics` |
| **Editing** | Grid behaviour · Time estimates · Work log templates · Quick comments · Annotate comments | `Grid` + `Fields Inputs` merge; **nested tab bar dies** → sibling sections; **EnableFieldOverflowTooltips + GridEndWheelSwallowsBeforeHorizontal** ← Appearance |
| **Shortcuts** | Keyboard shortcuts · System shortcuts | tab-local filter kept for the dynamic command rows; global query forwards into it |
| **Annotate** | Analysis · Tracker field mapping · Colors | shrinks — Perforce paths/commands leave for Connections |

Dead after this: `Local data`, `Integrations`, `User Info`, `Grid`, `Fields Inputs` as
top-level entries, and the nested `BeginTabBar("FieldsInputsSubTabBar")`.

### Descriptor + filter shape

```cpp
struct PrefsCategoryDesc { const char* Id; const char* TitleEn; };
struct PrefsSectionDesc  { const char* Id; const char* CategoryId; const char* TitleEn;
                           SaveSemantics Save; };
struct PrefsSettingDesc  { const char* Id; const char* SectionId; const char* LabelEn;
                           const char* Keywords; };
```

`SaveSemantics` = `{ SaveAndSync, Autosave, Immediate, Restart, AnnotateDetached }`. Ids are
dotted and stable (`general.updates.check_enabled`, `connections.perforce.p4_exe`,
`editing.grid.single_click_edit`). `Keywords` is seeded with the **old** tab name so muscle
memory still resolves (`"integrations mcp server port"`).

**Localization: index by English source string, never by key.** There is deliberately no
`LabelKey` / `TitleKey` field. Six of the seven prefs TUs carry
`#define ImGui SmatchetLocalizedImGui`, so a widget's displayed label resolves through
`SmatchetLocalization::LabelFromSource(englishLiteral)` /
[`TranslateSource`](../../../Source/Core/include/SmatchetLocalizedImGui.h) — matched against
the `English` column of `kEntries[]` via `EntriesByEnglish()`
([`SmatchetLocalization.cpp:943`](../../../Source/Core/src/SmatchetLocalization.cpp)) — not
through the key-based `T(key, englishFallback)` path (which a minority of strings use in
addition). A `LabelKey` field would therefore be a **third** lookup path: any descriptor
whose key is absent from `kEntries[]` while its English source string *has* a row would
index the English text while the widget draws French, silently breaking search for that
setting in `fr` only. So the filter indexes `TranslateSource(LabelEn)` — the same call the
widget makes — which makes the indexed string byte-identical to the drawn one by
construction, in every language. Two consequences: (a) new category/section titles get
ordinary `kEntries[]` rows keyed by their English text, same as every other prefs string;
(b) the haystack is truncated at the first `##`, because `LabelFromSource` appends a stable
English-derived `###id` suffix when the label has none (so ImGui ids don't change with
language) and that suffix must not become searchable. Note
`AnnotateAnalysisUi_Preferences.cpp` has neither the `#define` nor any `T()` call — it is
unlocalized, so `TranslateSource` returns its English input unchanged, which is consistent
rather than a special case.

Call-site shape — the section body goes through one template helper so a `Begin`/`End` pair
never spans two functions (decomposition contract at
[`SmatchetUI.h:346-370`](../../../Source/Core/include/Ui/SmatchetUI.h)):

```cpp
PrefsSection(st, "appearance.display", [&] {
    if (st.Filter.ShowSetting("appearance.display.vsync")) {
        ImGui::Checkbox("Enable vsync", &cfg.VsyncEnabled);
    }
});
```

Matching is two-tier: `ContainsLower` substring first, `FuzzyScore(...) > threshold` only if
tier 1 returned nothing. `ShowSetting` returns `true` immediately on an empty query, so the
steady-state (non-searching) frame does zero extra work.

**Granularity**: one descriptor per *value*, not per widget. The list editors
(`DurationSuggestions`, the three template lists, `PathRemaps`, `MobileNavPages`,
`recentProjects`) get **one** descriptor for the whole editor — their per-row `InputText`s
are not individually filterable. The 7 Annotate `ColorEdit4` rows each get their own.
Measured: 85 value-widget calls across the 7 prefs TUs → **≈80 descriptors**.

**Feature gates**: MCP / Assistant / Whisper descriptor rows sit inside the same
`#if defined(SMATCHET_WITH_MCP)` / `_AI` / `_WHISPER` guards the draw code uses (11 guard
sites in `SmatchetPreferencesUi.cpp` alone). A descriptor compiled in while its widget is
gated out fails the coverage test in the feature-OFF build — the class
`unused-symbol-under-config-guard` (WARN) exists to catch.

**Drift guard**: `PreferencesFilter` records every id passed to `ShowSetting` during a
frame; a bucket-E test walks all categories with an empty query and asserts the observed id
set equals the descriptor set. It also asserts each observed setting's **rendered label**
matches its descriptor's `LabelEn` — an id-only check would pass a renamed label that kept
its id, which is exactly an invisible search break. Without this the table rots silently.

### Slices

Separate PRs on one feature branch, shipped as one feature (§ Merge gates PR-batching); the
2a/2b split keeps each diff under CodeRabbit's per-PR file ceiling.

- **Slice 1 — schema + filter core, no UI touched.** New TUs + bucket-A tests. No
  `Smatchet*Ui*.cpp` in the diff → no visual-validation pause.
- **Slice 2a — rail shell + section-ize, categories still 1:1 with today's tabs.** Rail
  replaces `BeginTabBar`; per-section save hint replaces the footer switch; each existing
  tab body is cut into section functions in place. Mechanical.
- **Slice 2b — re-parent sections to the new categories.** One `CategoryId` field edit per
  descriptor row + move the section function's call site. New localization rows for the 8
  category + ~22 section titles.
- **Slice 3 — per-setting gating (~80 call sites).** Wrap each setting in
  `if (st.Filter.ShowSetting("<id>"))`; live search box with `(showing N of M)`, matched
  sections auto-expand, empty-result state, global query forwarded into the Shortcuts
  tab-local `RowMatchesFilter`.

## Files to modify

Grep-checked before naming: `rg -l 'PreferencesSchema|PreferencesFilter|PrefsSection' Source/`
returns nothing — no existing TU or synonym to collide with.

**New (taxonomy + matching)**
1. `Source/Core/include/Ui/PreferencesSchema.h` + `Source/Core/src/Ui/PreferencesSchema.cpp` — the three descriptor tables. Data only, no ImGui include.
2. `Source/Core/include/Ui/PreferencesFilter.h` + `Source/Core/src/Ui/PreferencesFilter.cpp` — haystack index, two-tier match, `ShowSetting`, observed-id recorder for the drift guard.
3. `Source/Core/src/Ui/SmatchetPreferencesUi_Shell.cpp` — rail / category combo / `PrefsSection` helper.
4. `Source/Core/src/Ui/SmatchetPreferencesUi_General.cpp` — the new General category (Updates, Language & region, Storage, Local database).

**Shell + category pages**
5. [`Source/Core/src/Ui/SmatchetPreferencesUi.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp) `:675-691` tab bar → rail; `:697-736` footer save-semantics switch → per-section hint.
6. [`Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp) `:396` Updates block leaves for General; `:248` Local-data tab dissolves; keeps Appearance only.
7. [`Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp) `:446` nested `BeginTabBar("FieldsInputsSubTabBar")` → sibling sections under Editing.
8. [`Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp) + [`_Whisper.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp) — merge into the `AI & Voice` category as 4 sections.
9. [`Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp) `:47-56` — `ToLowerAscii` / `ContainsLower` move into `PreferencesFilter` (removes the duplicate, Pillar 5); `RowMatchesFilter` `:62` stays for the dynamic command rows and accepts the forwarded global query.
10. [`Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp`](../../../Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp) — Perforce block leaves for Connections; the detached `PersistAnnotateCfg:16` save path becomes `SaveSemantics::AnnotateDetached` on its sections.
11. [`Source/Core/src/Ui/SmatchetPreferencesUi_detail.h`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_detail.h) — section-function declarations.

**State + config + strings**
12. [`Source/Core/include/Ui/SmatchetUiSession.h`](../../../Source/Core/include/Ui/SmatchetUiSession.h) `:146-157` `enum class PreferencesActiveTab` → `PreferencesCategory` (not persisted today, free to change); member `:257`; add filter state + collapsed-section set.
13. [`Source/Core/src/Config/ConfigManager.cpp`](../../../Source/Core/src/Config/ConfigManager.cpp) `:201` — one new `std::string PreferencesCollapsedSections` row (comma-joined) in the existing `kStringFields[]` table; no bespoke serializer. Matching member in [`ConfigManager.h`](../../../Source/Core/include/Config/ConfigManager.h).
14. [`Source/Core/src/SmatchetLocalization.cpp`](../../../Source/Core/src/SmatchetLocalization.cpp) — `kEntries[]` rows for 8 category + ~22 section titles.

**Tests**
15. `tests/Core/PreferencesSchema.test.cpp`, `tests/Core/PreferencesFilter.test.cpp` (new) + `tests/CMakeLists.txt` rows — the bucket-A list is explicit, not globbed.
16. `tests/ui/prefs_schema_coverage.test.cpp`, `tests/ui/prefs_search_filter.test.cpp` (new); `tests/ui/funcsize_preferences_tabs.test.cpp` retargeted off `ItemClick("PreferencesTabs/…")` onto rail selectables.

**Build**: there is **no `Source/Core/CMakeLists.txt`**. Core TUs are picked up by
`file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")`
([`CMakeLists.txt:1060`](../../../CMakeLists.txt)), so the four new source files need no
build edit — only a re-configure if the cache predates them. The `SMATCHET_WITH_*` gating
is inside the TUs (not in source-list gating), so no per-file dual-target anchoring is
needed beyond the § Verification build line.

## Existing utilities reused

- `FuzzyScore(const std::string&, const std::string&)` — [`Source/Core/include/Commands/FuzzyMatch.h:20`](../../../Source/Core/include/Commands/FuzzyMatch.h) — tier-2 subsequence fallback; already shipped + tested, no new scoring code.
- `ToLowerAscii` / `ContainsLower` — [`SmatchetPreferencesUi_Keybindings.cpp:47,:56`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp) — *moved* into `PreferencesFilter` rather than copied; deletes the would-be duplicate.
- Search-box + `BeginChild` + `Selectable` + `SetScrollHereY(0.5f)` list shape — [`CommandPaletteUi.cpp:284-330`](../../../Source/Core/src/Commands/CommandPaletteUi.cpp) — the rail is the same pattern; don't invent a second one.
- Deferred-scroll flag idiom (`scrollToSelected` → `SetScrollHereY` next frame) — [`SmatchetAutocompleteUi_detail.h:14`](../../../Source/Core/src/Ui/SmatchetAutocompleteUi_detail.h) — scrolling the pane to the first match.
- Localized `CollapsingHeader` (+ `p_visible` overload) — [`SmatchetLocalizedImGui.h:165,:169`](../../../Source/Core/include/SmatchetLocalizedImGui.h) — section headers come translated for free.
- `SmatchetLocalization::TranslateSource(englishSource)` — [`SmatchetLocalization.h`](../../../Source/Core/include/SmatchetLocalization.h) — the filter indexes through the *same* English-source lookup the localized ImGui wrappers use, so no second translation path is introduced (see § Descriptor + filter shape § Localization).
- `template <typename T> struct FieldDesc { const char* key; T TrackerConfig::* member; }` — [`ConfigManager.cpp:188`](../../../Source/Core/src/Config/ConfigManager.cpp) — the collapsed-sections key is one `kStringFields[]` row, not a hand-rolled serializer.
- `PreferencesSliderDragScenario` (`"preferences-slider-drag"`) — [`Source/Core/src/Commands/Scenarios/PreferencesSliderDragScenario.cpp:38`](../../../Source/Core/src/Commands/Scenarios/PreferencesSliderDragScenario.cpp) — the existing perf hook for this window; must keep driving after the rail lands.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: `ShowSetting` short-circuits to `true` on an empty query, so the non-searching frame adds no per-frame work; while filtering, ≈80 hash lookups/frame on a modal window that draws far fewer widgets than the grid. The rail replaces a tab bar (one visible category's sections drawn instead of one tab's body — net neutral). Measured against the `preferences-slider-drag` scenario per § Perf gates.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new I/O. The filter index rebuilds on query change and on `cfg.UiLanguage` change only — an in-memory pass over ≈80 short strings, sub-millisecond. `Check for updates now` and the Whisper test buttons keep their existing async paths untouched.
- **Pillar 3 (never crash)**: descriptor tables are `static const` arrays of string literals; `ShowSetting` on an unknown id returns `true` (fail-open — a mis-typed id shows the setting rather than hiding it forever). The drift-guard bucket-E test turns any id mismatch into a red test rather than a silently invisible setting. No raw `new`/`delete`; `PrefsSection` takes the body by deduced callable (C++14, no `if constexpr` / `string_view`).
- **Pillar 4 (accessibility)**: net improvement — the rail is `Selectable`-based and keyboard-navigable, replacing a horizontal tab strip that overflowed at narrow widths, and the mobile combo removes the 11-item strip on phones. Font scaling unaffected (no fixed pixel sizes introduced beyond the rail width threshold, which is `GetFontSize()`-relative). No new WCAG contrast surface — section headers use existing theme colors.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

Diff touches `Source/Core/src/Ui/`, `Source/Core/src/Config/`, and `Source/Core/include/` → gates fire.

1. **PR-fast CI** — **the directly-exercising scenario is `preferences-slider-drag`, which is NOT in the PR-fast subset** ([`scripts/dev/perf-pr-fast-set.json`](../../../scripts/dev/perf-pr-fast-set.json) carries `idle`, `priority-grid-scroll`, `cell-edit-burst`, `ai-chat-history-render`, `side-by-side-2-grid`, `concurrent-sync`). Per `agents/core/perf-gatekeeper.md` § Curated diff → scenario map, `SmatchetPreferencesUi.cpp` (slider drag paths) → `preferences-slider-drag`. So PR-fast gives **no** coverage of this diff; the pre-push local run below is the real gate. Do **not** add `preferences-slider-drag` to the PR-fast set in this plan's PRs — it has no `ci-windows-latest` baseline and would bootstrap-on-first-run mid-feature; propose it separately if the coverage gap should be closed permanently.
2. **Pillar 2 static scanner** — **N/A**. No new sync I/O reachable from `ImGui::*`. The filter is pure in-memory string work; the update-check and Whisper-test buttons keep their existing (unmodified) async call paths.
3. **Dispatcher drain** — **N/A**. `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — **N/A**. No new sync-stall code path > 100 ms.
5. **Marker inventory** — **N/A** unless slice 2a's rail needs a scope. If any `SMATCHET_UI_PERF_SCOPE` is added, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `preferences-slider-drag`, run on slice 2a (rail lands) and slice 3 (per-setting gating lands). Slice 1 is non-UI → skip.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — intentional regression + baseline-bump PR queued only. Not expected here.

## Risks / non-goals

- **Muscle-memory break** — users who know "Updates is under Appearance" lose the path. Mitigated: every setting's `Keywords` is seeded with its old tab name, so searching `appearance` still surfaces Updates. Accepted otherwise; this is the point of the change.
- **Descriptor/draw drift** — the table can silently disagree with what actually draws, leaving a setting unfindable. Mitigated by the bucket-E drift guard (observed-id set == descriptor set) and by `ShowSetting` failing open on an unknown id.
- **Feature-OFF config skew** — an unguarded MCP/AI/Whisper descriptor row breaks the coverage test only in the feature-OFF build. Mitigated by the explicit feature-OFF verification step; this is the `unused-symbol-under-config-guard` (WARN) failure class.
- **DRY (Pillar 5) WARN** — ~22 same-shaped section functions will read as copy-paste to `dup_audit.py`. Mitigated: shared chrome lives in `PrefsSection`, section bodies stay thin. WARN-first per [ADR-0015](../../adr/0015-dry-quality-pillar-duplication-gate.md), so it won't block; if it fires, take the exemption with `code-review` sign-off rather than fusing unrelated sections.
- **`function-too-long`** — net improvement expected (extraction reduces the long category draw functions), but re-check `DrawWhisperPreferencesTab` / `DrawAssistantPreferencesTab` after section-izing.
- **Collision with in-flight onboarding plan** — [`dev-onboarding-first-run-quickstart`](dev-onboarding-first-run-quickstart.md) rewrites `drawPreferencesTrackerTab` and adds `UiDrawSession` fields cleared at `SmatchetPreferencesUi.cpp:155-157`. Mitigated by sequencing: this plan starts only after its slice 2 merges.
- **Annotate is unlocalized** — `AnnotateAnalysisUi_Preferences.cpp` has zero localization today. In scope: the new section titles + any string this change adds. Body-string localization is **deferred** → owes a § Deviations row + a `docs/self-improvement/categories/debt/` entry in the same turn it is skipped.
- **Non-goal: settings as `Command`s.** `Command` ([`Command.h:159`](../../../Source/Core/include/Commands/Command.h)) is an action with no value binding, no widget, and no keywords field; registering ≈80 settings would pollute the palette without giving in-place editing.
- **Non-goal: per-setting deep links / anchors** from outside the window.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `PreferencesSchema.test.cpp` — ids unique, every section's `CategoryId` resolves, every setting's `SectionId` resolves, no empty label. `PreferencesFilter.test.cpp` — empty query returns everything, substring hit, fuzzy fallback fires only when substring misses, case-insensitivity, no-match returns empty. Existing `PreferencesCredentialTrim`, `PreferencesDateFormatIndex`, `ConfigManager*`, `ConfigMigration` must stay green (the new `PreferencesCollapsedSections` key round-trips).
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: `prefs_schema_coverage.test.cpp` (drift guard — walk all 8 categories with an empty query, assert observed-id set == descriptor set); `prefs_search_filter.test.cpp` (type `vsy` → 1 of ≈80 visible **and** the survivor is the real checkbox, toggled in place, `cfg.VsyncEnabled` flips); retargeted `funcsize_preferences_tabs.test.cpp`; unchanged `ai_assistant_preferences_*.test.cpp`, `ai_prefs_autosave_flow`, `annotate_prefs_persist_flow`, `keybindings_editor_rebind`, `help_marker_keyboard_focus` must stay green.
- **Bash-driver scenario / screenshot / sanitizer**: `preferences-slider-drag` scenario runs clean post-rail (`scripts/dev/perf-run.sh`); `scripts/dev/test-all.sh` covers the sanitizer leg.
- **Feature-OFF build**: configure without `SMATCHET_WITH_MCP` / `SMATCHET_WITH_AI` / `SMATCHET_WITH_WHISPER` and confirm `PreferencesSchema.test.cpp` + the coverage test still pass — this is where a mis-guarded descriptor row surfaces.
- **Build gate**: `bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target; a bare `cmake --build` from bash fails on `stdio.h`).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome here.
- **Lint**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` per slice.
- **Manual residue**: the visual-validation exception fires on slices 2a/2b/3 (`Smatchet*Ui*.cpp` in the diff) — the orchestrator pauses with a launched exe for the user's verdict on: all 8 categories open; a cross-category setting (`Enable vsync`, `Check for updates automatically`) persists across restart; collapsed-section state survives restart; `Save & Sync` still commits Tracker credentials; Annotate colors still save via the detached path; `UiMode = Mobile` Settings shows the category combo, not a tab strip. **Deferred-automation action plan**: the first three are automatable as bucket-E once the drift guard lands (config round-trip + rail navigation are both Test-Engine-reachable) — file a `docs/self-improvement/categories/tooling/` entry in slice 2a naming them, so the residue shrinks to genuine pixel judgement (rail spacing, mobile combo layout) rather than behaviour.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (`Local data` / `Integrations` / `User Info` / `Grid` / `Fields Inputs` as tab names; `PreferencesActiveTab`), and revise or delete them.

- **Deep mobile drill-down navigation** — the width-threshold combo is the whole mobile story here; a real drill-down stack belongs to [`docs/plans/mobile-app-fuller-integration.md`](mobile-app-fuller-integration.md).
- **Localizing existing Annotate preference body strings** — deferred; owes a § Deviations row + `docs/self-improvement/categories/debt/` entry (see § Risks).
- **Per-item filtering inside the list editors** (templates, path remaps, nav pages) — one descriptor per editor; per-row filtering is a follow-up if users ask.
- **Registering settings as `Command`s** — no-action, see § Risks non-goal.
- **Per-setting deep links / anchors from outside the window** — no-action; would need a stable public id contract beyond this plan's internal ids.
- **Adding `preferences-slider-drag` to the PR-fast perf subset** — real coverage gap (see § Perf gates 1) but needs a `ci-windows-latest` baseline landed by a human first; propose separately rather than bootstrapping mid-feature.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
