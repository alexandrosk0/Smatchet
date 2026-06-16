# UI text shortening + (?) help-marker tooltip

> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.

## Context

Smatchet UI has ~40 visible strings longer than 8 words or multiline — mostly in Preferences tabs, Annotate Analysis, Perf and Offline Queue. Long paragraphs inline waste vertical space, push controls below the fold, and make the panels feel noisy. User wants the visible text shrunk and the full original text reachable via a "(?)"-in-a-circle icon next to it that shows a hover tooltip.

Outcome: tight one-line labels in the UI, a reusable `SmatchetHelpMarker` widget rendered inline as `ICON_FA_CIRCLE_QUESTION`, hover surfaces the original full text in a wrapped tooltip. Behavior + localization intact.

## Inventory (from Phase 1 exploration)

Highest-impact callsites (full table in exploration result, this is the action list):

**Preferences — Assistant** (`Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp`)
- L474–480 — custom-endpoint security warning (16w + 28w `TextWrapped`)
- L592–595, L606–607, L616–617, L625–626, L635–636 — reasoning_effort / agents.md tooltips (12–18w each)

**Preferences — Local/Appearance** (`Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp`)
- L110–112 — DB reset consequences (22w)
- L169–177 — plugin/standalone storage mode (28w + 23w paragraphs)
- L268–359 — typography/lang/vsync/wheel/date tooltips (11–18w each)

**Preferences — Tracker/Integration** (`Source/Core/src/Ui/SmatchetPreferencesUi.cpp`)
- L219–221 read-only mode (14w)
- L290–297 GitHub repo/fields (14–15w)
- L394–404 MCP bind/auth/lua (14–18w)

**Preferences — Templates** (`Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp`)
- L42–44 long-text edit modal (22w)
- L54–55 estimate dropdown defaults (17w)

**Annotate Analysis** (`Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp`)
- L56–57, L128–137, L148–149, L164–169 (35w multiline), L181–182, L508–509

**Perf** (`Source/Core/src/Ui/SmatchetPerfUi.cpp`)
- L259–261 perf methodology (28w multiline)
- L324–326 HTTP totals (17w)

**Other**
- `SmatchetOfflineQueueUi.cpp:1304–1305` — conflict resolution (22w `TextWrapped`)
- `SmatchetPreferencesUi_Whisper.cpp:854–856` — PTT explanation (24w)
- `SmatchetAiAssistantUi.cpp:1201–1202` — per-turn reasoning (12w)
- `AnnotateAnalysisUi.cpp:76–77` — config blurb (13w)
- `SmatchetBugReportUi.cpp:322–323` — screenshot redaction (16w)
- `SmatchetGridHeaderUi.cpp:519–524` — MCP status (multiline)
- `SmatchetUI.cpp:168` — update banner (11w)

## Existing infrastructure (re-use)

- **Icon font** — Font Awesome 6 Solid merged at startup (`Source/Core/src/Ui/SmatchetImGuiFonts.cpp:251`); range `0xe005..0xf8ff` covers `ICON_FA_CIRCLE_QUESTION` (`"\xef\x81\x99"`, U+f059). Header `Source/Core/ThirdParty/IconsFontAwesome6/IconsFontAwesome6.h`. Use `SmatchetAreFaIconsLoaded()` (`SmatchetImGuiFonts.h:52`) for the "?" fallback path when atlas absent (tests / minimal-font mode).
- **Localization** — `SmatchetLocalization::T(key, englishFallback)` (`Source/Core/include/SmatchetLocalization.h:18`) and `SmatchetLocalizedImGui::SetItemTooltip(...)` (`Source/Core/include/SmatchetLocalizedImGui.h:232`, all-inline, at `include/` root — not under `Ui/`) — short label and long text both route through these. Translations are a hardcoded C++ table in `Source/Core/src/SmatchetLocalization.cpp` (no `Locales/*.json` file exists); new keys are introduced at the callsite by the `T("key", "english fallback")` argument — no separate locale file edit.
- **Tooltip style** — current sites use `ImGui::SetItemTooltip` without wrap; help-marker must `PushTextWrapPos(io.FontDefault->FontSize * 35.0f)` so long tooltips stay readable.
- **Theme** — no new color token needed; use `ImGuiCol_TextDisabled` for the dimmed glyph (matches existing `TextDisabled` callsites in templates tab).

## Approach

### New helper — `SmatchetHelpMarker`

New files:
- `Source/Core/include/Ui/SmatchetHelpMarker.h`
- `Source/Core/src/Ui/SmatchetHelpMarker.cpp`

CMake: **no edit required**. Top-level `CMakeLists.txt:868` `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")` picks up the new cpp; the header is covered by the existing include path. Re-run `cmake --preset` after creating to force the glob refresh.

