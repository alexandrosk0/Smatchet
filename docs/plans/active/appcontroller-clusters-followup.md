# Plan — AppController cluster extraction (follow-up slices)

> **Slug**: `appcontroller-clusters-followup` (matches this file's basename without `.md`).
>
> **Status**: `active` — slice 1 (MCP client-activity) in flight.

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
`file(GLOB_RECURSE CORE_SOURCES …)` (CMakeLists.txt:1127); the `tests/` target list is checked
per-slice (no test references the moved symbols → no test edit). Any file-local
(anon-namespace) helper used *exclusively* by the moved cluster moves with it, verified by
whole-tree grep before the cut.

## Slices

- **Slice 1 — MCP client-activity plumbing (this PR).** Extract the whole
  `#if defined(SMATCHET_WITH_MCP)` cluster — the anon `PrefixMcpActivityLine` helper plus
  `AppendMcpActivity` / `CopyMcpActivityLog` / `NotifyMcpClientHttpActivity` /
  `GetMcpHttpTrafficEpoch` / `TryGetMcpLastClientHttpActivity` — into
  `AppController_McpActivity.cpp`. Anon helper confirmed cluster-private (2 grep hits, both
  moved). `AppController.cpp` 1519 → 1427 LOC. No CMake/test edit.

- **Slice 2+ (future) — remaining clusters.** Lua-script-file handling
  (`ResolveLuaScriptPath` / `ListLuaScriptFiles` / `GetAutomationScriptContent` /
  `SaveAutomationScriptContent`) and the AI-context cluster (`AddAiContext` / `ClearAiContext` /
  `GetAiContext` / `PromptAi`). Sized per-slice; not designed until slice 1 lands.

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

- (pending) Slice 1 — `<sha>` · MCP client-activity → `AppController_McpActivity.cpp`.
