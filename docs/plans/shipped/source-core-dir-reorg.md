# Plan — Source_Core directory reorganization (precursor to high-integrity-cpp-enforcement)
<!-- plan-date: 2026-05-28 -->

> **Slug**: `source-core-dir-reorg` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`docs/plans/shipped/high-integrity-cpp-enforcement.md`](high-integrity-cpp-enforcement.md) § Dependency declares itself **blocked** on this reorg. That plan's tiered-enforcement design keys the strict/light/exempt lint zones off **directory globs** (`Source_Core/src/Tracker/**`, `/Sync/**`, `/Persistence/**`, `/Config/**`, `Plugins/Mcp/src/**`). Audit (2026-05-28) found those directories **do not exist**: `Source_Core/src/` is flat — 154 `.cpp` files at the top level organized by filename prefix, with only `Commands/` as a real subdirectory; `Source_Core/include/` is likewise flat (146 `.h`, only `Commands/`); `Plugins/Mcp/` has no `src/`. The enforcement plan considered fragile filename-prefix globs and **deliberately rejected them** in favour of this mechanical reorg so the directory globs become literally valid.

Intended outcome — after this lands: `Source_Core/src/` (and, per the header decision below, `Source_Core/include/`) and `Plugins/Mcp/` are organized into the subsystem directories the enforcement plan's strict/light zone globs name, the dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) is green, and the Unreal package build still resolves all includes. The enforcement PR is then unblocked.

This is a **mechanical, no-logic-change** reorg: file moves + CMake source-list / include-dir updates (+ optionally `#include` path fixups, depending on the header decision). Zero behaviour change is the success criterion.

## Approach

**Move `.cpp` + `.h` together into subsystem subdirectories, and add the new subdirectories to the CMake include path — do NOT rewrite `#include` directives.** This is the key insight from the CMake audit and is much cheaper than the enforcement plan's hand-waved "`#include` path fixups across the tree."

Why no include rewrites are needed: includes are bare (`#include "JiraClient.h"`), resolved against include-path **roots** — `SmatchetCoreInterface` adds `Source_Core/include` (INTERFACE, `CMakeLists.txt:719`) and each core-impl target adds `Source_Core/src` (PRIVATE, `CMakeLists.txt:807`). A bare include resolves anywhere on the path. If `JiraClient.h` moves to `Source_Core/include/Tracker/`, `#include "JiraClient.h"` keeps resolving **iff** `Source_Core/include/Tracker` is also on the path. So the reorg adds ~10 subdirectory include entries instead of touching hundreds of `#include` lines across Source_Core (and the Unreal plugin). Trade named: a flat include namespace persists (you can still `#include "JiraClient.h"` from anywhere) — we accept that because the goal is lint **zones**, not include hygiene, and rewriting every include is a far larger blast radius that would also churn the Unreal-side consumer.

The standalone source list uses `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source_Core/src/*.cpp")` (`CMakeLists.txt:606`), so moved `.cpp` files are picked up automatically. The cost concentrates in **hard-coded path references** that `GLOB_RECURSE` does not cover: the ~29-entry explicit DX12 core source list (`CMakeLists.txt:684+`) and the `list(REMOVE_ITEM …)` / `list(APPEND …)` special-cases for Lua / Whisper / ImGuiHost / CppSyntax (`CMakeLists.txt:607-678`). Every moved file referenced there must have its path updated, or the DX12 build silently drops/duplicates a TU (a `REMOVE_ITEM` whose path no longer matches is a silent no-op).

**Open decision (see § Risks — escalated to the user before implementation): headers move or stay flat.** The recommended approach above moves headers too (full zone coverage incl. header-resident `#define ImGui` / `new` / `printf`). The cheaper-but-narrower alternative leaves headers flat in `include/` and moves only `.cpp` — UE-safe (no packaged-include path change) but the strict zone then covers `.cpp` only.

## Files to modify

