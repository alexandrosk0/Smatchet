# Plan — AppController cluster extraction (follow-up slices)

> **Slug**: `appcontroller-clusters-followup` (matches this file's basename without `.md`).
>
> **Status**: `active` — slice 1 (MCP client-activity) shipped (PR #1660); slice 2 (Lua-script-file handling) shipped (PR #1742, squash `1461bee3`); slice 3 (AI-context) shipped (PR #1743, squash `2c580c79`); slice 4 (host-integration) in flight.

<!-- index-summary: Continuation of the shipped appcontroller-service-extraction plan — behavior-preserving extraction of the remaining cohesive AppController.cpp clusters (flagged there as § Out of scope) into focused companion TUs, toward the ≤ ~800 LOC target. -->

## Context

[`docs/plans/appcontroller-service-extraction.md`](../appcontroller-service-extraction.md)
shipped two slices (bootstrap → `AppController_Init.cpp`, pane-context →
`AppController_PaneContexts.cpp`) via PR #1653, dropping `AppController.cpp` **2862 → 1518
LOC**, then closed at its 2-PR cap. It flagged the **remaining clusters as § Out of scope
(not designed)**: further `AppController.cpp` clusters (MCP/activity plumbing,
Lua-script-file handling, AI-context), plus a separate `AppController.h` split. This plan
carries those forward, one cohesive slice per PR, toward the shipped plan's ≤ ~800 LOC target.

**Hard environment constraint (unchanged)**: `AppController.cpp` is in `CORE_SOURCES`, which
transitively needs `cpr`/`curl`; the tarball fetch is blocked by session egress policy, so
`posix-core-check` cannot configure and **no Core TU compiles in this container**. **CI
(Windows+MSVC full build + ctest + bucket UI lanes) is the sole correctness gate** for every
slice. Approach is therefore identical to the shipped plan: byte-identical body moves,
declarations stay in `AppController.h`, curated includes, one cohesive cluster per PR.

## Approach

Each slice moves a contiguous, cohesive block of `AppController::` method *definitions* out
of `AppController.cpp` into a new `AppController_<Area>.cpp` companion TU. Declarations stay
in `AppController.h` untouched — callers, linkage, and behavior are identical; only which
`.o` a symbol lands in changes. New TUs auto-join the DX12/Standalone build via the existing
`file(GLOB_RECURSE CORE_SOURCES …)` (Source/Core/CMakeLists.txt); the `tests/` target list is checked
per-slice (no test references the moved symbols → no test edit). Any file-local
(anon-namespace) helper used *exclusively* by the moved cluster moves with it, verified by
whole-tree grep before the cut.

## Slices

- **Slice 1 — MCP client-activity plumbing (SHIPPED, PR #1660).** Extract the whole
  `#if defined(SMATCHET_WITH_MCP)` cluster — the anon `PrefixMcpActivityLine` helper plus
  `AppendMcpActivity` / `CopyMcpActivityLog` / `NotifyMcpClientHttpActivity` /
  `GetMcpHttpTrafficEpoch` / `TryGetMcpLastClientHttpActivity` — into
  `AppController_McpActivity.cpp`. Anon helper confirmed cluster-private (2 grep hits, both
  moved). `AppController.cpp` 1519 → 1427 LOC. No CMake/test edit.

- **Slice 2 — Lua-script-file handling (SHIPPED, PR #1742).** Extract `ResolveLuaScriptPath` /
  `ListLuaScriptFiles` / `GetAutomationScriptContent` / `SaveAutomationScriptContent` into
  `AppController_LuaScriptFiles.cpp`. The four are topically cohesive but not contiguous
  (two around former lines 1007–1109, two around 1188–1230, with the field-icon path
  resolver between — that stays). Helper census: the bodies reference no anon-namespace or
  static file-local helper, so nothing else moves; the cluster is compiled unconditionally
  (not Lua-gated — matches the pre-move layout and the `AppController_LuaStubs.cpp` note).
  `AppController.cpp` 1442 → 1294 LOC. No CMake/test edit.

- **Slice 3 — AI-context cluster (SHIPPED, PR #1743, squash `2c580c79`).** Extract `AddAiContext` / `ClearAiContext` /
  `GetAiContext` / `PromptAi` into `AppController_AiContext.cpp`. The four are contiguous
  and always-on (unconditional signatures; each body internally guards its
  `impl_->aiAssistant_` delegation with `SMATCHET_WITH_AI` and no-ops otherwise —
  reproduced byte-identically, including the `#else (void)param;` tails, so the light
  build compiles the same bodies with the guarded branches dropped). Helper census: the
  bodies reference no anon-namespace or static file-local helper (only Impl's
  `aiAssistant_`, `GetGlobalAiAssistantUiState()`, and `LOG_WARN`), so nothing else
  moves. Unlike slice 2 this TU dereferences the pImpl, so it includes
  `AppControllerImpl.h`. `AppController.cpp` 1297 → 1237 LOC. No CMake/test edit.

- **Slice 4 — host-integration cluster (this PR).** Extract the contiguous former-line
  693–865 span — the host-callback setters an embedding shell registers plus the actions
  that invoke them (embedded-UI close, app-quit request, URL open, automation sink
  registration, scripting-window request consume, attachment handler setters, and the
  file-open-dialog request) — into `AppController_HostIntegration.cpp`. Platform gating:
  the URL-open body carries the pre-existing `_WIN32` / `__APPLE__` / else branches
  (system shell-open vs no-shell launcher), reproduced byte-identically along with the
  platform include block; no winsock preamble (the TU pulls no cpr/curl header). Helper
  census: the file-local no-shell launch helper is used only by the URL-open fallback,
  so it moves with the cluster; everything else the bodies touch is header-provided
  (host-callback members, log truncation). The automation sink delegators dereference
  the pImpl, so the TU includes `AppControllerImpl.h` and completes the Lua automation
  host unconditionally (the slice-3 lesson). `AppController.cpp` 1237 → 1023 LOC. No
  CMake/test edit. Remaining candidate clusters (not designed here): the ticket-prefetch
  block, the field-icon path resolver plus its two file-local helpers, and the
  local-cache database block with its db-file removal helper.

## Verification

- **Build gate (THE real verification here)**: CI Windows+MSVC full build (DX12 + Standalone +
  light) + ctest + bucket UI lanes must be green. Local compile is impossible in this container
  (see § Context). § Verification (actual) is populated post-ship per slice.
- Pre-push: `test-lint-rules.sh --diff origin/develop` (strict-zone / comment-noise) and
  `dup_audit.py --diff` clean; behavior-preserving move → `tests-out-of-band` (no runtime
  surface to add an assertion against).

## Out of scope

- **`AppController.h` (1465 LOC) split** — separate follow-up plan; not clustered here.
- **`GridContextDepsAdapter` friend-drop** — DEFERRED per the shipped plan (wide ownership /
  lifetime change this container cannot validate).

## Implementation log

- Slice 1 — PR #1660 (`17ea3235`) · MCP client-activity → `AppController_McpActivity.cpp`;
  `AppController.cpp` 1519 → 1427 LOC.
- Slice 2 — PR #1742 (`1461bee3`) · Lua-script-file handling → `AppController_LuaScriptFiles.cpp`;
  `AppController.cpp` 1442 → 1294 LOC.
- Slice 3 — PR #1743 (`2c580c79`) · AI-context cluster → `AppController_AiContext.cpp`;
  `AppController.cpp` 1297 → 1237 LOC.
- Slice 4 — PR #1749 · host-integration cluster → `AppController_HostIntegration.cpp`;
  `AppController.cpp` 1237 → 1023 LOC.