```cpp
namespace SmatchetHelpMarker {
    // Renders a dimmed (?) glyph inline. Hover shows `fullText` in a wrapped tooltip.
    // Caller is responsible for ImGui::SameLine() placement before the marker.
    // tooltipKey/tooltipFallback follow SmatchetLocalization::T contract.
    void Render(const char* tooltipKey, const char* tooltipFallback);

    // Overload — accept pre-translated UTF-8 (e.g. dynamic strings with sprintf args).
    void RenderText(const char* fullText);
}
```

Rendering rules inside `Render`:
1. `ImGui::PushStyleColor(ImGuiCol_Text, GetStyleColorVec4(ImGuiCol_TextDisabled))`
2. Glyph: re-check `SmatchetAreFaIconsLoaded()` **per call** (atlas can rebuild on font/locale reload) — `ICON_FA_CIRCLE_QUESTION` if loaded, else literal `"(?)"`
3. `ImGui::TextUnformatted(glyph)`
4. `PopStyleColor`
5. On `IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)` → `BeginTooltip` + `PushTextWrapPos(ImGui::GetFontSize() * 35.0f)` + `TextUnformatted(fullText)` + `PopTextWrapPos` + `EndTooltip`. `AllowWhenDisabled` is **required** — `SmatchetPreferencesUi_Assistant.cpp:374` wraps target sites in `BeginDisabled(true)`; without the flag the tooltip is swallowed.

### Refactor pattern at each callsite

Before:
```cpp
ImGui::TextWrapped("By default Smatchet sends your %s API key only to %s over HTTPS.", provider, host);
ImGui::TextWrapped("Enabling custom endpoints lets a proxy / gateway host (Azure OpenAI, LiteLLM, openrouter) ...");
```

After:
```cpp
SmatchetLocalizedImGui::Text("assistant.endpoint.short", "Key sent over HTTPS to %s.", host);
ImGui::SameLine();
// std::string for dynamic-text path — snprintf with a fixed buffer would silently truncate
// the longest combined cases (35w+28w + interpolated URL > 1 KB worst case).
std::string full = std::string("By default Smatchet sends your ") + provider
    + " API key only to " + host + " over HTTPS. "
    "Enabling custom endpoints lets a proxy / gateway host (Azure OpenAI, LiteLLM, openrouter) "
    "receive that key, and permits plain http:// to non-loopback hosts — sending the key in cleartext.";
SmatchetHelpMarker::RenderText(full.c_str());
```

Static-text sites use the `Render(key, fallback)` overload — no buffer concern there.

For tooltip-only sites (`SetItemTooltip` with a long body): shrink the tooltip to a one-line summary AND add a `SmatchetHelpMarker::RenderText(longBody)` after the control with the original full text — preserves the always-on hover for the field while making the full explanation a deliberate hover on the (?).

For multiline `\n`-embedded strings (e.g. `AnnotateAnalysisUi_Window.cpp:164`, `SmatchetPerfUi.cpp:259`): collapse the visible label to the first line, pass the whole multiline body unchanged to `RenderText` — `TextUnformatted` inside the tooltip preserves `\n`.

### Shortening rules (per string)

- Target ≤ 8 words, ≤ 60 chars
- Strip examples in parens — they belong in the tooltip
- Strip "When set / If empty / Off by default" clauses — keep in tooltip
- Keep technical nouns exact (`reasoning_effort`, `agents.md`, `X-Smatchet-Token`) — if a noun blows the 8w budget, keep the noun and let the line run to ≤80 chars instead of abbreviating
- Localization keys: new `<panel>.<field>.short` for the short label, existing or new `<panel>.<field>.help` for the long text — keys are introduced inline via `T("...", "english fallback")` (no JSON file)

### Files to modify

