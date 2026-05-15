# BACKLOG — plans

Centralized tracker for plan-shaped work in this repo. Three buckets:

1. **Applied / archived plans** — design docs whose work shipped. Source under [`docs/design/applied/`](../design/applied/). Indexed below with approximate date and one-line summary.
2. **Deferred code items** — concrete C++ refactors / improvements explicitly waiting on either an unrelated PR (bundle-with-next) or an external upstream fix. Each entry is also tracked in [`AGENT_SELF_IMPROVEMENT.md`](./AGENT_SELF_IMPROVEMENT.md) where it originated; this file is the **code-focused view** so future C++ touches can scan a single page for "what should I bundle in this PR?"
3. **Agentic dependencies** — work that the agent ecosystem (prompts, harness, mirror, telemetry) needs in the codebase to unblock further automation.

Format: `- <slug> · <approx-date> · <one-line>`.

---

## 1. Applied / archived plans

These plans shipped. Files moved from `docs/design/*.md` (and a few from `backlog/*PLAN*.md` / `*REMAINING.md`) into `docs/design/applied/` to declutter the working set. Date column is the **first-commit date** of the canonical plan file under its original path.

| Plan (slug) | Approx. date | One-line summary |
|---|---|---|
| [`rich-text-editing-v2-remaining.md`](../design/applied/rich-text-editing-v2-remaining.md) | 2026-05-09 | Rich-text editing v2 backlog — golden / snapshot tests for `MarkdownToAdf` / `AdfToMarkdown` / `MarkdownToHtml` / `HtmlSubsetToMarkdown`; raw-mode + fidelity UX. Originally `backlog/RICH_TEXT_EDITING_V2_REMAINING.md`. |
| [`command-system-plan.md`](../design/applied/command-system-plan.md) | 2026-05-11 | Unified Command System (CLI + Palette + MCP + Lua + Scenarios). Originally `backlog/COMMAND_SYSTEM_PLAN.md`. C++ source comments throughout `Source_Core/src/Commands/` + `Target_Standalone/` reference this. |
| [`remove-global-project-key.md`](../design/applied/remove-global-project-key.md) | 2026-05-12 | Multi-project design — remove the singleton `TrackerConfig::ProjectKey` / Plane equivalent; resolve project per call site (view JQL, selection prefix, explicit picker). |
| [`vs-style-view-menu.md`](../design/applied/vs-style-view-menu.md) | 2026-05-12 | VS Code shell — classic menu bar, View menu around VS Code "Views" concept, embedded Command Palette input, locked docking. |
| [`lua-recorded-cmd-list.md`](../design/applied/lua-recorded-cmd-list.md) | 2026-05-14 | Lua recorded ImGui command list — cached cell + window bindings (PR #66 `5b740e9`). Replaces per-frame Lua dispatch with cached command replay (~390 µs/cell → ~5 µs/cell). |
| [`lua-recorded-cmd-list-v2.md`](../design/applied/lua-recorded-cmd-list-v2.md) | 2026-05-14 | Stub tracking v2 follow-ups (extended recorder vocabulary, chrome buttons, auto-dirty relaxation). None scoped to ship yet. |
| [`agent-ecosystem-gap-fill.md`](../design/applied/agent-ecosystem-gap-fill.md) | 2026-05-15 | Fill 8 patterns borrowed from Anthropic multi-agent / OpenAI Agents SDK / OpenHands / wshobson — parallel dispatch, session scratchpad, tool-trace, output-shape contract, trigger map, versioning, skeleton-first, telemetry. |
| [`imgui-test-engine-bucket-e.md`](../design/applied/imgui-test-engine-bucket-e.md) | 2026-05-15 | Scope-only plan for wiring ImGui Test Engine (`test-author` bucket E). Does not execute until the first concrete bucket-E item arrives. |
| [`open-backlog-sweep.md`](../design/applied/open-backlog-sweep.md) | 2026-05-15 | Triage of nine open `AGENT_SELF_IMPROVEMENT.md` entries — apply, defer, or scope. |
| [`test-rig-agent.md`](../design/applied/test-rig-agent.md) | 2026-05-15 | Add `test-rig` agent + CTest target for `Source_Core` pure-logic helpers using doctest. |

### Notes

- The repo's project rule (AGENTS.md § Plan location) says new plans go to `docs/design/<slug>.md`. Once a plan ships, **move it to `docs/design/applied/`** and add a row above. The active-plan working set should stay shallow.
- The kebab-case slug is canonical. Older `SCREAMING_CASE` filenames from `backlog/` were renamed on move.
- Approximate dates are first-commit dates of the canonical file under its original path. Most plans were authored over a single short window; "approximate" reflects that.

---

## 2. Deferred code items (C++ refactors / improvements)

Each item is already tracked in [`AGENT_SELF_IMPROVEMENT.md`](./AGENT_SELF_IMPROVEMENT.md). This list is the **code-focused view** — when you're about to touch a relevant area, scan here for bundling opportunities.

| Item | Where it bites | Bundle with |
|---|---|---|
| **`JiraClient.h` split** — `TrackerFieldValueParser.h` includes `JiraClient.h` which pulls `ITrackerClient.h` + `ConfigManager.h` + HTTP/cpr surfaces; blocks per-cpp testing of `ParseWorkDurationToSeconds` / `NormalizeTrackerFieldValue` / friends. Fix: make `TrackerFieldValueParser.h` depend on `TrackerFieldSchema.h` only. | `Source_Core/include/TrackerFieldValueParser.h`, `Source_Core/include/JiraClient.h`, downstream parser tests | Next PR that touches `TrackerFieldValueParser.*` or `JiraClient.h`. Source: AGENT_SELF_IMPROVEMENT.md `2026-05-15 · test-rig · context`. Candidate agent: `tracker-backend`. |
| **`OfflineCreateQueue::kMaxReplayAttempts` extraction** — replay-cap constants live in `LocalCacheManager.h` which `#include <SQLiteCpp/SQLiteCpp.h>`. Doctest rig bans SQLite includes. Fix: lift the cap-decision into a pure free function `bool ShouldArchiveAfterAttempt(int currentAttempts)` in its own `.cpp` with no SQLite dep. | `Source_Core/include/LocalCacheManager.h`, `Source_Core/src/OfflineQueueService.cpp`, offline-queue tests | Next PR that touches the offline-queue replay path. Source: AGENT_SELF_IMPROVEMENT.md `2026-05-15 · test-rig · context`. Candidate agent: `offline-sync`. |
| **`RemoteProject` casing drift** — `RemoteProject` POD uses `lowerCamelCase` (`id`, `key`, `displayName`) while most other `Source_Core/include/` DTOs use `PascalCase`. Style drift introduced in PR 1 of the project-key removal. Multi-file rename. | `Source_Core/include/JiraClient.h` (definition), `tracker-backend`, `grid-engine`, bulk-import call sites | Next PR that legitimately touches `RemoteProject`. Do NOT open a standalone rename PR — bundle to minimise diff noise. Source: AGENT_SELF_IMPROVEMENT.md `2026-05-12 · tracker-backend · context`. |
| **`SaveFieldCatalogSnapshot` arg-bundle** — accumulated 4 extra primitive args; bundling them into a `FieldCatalogSaveContext` struct keeps call sites narrow as more per-axis state lands. | `Source_Core/src/TrackerFieldCatalog.cpp` + callers | Next PR that adds a per-axis arg to `SaveFieldCatalogSnapshot`. The bundling decision only shows ROI in that context. Source: AGENT_SELF_IMPROVEMENT.md `2026-05-12 · offline-sync · shortcut`. |

### Adding to this list

When `docs/backlog/AGENT_SELF_IMPROVEMENT.md` gains a new entry whose category is **`shortcut` / `context` / `tooling`** AND the action is a concrete C++ refactor (not an agent-prompt edit), mirror a one-line row here. Agent-prompt-only entries live in `AGENT_SELF_IMPROVEMENT.md` exclusively.

---

## 3. Agentic dependencies (code-side)

Work the C++ codebase needs so the agent ecosystem can land further automation. Empty for now — fill in as agents identify pre-conditions they need from the codebase.

| Item | Blocks which agent / automation | Notes |
|---|---|---|
| *(none currently)* | | |

### Adding to this list

When an agent's `## Self-improvement` section identifies a codebase-side dependency (e.g. "no thread-safe handle for `GridModel` from the UI thread, so `perf-instrument` can't measure cross-thread cost"), capture it here so the next person touching the relevant subsystem can fix it.

---

## 4. Externally blocked (won't action — for reference)

| Item | Why deferred | Source |
|---|---|---|
| vexp `<!-- vexp -->` block autoregenerates inside `AGENTS.md` instead of `.claude/CLAUDE.md` | vexp tool source lives outside this repo. File upstream issue/PR. Workaround: leave block alone; ~250 input tokens/session is small vs autoregen friction. | AGENT_SELF_IMPROVEMENT.md `2026-05-13 · orchestrator · process` |
| `mcp__vexp__run_pipeline` rejects `max_tokens` as float | vexp tool source. Workaround: pass int literal. | AGENT_SELF_IMPROVEMENT.md `2026-05-12 · tracker-backend · tooling` |
| ImGui Test Engine bucket E | Wait for first plan whose §Verification contains a click/drag/type that no scenario can drive. Plan scoped at `docs/design/applied/imgui-test-engine-bucket-e.md`. | AGENT_SELF_IMPROVEMENT.md `2026-05-13 · test-author · new-agent / tooling` |