1. **File moves** — `git mv` each flat file into its taxonomy directory (table below). `.cpp` under `Source_Core/src/<Dir>/`; `.h` under `Source_Core/include/<Dir>/` (if header-move chosen) or co-located private `.h` under `Source_Core/src/<Dir>/`. MCP: `Plugins/Mcp/*.{cpp,h}` → `Plugins/Mcp/src/`.
2. `CMakeLists.txt` — (a) add new `src`/`include` subdirs to the include path (`SmatchetCoreInterface` INTERFACE block ~`:719` for `include/<Dir>`; the per-target PRIVATE block ~`:807` for `src/<Dir>`; the MCP block ~`:810` for `Plugins/Mcp/src`); (b) update every hard-coded path in the explicit DX12 list (`:684+`) and the `REMOVE_ITEM`/`APPEND` special-cases (`:607-678`) to the new locations. `GLOB_RECURSE` (`:606`) needs no change.
3. `UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs` — **only if headers move**: the plugin adds a single packaged-include root `PublicIncludePaths.Add(.../Smatchet/include)` (`:89-90`); add the new subdir include paths so bare includes resolve in the UE package build. The recursive source glob (`:239`) is for the mtime staleness check and needs no change. **This file is NOT exercised by the CMake dual-target gate** — see § Verification.
4. Any `.h` whose path is referenced by a packaging manifest / copy step (`SmatchetPackageUnrealLibs_DX12`) — verify the packaging copies the new tree structure intact.

**Target directory taxonomy** (from `high-integrity-cpp-enforcement.md` § Dependency; exact per-file placement of edge cases settled in review):

| New dir | File families | Zone |
|---|---|---|
| `Source_Core/src/Tracker/` (+ `include/Tracker/`) | `Tracker*`, `Jira*`, `Plane*`, `GitHub*`, `Jql*`, `Issue*`, `IssueDraft`, `IssueTableSerializer`, `LabelEdit*`, `ProjectResolver`, `DefaultTrackerBackendFactory`, `FieldCatalog*` | strict |
| `Source_Core/src/Sync/` | `OfflineQueueService`, `TicketSyncService`, `NetworkUsageTracker` | strict |
| `Source_Core/src/Persistence/` | `LocalCacheManager`, `BackendAuditTrail`, `FieldEditAuditSource`, `*Cache` | strict |
| `Source_Core/src/Config/` | `ConfigManager*` | strict |
| `Source_Core/src/Commands/` | (already exists) | strict |
| `Plugins/Mcp/src/` | `McpJsonRpc*`, `McpPlugin*` | strict |
| `Source_Core/src/Ui/` | `Smatchet*Ui*`, `*Ui*`, `SmatchetTheme`, `SmatchetToast`, `Blame*`, `CodeColorView`, `Markdown*`, `CppSyntaxHighlight` | light |
| `Target_Standalone/` | (already exists) | light |
| residual `Source_Core/src/` root (+ new `Ai/`, `Core/` if useful) | everything else | exempt-by-default |

## Existing utilities reused

- `git mv` — preserves blame/history across the move (vs delete+add). One `git mv` per file; batchable per directory.
- `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS …)` `CMakeLists.txt:606` — auto-enumerates moved standalone `.cpp`; no edit.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — the dual-target gate that proves no TU was dropped/duplicated and no include broke.
- `scripts/dev/is-pure-docs-diff.sh` — confirms this is NOT a pure-docs diff (forces the build gate to run).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — pure file moves, zero logic change; binary output is byte-equivalent modulo `__FILE__` strings. Perf-neutral by construction; validated not asserted (see § Perf-review-system gates).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no code paths added or moved across threads.
- **Pillar 3 (never crash)**: no impact directly; **enables** the enforcement plan that improves Pillar 3 (narrowing/define-imgui gates).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

This diff moves `Source_Core/` files wholesale, so the gate **applies** — but every gate is N/A-by-content because there is no logic change:

1. **PR-fast CI** — N/A by content (no algorithmic change). Run one representative scenario (`priority-grid-scroll`) post-move to confirm perf-neutrality rather than assert it.
2. **Pillar 2 static scanner** — N/A — no new sync-I/O; `pillar2-scan.sh` over the moved tree must stay silent (file-move can't introduce a UI-thread block).
3. **Dispatcher drain** — N/A — `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A — no new sync-stall path.
5. **Marker inventory** — N/A — no `SMATCHET_UI_PERF_SCOPE` markers added; existing markers move with their TU (string scope-names unchanged).

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline against `priority-grid-scroll` once post-move; expect within-noise.

**Override**: not needed — no intentional regression.

## Risks / non-goals

- **Open decision — headers move vs stay flat (escalated to user).** Moving headers = full strict-zone coverage (header-resident `#define ImGui`, raw `new`, `printf` are caught) but introduces a UE-side `PublicIncludePaths` change (Files §3) validated only by an actual Unreal package build, which the CMake dual-target gate does NOT exercise — a real "green-in-CMake, broken-in-UE" hazard. Cpp-only move = UE-safe, smaller diff, but strict zone covers `.cpp` only. **Recommendation: move both** (full coverage is the point of the enforcement plan) **and add the UE package build as an explicit manual verification step**. Decision must be locked before implementation.
- **Silent TU drop/duplicate in the DX12 list.** A `list(REMOVE_ITEM …)` whose hard-coded path no longer matches after a move becomes a no-op → the special-cased TU (e.g. a Lua-stub or Whisper file) is compiled on the wrong feature-flag path or twice. Mitigation: after the move, diff the configured DX12 source list against the standalone list and assert the special-case partition is preserved; the dual-target build link step catches duplicate-symbol / missing-symbol regressions.
- **Large blast-radius diff is hard to review.** ~300 file moves (cpp+h) is a wall of `git mv`. Mitigation: land as its own PR (this plan), commit-per-directory so each move batch is independently reviewable, and rely on the no-logic-change invariant (`git show --stat` shows pure renames; `git diff -M` shows ~0 content change).
- **`git mv` rename detection vs content edit.** If any file needs a content edit (e.g. a co-located private `.h` that hard-codes its own path in a comment), keep it in a separate follow-up commit so the move commits stay pure renames for clean review.
- **Non-goal**: `#include` hygiene / qualified-path includes (`#include "Tracker/JiraClient.h"`). Explicitly rejected — see § Approach. A future include-hygiene pass is a separate evidence-driven plan.
- **Non-goal**: splitting/merging TUs, renaming files, or any namespace change. Moves only.
- **Non-goal**: the enforcement scanner itself — that's `high-integrity-cpp-enforcement.md`, unblocked once this lands.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `cmake --preset ninja-test-msvc && cmake --build --preset ninja-test-msvc && ctest` — all existing tests pass unchanged (no logic moved). The test target also GLOB-enumerates, so moves are picked up.
- **Bucket E (ImGui Test Engine)**: N/A — no UI behaviour change; existing bucket-E scenario must still pass as a regression check, not a new test.
- **Bash-driver scenario / screenshot / sanitizer**: run `priority-grid-scroll` perf scenario once for perf-neutrality (§ Perf gates). Existing `scripts/dev/test-*.sh` lints must stay green over the moved tree.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — the primary correctness gate; proves no dropped/duplicated TU and no broken include in both worlds.
- **Manual residue — UE package build (unavoidable, if headers move).** The CMake `SmatchetCore_DX12` target uses the same `SmatchetCoreInterface` include dirs, so it validates CMake-side. It does **not** run the Unreal `SmatchetImGuiPlugin.Build.cs`, which consumes a *packaged* Source_Core via its own `PublicIncludePaths`. A real UE package build (`SmatchetPackageUnrealLibs_DX12` + plugin compile) is required to confirm bare includes still resolve UE-side. This is genuine manual residue; deferred-automation plan: add a UE-package smoke-build to CI is tracked as a `docs/self-improvement/categories/tooling.md` candidate (no headless UE runner today). If cpp-only move is chosen, this residue disappears.

## Out of scope (flagged, not designed)

- The high-integrity-cpp enforcement scanner, AGENTS.md tiered-enforcement subsection, baseline file, bats fixtures — all in `high-integrity-cpp-enforcement.md`, which this unblocks.
- A CI job that runs the Unreal package build — backlog candidate (no headless UE runner today); for now the UE build is a one-time manual gate on this PR.
- Renaming `Source_Core/include/` headers to match `.cpp` co-location patterns beyond the taxonomy — not needed for zone validity.

## Implementation log

- `ae2d522` · wip(plan): source-core-dir-reorg plan-doc.
- `22495f08` · #505 (2026-05-28) · reorg: 183 Source_Core + 4 MCP files `git mv`'d into subsystem dirs (196 files, almost all pure renames); CMake source paths + include dirs; Build.cs PublicIncludePaths; test-CMake path + include-dir fixes. Unblocked the downstream `high-integrity-cpp-enforcement` gate (#507, same day).

## Deviations from plan

- **Cheaper than the merged HICE plan implied — zero `#include` rewrites.** Includes are bare and resolve against path roots, so moving cpp+h together + adding the new subdirs to the include path sufficed. No "#include path fixups across the tree."
- **Classifier substring false-positives.** A case-insensitive `*ui*` net wrongly matched `AiContextB`**`ui`**`lder` and `SmatchetLocalizedImGui.h`; both moved back to root. Lesson for any re-run: match the CamelCase word `Ui`, not the raw substring.
- **Private headers co-located late.** 6 `*_Internal.h` / `*_detail.h` headers in `src/` (no matching `.cpp`) were missed by the initial header-only sweep (which scanned `include/` only) and moved in a follow-up step into their cpp's subdir (`ConfigManager_Internal.h`→Config, `PlaneClient_Internal.h`→Tracker, 4 Ui ones).
- **Transitive cross-subsystem includes ⇒ every target needs ALL subdirs.** Moved headers include each other bare across subsystems (`ConfigManager.h`→`SmatchetThemeIds.h`, `ILuaBindingHost.h`→`TrackerFieldSchema.h`), so each include-dir site (core-impl function, `SmatchetTests`, `SmatchetLuaTests`) carries the full 5-subdir set, not just the subdirs of files it directly compiles. **This is the standing fragility of subdir-based zones** — flagged for the enforcement-plan handoff.
- **Ui/ROOT boundary kept inclusive, not perfected.** Light zone == exempt for the gate, so borderline Ui-vs-root placements have zero enforcement consequence; not bikeshedded. Final Ui bucket: 52 cpp.
- **UE packaging consumes a FLAT curated header set, not the whole tree** (corrects the plan's § Approach assumption). `CMakeLists.txt`'s `SmatchetPackageUnrealLibs_DX12` copies only `SmatchetImGuiHost.h` / `SmatchetImGuiHostC.h` / `SmatchetDefaults.h` (+ imgui) into a flat `ThirdParty/Smatchet/include/`, and the plugin only `#include`s that set. Consequences: (1) the two host-header copy **source** paths were stale (moved to `Ui/`) and fixed — invisible to the `ninja-iter-msvc` gate because the packaging target is `EXCLUDE_FROM_ALL`; (2) the initial `Build.cs` `PublicIncludePaths` subdir additions were **reverted** — pointless against a flat packaged dir. So the reorg's true UE surface was 2 stale copy paths, not an include-path expansion.

## Verification (actual)

- **Build gate (dual-target)**: `SmatchetStandalone` + `SmatchetCore_DX12` via `ninja-iter-msvc` — **PASS** (validated twice, incl. a regression run after the private-header co-location).
- **Bucket A (ctest)**: `ninja-test-msvc` build + `ctest` — **PASS** (100%, 2/2 test executables, 2.47s). Required iterative include-dir fixes: test source-paths, the full 5-subdir set on both `SmatchetTests` + `SmatchetLuaTests` (transitive cross-subsystem includes), and co-locating 6 private headers.
- **FetchContent**: fresh worktree lacked the dep cache; configured against the main repo's `.fetchcontent-src` via `-DFETCHCONTENT_BASE_DIR` (the submodule-download failure was environmental, not the reorg).
- **Manual residue (Unreal package build)**: NOT run here — the CMake `SmatchetCore_DX12` gate does not exercise `SmatchetImGuiPlugin.Build.cs`. A real UE package build is required to confirm the new `PublicIncludePaths` subdirs resolve bare includes UE-side. Backlog candidate (no headless UE runner).