| File | Sites | Notes |
|---|---|---|
| `Source/Core/include/Ui/SmatchetHelpMarker.h` | new | helper header |
| `Source/Core/src/Ui/SmatchetHelpMarker.cpp` | new | helper impl (no CMake edit — top-level `GLOB_RECURSE` picks it up) |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp` | 7 sites | densest |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp` | 9 sites | densest |
| `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` | 7 sites | tracker tab |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp` | 2 sites | |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp` | 1 site | |
| `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp` | 7 sites | one is 35w multiline |
| `Source/Core/src/Ui/AnnotateAnalysisUi.cpp` | 1 site | |
| `Source/Core/src/Ui/SmatchetPerfUi.cpp` | 2 sites | one multiline |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` | 1 site | conflict modal |
| `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp` | 1 site | |
| `Source/Core/src/Ui/SmatchetBugReportUi.cpp` | 1 site | redaction tooltip |
| `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp` | 2 sites | MCP status |
| `Source/Core/src/Ui/SmatchetUI.cpp` | 1 site | update banner |
| `Source/Core/src/SmatchetLocalization.cpp` | ~80 entries | `{key, en, fr}` triplets for new short+help keys, per slice |

Localization: new `<panel>.<field>.{short,help}` keys appear inline at the `T(key, fallback)` callsites, **plus** a `{key, english, french}` entry per key in the translation table at `Source/Core/src/SmatchetLocalization.cpp` (only two locales exist: en-US + fr-FR — see `AvailableLanguages()` at L697). ~80 entries total (40 short + 40 help), added per slice alongside the callsites they serve. French follows the register of existing entries (e.g. `prefs.local_data.recreate_tooltip`).

### Slicing (one PR per slice; ≤ 15 file diff each to respect CR ceiling)

1. **Slice 0 — helper + stress-test demo**. Add `SmatchetHelpMarker.{h,cpp}` + demo on `SmatchetPreferencesUi_Assistant.cpp:474–480` (custom-endpoint security warning) — this site exercises both the longest-string path AND the `BeginDisabled(true)` block at L374, so the `AllowWhenDisabled` flag and the dynamic-text `std::string` path are validated up front. Commit the first 3–4 shortened strings here and pause for user thumbs-up on tone before bulk sweep.
2. **Slice 1a — Preferences/Assistant + Whisper**. Remaining Assistant sites + Whisper. ~8 sites, 2 files.
3. **Slice 1b — Preferences/Local + Tracker + Templates**. Storage / typography / MCP / templates cluster. ~18 sites, 3 files.
4. **Slice 2 — Annotate / Perf / Modals**. Annotate window, Perf panel, Offline conflict modal, Bug report, AI assistant, grid header, main UI banner. ~15 sites.

## Verification

- **Bucket A — lint/build** — `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (dup_audit + agent_size_audit + zone lints). `cmake --build --preset ninja-msvc` and the DX12 preset both clean. `pre-ship.sh` once per slice.
- **Bucket C — screenshot diff** per touched panel pre/post: `(?)` glyph present, short-label one-liner fits at default + 1.5× font scale, tooltip body wraps at ~35 ems, multiline preserved, hover delay matches existing tooltips. Visual-validation exception applies (visual change in `Smatchet*Ui*.cpp` — orchestrator pauses for user verdict per `ship-loops.md` if no bucket-E coverage exists for the panel).
- **Bucket E (resolved)** — keyboard-nav reachability of new tooltips: now covered by `tests/ui/help_marker_keyboard_focus.test.cpp` (ImGui Test Engine), added in the a11y follow-up (#1179).
- **Localization sanity** — grep `Source/Core/src/SmatchetLocalization.cpp` for any new key whose English column differs from the callsite fallback literal (duplicate-fallback drift). Switch language to fr-FR in Preferences and spot-check touched panels: short labels + tooltips render French, no `missing translation key` warnings in log for new keys.
- **`scripts/dev/test-all.sh`** — once at end of each slice (per process-rules: at most one full run per slice).
- **`BeginDisabled` regression check** — explicitly hover the (?) inside the Assistant custom-endpoint disabled group (Slice 0 demo) and confirm the tooltip fires. This is the canonical test of the `AllowWhenDisabled` flag.

## Implementation log

All four slices shipped together on one branch (`feat/help-marker`) per the PR-batching rule (one PR per logical feature), squash-merged as `9c0ef307` (#1124) — the per-slice "pause for tone thumbs-up" was waived by the user's "go autonomously until ready for visual approval". The keyboard-a11y regression noted below was filed as Issue #1128 and fixed in the follow-up squash-merge `4be10390` (#1179).

- **Slice 0** — `SmatchetHelpMarker.{h,cpp}` + Assistant custom-endpoint demo (dynamic `std::string` path, inside `BeginDisabled`). Glyph fallback checks `SmatchetAreFaIconsLoaded()` per call.
- **Slice 1a** — Assistant remaining sites (reasoning_effort / agents.md tooltips) + Whisper PTT.
- **Slice 1b** — Local (DB recreate intro, storage-mode paragraphs, typography/language/vsync/wheel/date tooltips), Tracker (read-only, inherit-token trio, GitHub repo), Integrations (MCP bind/token/lua), Templates (long-text modal, duration suggestions).
- **Slice 2** — Annotate window (4 sites) + Annotate config blurb, Perf CPU/Network intros, Offline-queue unknown-conflict pane, AI-assistant per-turn effort, Bug-report screenshot redaction, update banner.
- **Localization** — ~90 `{key, en, fr}` entries (`<panel>.<field>.{short,help}`). `.short` English columns byte-match callsite literals (localized-ImGui source lookup); `.help` bodies resolve by key. Drift check (29 `.short` entries grep-verified against `Source/Core/src/Ui/`) passed pre- and post-clang-format.
- **Follow-up (visual round 1, in squash-merge `9c0ef307`, #1124)** — tab-aware Preferences footer per user feedback. New `PreferencesActiveTab` enum + `UiDrawSession::preferencesActiveTab` (`SmatchetUiSession.h`); 9 `BeginTabItem`-true set-points across 5 Preferences TUs (nested Fields Inputs sub-tabs map to parent); footer switch in `drawPreferencesWindow` shows one of 5 per-tab save-semantics lines (`prefs.footer.*.short`) + shared `(?)` carrying the full cross-tab paragraph (`prefs.footer.save_sync.help`). 6 new `{key, en, fr}` entries. Works in both desktop and embedded (mobile Settings) footer paths.

- **Keyboard-a11y follow-up** — `4be10390` · keyboard-reachable (?) help-marker tooltips (Issue #1128, #1179): made the (?) marker focusable so keyboard-only users surface the long-form text; added bucket-E coverage `tests/ui/help_marker_keyboard_focus.test.cpp`.

## Deviations

- **Annotate TUs unlocalized** — `AnnotateAnalysisUi.cpp` + `AnnotateAnalysisUi_Window.cpp` lack the `#define ImGui SmatchetLocalizedImGui` TU pattern; markers there use plain literals via `RenderText`, no fr entries. Localizing those TUs is out of scope.
- **Button-action tooltips left unchanged** (Annotate: Ask AI / Show Table / Show Raw Text / Cancel) — already hover-only with zero visible footprint; adding markers to a right-aligned button row risks layout churn for no vertical-space win.
- **`SmatchetGridHeaderUi.cpp` skipped** — MCP chip texts are already tooltips and ≤12 words; below threshold.
- **Update banner shortened without marker** — only the word "standalone" dropped; remaining text ≤8 words, nothing left for a tooltip.
- **Consent/destructive/security texts kept visible verbatim** — endpoint-consent risk line, DB delete confirm modal + recreate button tooltip, Android plaintext-token warning. Informed-consent text must not hide behind a hover.
- **FA TTF configure-gate workaround** — `CMakeLists.txt` POST_BUILD copy of `fa-solid-900.ttf` is gated on configure-time `EXISTS`; the TTF was absent at configure so the copy never registered. Manually copied next to the exe + into `assets/fonts/` so any future reconfigure registers it.

## Verification (results)

- Bucket A: `test-lint-rules.sh --diff origin/develop` all PASS (3 dup WARNs = pre-existing include-block clone pattern, calibration phase; comment-ratio WARN on the 20-line helper header). Dual-target build (`ninja-iter-msvc`, SmatchetStandalone + SmatchetCore_DX12) exit 0.
- Localization drift check: 29 `.short` entries byte-match UI callsite literals — clean.
- Bucket C / visual-validation exception: standalone launched for user verdict (markers in Preferences tabs, Annotate, Perf, Offline conflict, Bug report, AI assistant; `AllowWhenDisabled` hover inside the Assistant disabled block; fr-FR spot check).
- `test-all.sh`: bats/agent-infra failures attributed to background-sandbox `/tmp` blocking (foreground spot-checks 14/14); `test-doctor.sh` 3/3 under `with-msvc-env.sh` (plain-shell FAIL = no `cl.exe` on PATH, environmental); whisper autosend/roundtrip script FAILs reproduce identically on develop — pre-existing local stdout/stderr-interleave parse bug in the scripts (scenario envelopes show `passed:true`), filed as a spawn-task, not branch-caused.
- Bucket E: resolved — the keyboard-only reachability regression (user-observable, Pillar 4) was filed as Issue #1128 and fixed via #1179 (squash `4be10390`), which added bucket-E coverage `tests/ui/help_marker_keyboard_focus.test.cpp` (hover/focus surface + `AllowWhenDisabled` contract). The originally-deferred coverage gap is now closed in-tree.

## Out of scope

- Rewriting log/CLI/MCP strings (this task is UI-only; AGENTS.md `LOG_*` rule untouched).
- Adding accessibility cues (Pillar 4 backlogged). The keyboard-only-access regression originally introduced here (long-form text became mouse-hover-only) was **filed as Issue #1128 and resolved** in the a11y follow-up #1179 (squash `4be10390`): the (?) marker is now keyboard-focusable, with bucket-E coverage `tests/ui/help_marker_keyboard_focus.test.cpp`.
- Refactoring strings inside Lua scripts or MCP tool descriptions.
