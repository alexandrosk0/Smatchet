# Plan — consolidate all C++ under a single `Source/` root
<!-- plan-date: 2026-05-29 -->

> **Slug**: `source-root-consolidation` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Status**: plan only — not executed (user chose "Plan doc only now"). Execute as **one atomic PR** (grill Q2) on a fresh branch off `develop` (suggested `plan/source-root-consolidation`); the current branch `plan/separate-agents-repo` is unrelated WIP.
>
> **Decision record**: [`docs/adr/0011-single-source-root.md`](../../adr/0011-single-source-root.md) captures the durable why (shape, rejected alternatives). Grill decisions folded inline: Q1 scope (Core+Standalone+Plugins+UnrealPlugins; `tests/` root stays, `tests/Source_Core`→`tests/Core`), Q2 one atomic PR, Q3 ADR 0011, subdir-per-component naming.
>
> **In-flight dependency**: coordinates with [`agentic-layer-project-independence`](agentic-layer-project-independence.md) (Phases A–D shipped #542–545; E + F pending) — see § Coordination. [`project.config.json`](../../../project.config.json) is now a canonical file-to-modify; recommend landing this reorg **after** agentic Phase F.

## Context

Today the repo has **four** top-level C++ roots — `Source_Core/`, `Target_Standalone/`, `Plugins/`, `UnrealPlugins/` — plus test C++ under `tests/`. The user wants all product C++ consolidated under a single `Source/` root for navigability. This is a **mechanical, no-logic-change** reorg: directory moves + build/config/doc path updates. Zero behaviour change is the success criterion.

Not to be confused with the already-landed [`source-core-dir-reorg`](source-core-dir-reorg.md), which created subsystem subdirs *inside* `Source_Core/src/` (Tracker/, Sync/, …). That work is done; this plan moves the component roots one level up under `Source/` and keeps each component's internal layout intact.

**Scope decided with user** — move four roots, leave `tests/` at repo root, subdir-per-component naming:

| From | To |
|---|---|
| `Source_Core/` (keeps `src/`, `include/`, `ThirdParty/`) | `Source/Core/` |
| `Target_Standalone/` | `Source/Standalone/` |
| `Plugins/` (`Mcp`, `LuaConsole`, `Whisper`) | `Source/Plugins/` |
| `UnrealPlugins/` (`SmatchetImGuiPlugin`) | `Source/UnrealPlugins/` |
| `tests/` (root) | **stays at repo root** (heavily wired into coverage/CI/lint tooling; conventionally repo-root) |
| `tests/Source_Core/` (test mirror subdir) | `tests/Core/` (decided in grill — mirror the product `Source/Core` name; the rest of `tests/` keeps its names) |

**Intended outcome** — after this lands: all four C++ components live under `Source/`, the dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) is green, the test rig builds, the high-integrity lint gate still enforces (new paths), and the Unreal package build still resolves all includes.

## Coordination with in-flight `agentic-layer-project-independence`

[`agentic-layer-project-independence`](agentic-layer-project-independence.md) (Phases A–D shipped — #542 config seam, #543 agent split `agents/{core,project}/`, #544 self-improvement split, #545 `docs/design`→`docs/plans/{active,shipped}`; **E + F pending**) introduced [`project.config.json`](../../../project.config.json) — the canonical value table for the literals/zones/globs this reorg mutates. Two hard couplings:

1. **`project.config.json` is now a first-class file to modify** (§ Files to modify #4b). Its `project.literals` (`"Source_Core"`, `"Target_Standalone"`, `"UnrealPlugins"`), `lint.zones.strict/light` (`Source_Core/src/Tracker/` …), and `ci.path_filters.code_globs` (`["Source_Core/**","Plugins/**","Target_Standalone/**"]`) name the moved dirs directly. `build.targets` (`SmatchetStandalone`/`SmatchetCore_DX12`) and `visual_validation.trigger_globs` (`**/`-anchored) are **unaffected**.
2. **Phase F adds `test-portable-purity.sh`** (not yet on disk) — it forbids the `project.literals` denylist inside portable files (`agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/`). This reorg edits portable `agents/core/` files that name those literals (git-janitor blacklist regex, test-rig charter, coderabbit-triage, mechanic, debug-detective, perf-*, …). So the reorg **must update `project.literals` in the same PR** (old strings gone, new ones listed) or the guard's denylist goes stale and silently passes.

**Ordering — land this reorg AFTER agentic Phase F.** F wires project-only scripts to read the config and (per its done-bar) config-drives portable-file references including git-janitor's blacklist. Post-F this reorg shrinks to: the `git mv`s + `CMakeLists.txt`/`CMakePresets.json` (which the agentic plan never touches) + one `project.config.json` value edit + the residual project-only script re-roots — far fewer scattered hardcodes, and the purity guard won't fight the move. **If forced before F**: dual-update `project.config.json` *and* every still-inline hardcode (`test-lint-rules.sh` `STRICT_GLOBS`, CI `paths:`, `CODEOWNERS`, `.coderabbit.yaml`, git-janitor regex), and hand Phase F the new literal values so its generated denylist is correct. **Either way — do not run concurrently with Phase E/F**: both edit `AGENTS.md` (§ Layout / delegation tables), CI `paths:` filters, and `scripts/dev/*` → merge conflicts.

## Approach

**Move whole directories with `git mv` (preserves blame), then update the build's path *roots* — do NOT rewrite `#include` directives.** Includes are bare (`#include "JiraClient.h"`) and resolve against include-path roots set in CMake (`SmatchetCoreInterface` adds `Source_Core/include` + subdirs at [CMakeLists.txt:719](../../../CMakeLists.txt); each core-impl target adds `Source_Core/src` + subdirs at [CMakeLists.txt:816](../../../CMakeLists.txt)). Re-root those entries to `Source/Core/…` and every bare include keeps resolving. Same insight that made `source-core-dir-reorg` cheap; it carries to the cross-component move because no component includes another by path prefix — only by bare name or via CMake include dirs.

**Exactly one source file needs an edit**: [`Source/Core/src/Logger.cpp`](../../../Source_Core/src/Logger.cpp) (post-move path) contains the repo's only cross-tree *relative* include — `#include "../../tests/_debug/SmatchetAgentDebug.h"`. Moving `Source_Core/src/` → `Source/Core/src/` adds one directory level, so the `../../` (2 hops to repo root) must become `../../../` (3 hops). Everything else is CMake/config/doc text.

**The work concentrates in hard-coded path references** the `GLOB_RECURSE` does not cover: the root `CMakeLists.txt` explicit DX12/AI source lists and `REMOVE_ITEM`/`APPEND` special-cases, `CMakePresets.json`, the `tests/` CMake files (which reference moved dirs by absolute `${CMAKE_SOURCE_DIR}/Source_Core/…`), the lint/gate scripts, `.coderabbit.yaml`, CI workflows, the Unreal `Build.cs` (its repo-root walk gains a level), and the canonical docs. Trade named: a flat include namespace persists (you can still `#include "JiraClient.h"` from anywhere) — accepted, same as the precursor reorg.

## Files to modify

Grouped by subsystem. Representative anchors given; the move touches ~30 files + ~4 directory trees.

### 1. Directory moves (`git mv`)
1. `mkdir Source` then `git mv Source_Core Source/Core`, `git mv Target_Standalone Source/Standalone`, `git mv Plugins Source/Plugins`, `git mv UnrealPlugins Source/UnrealPlugins`. (`git mv` errors if the destination parent doesn't exist — create `Source/` first.) `Plugins/Whisper/CMakeLists.txt` moves with its tree; its internal paths are `${CMAKE_CURRENT_SOURCE_DIR}`-relative, so no internal edits. **Also** `git mv tests/Source_Core tests/Core` (test mirror; rest of `tests/` unchanged).

### 2. The one source edit
2. [Source_Core/src/Logger.cpp](../../../Source_Core/src/Logger.cpp) — `"../../tests/_debug/SmatchetAgentDebug.h"` → `"../../../tests/_debug/SmatchetAgentDebug.h"`.

### 3. Root build files
3. [CMakeLists.txt](../../../CMakeLists.txt) — the heavy hitter (~90 lines carry a moved-dir token: `Source_Core` ×88 lines, `Plugins/{Mcp,LuaConsole,Whisper}` ×19, `Target_Standalone` ×3, `UnrealPlugins` ×1). Token-level replacements (see § Risks for the substring footgun):
   - `Source_Core` → `Source/Core` — GLOB ([:606](../../../CMakeLists.txt)), all `REMOVE_ITEM`/`APPEND` ([:607-715](../../../CMakeLists.txt)), include dirs ([:719-737](../../../CMakeLists.txt), [:816-823](../../../CMakeLists.txt)), packaging copies ([:1011-1018](../../../CMakeLists.txt)).
   - `Target_Standalone` → `Source/Standalone` — GLOB ([:1120](../../../CMakeLists.txt)), icon + `.rc.in` ([:1123-1133](../../../CMakeLists.txt)).
   - `Plugins/Mcp` → `Source/Plugins/Mcp`, `Plugins/LuaConsole` → `Source/Plugins/LuaConsole`, `Plugins/Whisper` → `Source/Plugins/Whisper` — plugin targets + include dirs + `add_subdirectory` ([:825-834](../../../CMakeLists.txt), [:904-953](../../../CMakeLists.txt), [:957-959](../../../CMakeLists.txt)).
   - `UnrealPlugins` → `Source/UnrealPlugins` — `SMATCHET_UNREAL_THIRDPARTY_DIR` default ([:144-145](../../../CMakeLists.txt)).
   - **Unchanged**: `add_subdirectory(tests …)`, `assets/`, `scripts/` Lua copy.
4. [CMakePresets.json](../../../CMakePresets.json) — 3× `${sourceDir}/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet` → `${sourceDir}/Source/UnrealPlugins/…` ([:58](../../../CMakePresets.json), :238, :255). `binaryDir` `build/<preset>` unchanged.

### 3b. Project config seam (canonical value table — see § Coordination)
4b. [project.config.json](../../../project.config.json) — the agentic-layer single source of truth; update **in the same PR** so the future portable-purity denylist matches:
   - `project.literals` (:7-16) — `"Source_Core"` → `"Source/Core"`, `"Target_Standalone"` → `"Source/Standalone"`, `"UnrealPlugins"` → `"Source/UnrealPlugins"`. Keep `"Smatchet"`, `"SmatchetStandalone"`, `"SmatchetCore_DX12"`, `"DX12"`, `"ITrackerClient"` (target/symbol names — unchanged).
   - `lint.zones.strict` (:32-39) — re-root the 5 `Source_Core/src/…/` → `Source/Core/src/…/` + `Plugins/Mcp/src/` → `Source/Plugins/Mcp/src/`. `lint.zones.light` (:40) — `Source_Core/src/Ui/`, `Source_Core/include/Ui/`, `Target_Standalone/` → `Source/Core/…`, `Source/Standalone/`. `exempt` (:41) unchanged.
   - `ci.path_filters.code_globs` (:59) — `["Source_Core/**","Plugins/**","Target_Standalone/**"]` → `["Source/**"]` (recommended; simplest, now also covers `Source/UnrealPlugins/**` which the old globs omitted — accept, or enumerate `Source/Core/** Source/Plugins/** Source/Standalone/**` to preserve the exact old set). `docs_ignore` unchanged.
   - **Unchanged**: `build.targets`/`exe_path`, `visual_validation.trigger_globs` (`**/`-anchored), `golden.artifacts_dir`. This edit is the *canonical* twin of the still-inline `test-lint-rules.sh` `STRICT_GLOBS` (#9) + CI `paths:` (#14) until agentic Phase F wires scripts to read the config.

### 4. Test CMake (tests/ stays put but references moved dirs)
5. [tests/CMakeLists.txt](../../../tests/CMakeLists.txt) — **two distinct kinds of `Source_Core` refs that now diverge — do NOT blind-replace** (see § Risks):
   - **Test-source entries** (bare-relative to `tests/`): `Source_Core/<Unit>.test.cpp` (:14-54+) → `Core/<Unit>.test.cpp` — and note `tests/Source_Core/` holds **79 .cpp, one of which is NOT `.test.cpp`**: `Source_Core/SmatchetScenarioRegistry.stubs.cpp` (:91) → `Core/SmatchetScenarioRegistry.stubs.cpp`. These follow the `git mv tests/Source_Core tests/Core`. Any `${CMAKE_SOURCE_DIR}/tests/Source_Core` → `…/tests/Core`.
   - **Product-source refs**: `${CMAKE_SOURCE_DIR}/Source_Core/{src,include}/…` (compiled-in product TUs + include dirs :162-207, :293-313) → `…/Source/Core/…`; `Target_Standalone/CliArgCoercion.cpp` → `Source/Standalone/…` (:207); `Plugins/Whisper` → `Source/Plugins/Whisper` (:281-287).
   - `tests/support`, `tests/_helpers` paths unchanged. **Distinguisher** (not the file extension): a **bare-relative** `Source_Core/…` ref (no `${CMAKE_SOURCE_DIR}` prefix, not under `/src/` or `/include/`) is a test-mirror file → `Core/…` (covers both `*.test.cpp` and the lone `*.stubs.cpp`); a `${CMAKE_SOURCE_DIR}/Source_Core/{src,include}/…` ref is product → `Source/Core/…`.
6. [tests/Lua/CMakeLists.txt](../../../tests/Lua/CMakeLists.txt) — same `Source_Core/` → `Source/Core/` (:31-39, :64-87).
7. [tests/ui/CMakeLists.txt](../../../tests/ui/CMakeLists.txt) — verify; its refs are mostly `tests/…` (unchanged). Update any `Source_Core/`/`Target_Standalone/` hit.

### 5. Unreal plugin (NOT covered by the CMake gate — manual verify)
8. [SmatchetImGuiPlugin.Build.cs](../../../UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs) — staleness check only: `repoRoot` walk gains one level (`ThirdPartyDir` is now one deeper): `Path.Combine(ThirdPartyDir, "..","..","..","..")` → add a fifth `".."` ([:238](../../../UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs)); and `Path.Combine(repoRoot, "Source_Core")` → `Path.Combine(repoRoot, "Source", "Core")` ([:239](../../../UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs)). `ThirdPartyDir` itself ([:88](../../../UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs)) is plugin-relative — unchanged.

### 6. Lint / gate scripts (functional — break enforcement if missed)
9. [scripts/dev/test-lint-rules.sh](../../../scripts/dev/test-lint-rules.sh) — `STRICT_GLOBS` array ([:58-70](../../../scripts/dev/test-lint-rules.sh)) → `Source/Core/src/{Tracker,Sync,Persistence,Config,Commands}/`, `Source/Core/include/{…}/`, `Source/Plugins/Mcp/src/`; light-zone `case` ([:83](../../../scripts/dev/test-lint-rules.sh)) → `Source/Core/src/Ui/*|Source/Core/include/Ui/*|Source/Standalone/*`; header comment ([:8-12](../../../scripts/dev/test-lint-rules.sh)). **Must change in lockstep with AGENTS.md** — `--selftest` asserts they're identical. (Delta gate is *basename*-keyed, so the move itself adds no new violation triples; but stale globs make `zone_of` return `exempt` for every moved file → enforcement silently no-ops.)
10. [scripts/dev/run_cppcheck.py](../../../scripts/dev/run_cppcheck.py) — `TOP_LEVEL_DIRS = ("Source_Core", "Plugins", "Target_Standalone")` ([:12](../../../scripts/dev/run_cppcheck.py)) → `("Source",)` (all first-party TUs now share the `Source/` prefix).
11. Other classifiers (all carry **bare `Plugins/*` / `Plugins/**` globs** — anchor the replacement, see § Risks): [perf-marker-inventory.sh](../../../scripts/dev/perf-marker-inventory.sh) (scope case ~:73), [pillar2-scan.sh](../../../scripts/dev/pillar2-scan.sh) (:70 `Source_Core/*|Plugins/*|Target_Standalone/*`), [test-build-warnings.sh](../../../scripts/dev/test-build-warnings.sh) (:4,:48), [run_clang_tidy.ps1](../../../scripts/dev/run_clang_tidy.ps1) (:2 prose), [test-cppcheck-path-detection.sh](../../../scripts/dev/test-cppcheck-path-detection.sh) (mock fixtures :52,:72,:302-303 — keep in sync with #10). [coverage-delta-gate.sh](../../../scripts/dev/coverage-delta-gate.sh) — **now needs edit** (the test mirror moved): the test-surface pattern `tests/Source_Core/*.test.cpp` → `tests/Core/*.test.cpp` and the echo hint (:103) `tests/Source_Core/` → `tests/Core/`. Its product-source coverage trigger (if it greps `Source_Core/src/`) also re-roots to `Source/Core/src/`.
12. [docs/harness/claude-code/hooks/lint-cpp-common.sh](../../../docs/harness/claude-code/hooks/lint-cpp-common.sh) (:67 — bare `Plugins/*.cpp|Plugins/**/*.cpp` glob, **functional**: decides which edited files the lint hook touches). Canonical source for the gitignored `.claude/hooks/` copy → re-run `bash scripts/setup-harness.sh claude-code` after editing.

### 7. Review / CI config
13. [.coderabbit.yaml](../../../.coderabbit.yaml) — ~12 `path:` globs ([:50-127](../../../.coderabbit.yaml)): `Source_Core/**`→`Source/Core/**`, `Plugins/Mcp/**`→`Source/Plugins/Mcp/**`, the CMake glob `UnrealPlugins/**`→`Source/UnrealPlugins/**`, and the `path_filter` `!UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/**`→`!Source/UnrealPlugins/…` ([:37](../../../.coderabbit.yaml)).
14. `.github/` — grep the whole tree for all four tokens; **functional** hits:
   - [.github/CODEOWNERS](../../../.github/CODEOWNERS) (:56-57 `Source_Core/include/ITrackerClient.h`, `Source_Core/include/Commands/`) — review-routing; easy to miss, breaks silently.
   - [perf-pr-fast.yml](../../../.github/workflows/perf-pr-fast.yml) (:30-33 `Source_Core/** Plugins/** Target_Standalone/** UnrealPlugins/**`) and [coverage.yml](../../../.github/workflows/coverage.yml) (:16-17 `Source_Core/** Plugins/**`) — **real `paths:` filters**, re-root. [pillar2-scan.yml](../../../.github/workflows/pillar2-scan.yml) (:53 `grep -E '^(Source_Core/|Plugins/)'`) — inline grep, re-root. **[coverage-gate.yml](../../../.github/workflows/coverage-gate.yml) has NO `paths:` filter** — it runs on every PR and delegates to `coverage-delta-gate.sh` (:55), so the only functional re-root for the coverage gate is in that script (#11); coverage-gate.yml's `Source_Core/src/*.cpp` at :3 is a comment (optional cosmetic update).
   - [.github/pull_request_template.md](../../../.github/pull_request_template.md) (:16,:18,:20,:23) — coverage-gate docs.
   - **Path-filter deadlock caveat**: per [gate-enforcement-hardening.md](gate-enforcement-hardening.md), positive `paths:` filters that go un-updated would stop firing on the moved tree — re-root them, don't drop them.

### 8. Publish / installer
15. [scripts/publish/installer/SmatchetStandalone.iss](../../../scripts/publish/installer/SmatchetStandalone.iss) (:41) — `..\..\..\Target_Standalone\smatchet.ico` → `..\..\..\Source\Standalone\smatchet.ico` (root-relative; same hop count). Plus `scripts/publish/*.ps1` Unreal-path refs (`release_github.ps1`, `build_and_deploy_unreal_plugin.ps1`, `install_unreal_plugin.ps1`, `common/SmatchetCMakeCommon.ps1`).

### 9. Canonical / live docs (update for accuracy)
16. [AGENTS.md](../../../AGENTS.md) — § Project rules § Layout (the `Source_Core/{src,include}` / `Target_Standalone/` / `Plugins/{Mcp,LuaConsole}` description), § Tiered enforcement strict/light zone lists (lockstep with #9), the dual-target full-verify command, and incidental path mentions. (`.claude/CLAUDE.md` `@`-imports AGENTS.md — no separate edit.)
17. [docs/plans/shipped/high-integrity-cpp-enforcement.md](high-integrity-cpp-enforcement.md) strict/light zone table; [docs/CONTEXT.md](../../CONTEXT.md) (:77); [README.md](../../../README.md) (:136); [BUILD.md](../../../BUILD.md) (:181).
18. `agents/*.md` — **functional**: [git-janitor.md](../../../agents/core/git-janitor.md) path blacklist prose (:75) **and** the `disallow` regex (:91) — collapse to `^(Source/|cmake/|CMakeLists\.txt$|CMakePresets\.json$)` since all C++ now lives under `Source/`. **Charter/prose** (update for accuracy, incl. the `tests/Source_Core/` → `tests/Core/` mirror rename): [test-rig.md](../../../agents/core/test-rig.md) (:3,:35,:46,:66 — owner doc, names `tests/Source_Core/<Unit>.test.cpp` as its charter), [coderabbit-triage.md](../../../agents/core/coderabbit-triage.md) (:131,:163), [p4-blame.md](../../../agents/project/p4-blame.md) (:51-52), [delegation.md](../../agent-rules/delegation.md) (:311,:322), plus `mechanic.md` (:38), `debug-detective.md`, `perf-*.md` (+ the `agents/_shared/skills/*` mirrors), `build-doctor.md`, `unreal-bridge.md`, `architect.md`, `code-review.md`, `test-author.md`. Also two **product-code comments** pointing at the moved test path: [Plugins/Whisper/SilenceTrim.cpp:2](../../../Plugins/Whisper/SilenceTrim.cpp) and [WavWriter.h:9](../../../Plugins/Whisper/WavWriter.h) (`tests/Source_Core/…` → `tests/Core/…`).
19. [docs/high-integrity/baseline.md](../../high-integrity/baseline.md) — **regenerate, do not hand-edit**: after the move + #9, run `bash scripts/dev/test-lint-rules.sh --catalog --refresh`.
20. [docs/plans/shipped/source-core-dir-reorg.md](source-core-dir-reorg.md) — add a one-line banner noting component roots since moved under `Source/` (point here). Historical — don't rewrite.

## Existing utilities reused

- `git mv` — preserves blame/history (rename detection) vs delete+add. One per top-level dir.
- `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS …)` [CMakeLists.txt:606](../../../CMakeLists.txt) — re-globs moved `.cpp` automatically once the pattern is re-rooted.
- `bash scripts/dev/test-lint-rules.sh --selftest` — guards AGENTS.md ↔ script glob parity (run after #9 + #16).
- `bash scripts/dev/test-lint-rules.sh --catalog --refresh` — regenerates `baseline.md` (#19).
- `bash scripts/setup-harness.sh claude-code` — regenerates gitignored `.claude/` adapter (hooks/agents) from canonical (#12).
- `bash scripts/dev/test-all.sh` + `bash scripts/dev/is-pure-docs-diff.sh` — the standard pre-push gate suite (forces the build gate since this is not a pure-docs diff).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — pure file moves, zero logic change; binary is byte-equivalent modulo `__FILE__` strings. Perf-neutral by construction.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — no code paths added or moved across threads.
- **Pillar 3 (never crash)**: no impact directly; preserves the high-integrity lint gate by re-rooting its zone globs (#9) so enforcement does not silently lapse.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

The diff *moves* `Source_Core/` but changes no executable code, so the perf system is **N/A by construction** (file relocation only, binary byte-equivalent modulo `__FILE__`).

1. **PR-fast CI** — N/A — no scenario logic changed. If the perf-PR path filter trips on the moved tree, label `perf-out-of-band` (move-only, baseline unchanged).
2. **Pillar 2 static scanner** — N/A — no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A — `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A — no new sync-stall code path.
5. **Marker inventory** — N/A — no `SMATCHET_UI_PERF_SCOPE` added; but `perf-marker-inventory.sh` scope classifier is re-rooted (#11) so the inventory tooling keeps working.

**Override**: `perf-out-of-band` label if CI perf filters fire on the relocation.

## Risks / non-goals

- **`Plugins/` is a substring of `UnrealPlugins/`** — a blind `sed 's#Plugins/#Source/Plugins/#'` corrupts `UnrealPlugins/` → `UnrealSource/Plugins/`. **Mitigation**: in `CMakeLists.txt` every `Plugins/` is suffixed, so replace the explicit tokens `Plugins/Mcp`, `Plugins/LuaConsole`, `Plugins/Whisper` (plus the unique `Source_Core`, `Target_Standalone`, `UnrealPlugins`). **But scripts/CI/docs use bare `Plugins/*` / `Plugins/**` globs** (`lint-cpp-common.sh:67`, `pillar2-scan.sh:70`, `perf-pr-fast.yml`, `pillar2-scan.yml`, several docs) that must *also* move — there, anchor on a leading boundary that excludes `Unreal`: e.g. `sed -E 's#(^|[^a-zA-Z])Plugins/#\1Source/Plugins/#g'` (the `[^a-zA-Z]` class never matches the `l` in `Unreal`), or do `UnrealPlugins → Source/UnrealPlugins` first then a `(?<!Unreal)Plugins/` pass. Verify with the § Verification grep guard, which deliberately catches a stray `UnrealSource/Plugins`.
- **`tests/CMakeLists.txt` two-kinds-of-`Source_Core` footgun** — after the `tests/Source_Core → tests/Core` rename, the file's `.test.cpp` entries must go to `Core/` while its compiled-in product refs go to `Source/Core/`. A blind `Source_Core → Source/Core` pass sends the test files to a nonexistent product path → CMake configure error (loud, not silent — good). **Mitigation**: replace `.test.cpp` entries (and any `${CMAKE_SOURCE_DIR}/tests/Source_Core`) → `Core/` *first*, then product `Source_Core/{src,include}` → `Source/Core/`.
- **Unreal `Build.cs` + packaging are NOT exercised by the CMake dual-target gate** — the staleness-path bug (#8) would only surface at UE plugin build time. **Mitigation**: manual UE package build, or at minimum a static re-read of the walk depth. → manual residue + backlog (see § Verification).
- **`baseline.md` / delta-gate drift** — the delta gate keys on `(rule, basename, hash)`, so the move adds no new triples (move-safe); the only failure mode is forgetting #9, which silently disables enforcement. **Mitigation**: `--selftest` + `--catalog --refresh` are mandatory steps, not optional.
- **Large diff vs in-flight branches** — a four-dir move conflicts with anything else touching those trees. **Mitigation**: land fast on a dedicated branch off `develop`; coordinate before merging.
- **Concurrency with `agentic-layer-project-independence` Phase E/F** — both plans edit `AGENTS.md`, CI `paths:` filters, `scripts/dev/*`, and the project-literal/zone values. **Mitigation**: sequence after agentic Phase F (§ Coordination); never run the two in the same window. The Phase-F `test-portable-purity.sh` gate can *block* this reorg's PR if a portable `agents/core/` file is left holding an old literal — update `project.config.json` `project.literals` in the same PR.
- **vexp / harness indices stale** — `.understand-anything/*` and `.claude/`,`.codex/`,`.cursor/` adapters are auto-generated/gitignored; reindex + re-run `setup-harness.sh` after the move. No manual edits.
- **Non-goals**: (a) `tests/` root does **not** move (stays at repo root); (b) only the `tests/Source_Core/` mirror renames to `tests/Core/` — the rest of `tests/` (`Lua/`, `Plugins/`, `ui/`, `support/`, …) keeps its layout; (c) bare `#include` directives are **not** rewritten (only Logger.cpp's relative include); (d) core is **not** flattened to `Source/` directly (subdir-per-component chosen); (e) `build/<preset>` output dirs unchanged; (f) `docs/plans/shipped/**` left as historical record.

## Verification

Per `AGENTS.md` § Verification automation. Reconfigure first — CMake explicit lists changed, so delete/reconfigure the build dir (`cmake --preset ninja-iter-msvc`) before building.

- **Build gate (primary)**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — proves no TU dropped/duplicated and every include re-resolved across both targets. A stale `REMOVE_ITEM` whose path no longer matches is a silent no-op, so a green dual-target build is the real proof.
- **Bucket A (pure-logic ctest)**: configure with `-DSMATCHET_BUILD_TESTS=ON` (or `ninja-test-msvc`) and run CTest — proves `tests/` CMake re-rooting (#5-7) is correct.
- **Lint-gate selftest**: `bash scripts/dev/test-lint-rules.sh --selftest` (AGENTS.md ↔ script parity) then `bash scripts/dev/test-lint-rules.sh` (delta gate stays green — move is basename-safe).
- **Full pre-push suite**: `bash scripts/dev/test-all.sh` (includes shell-lint + lint-rules + coverage-delta).
- **Grep guards** (post-move; common excludes: `--glob '!docs/plans/shipped/**' --glob '!docs/plans/shipped/source-core-dir-reorg.md' --glob '!docs/plans/shipped/source-root-consolidation.md' --glob '!build/**' --glob '!.understand-anything/**' --glob '!docs/high-integrity/baseline.md'`):
  - `rg '\.\./\.\./tests/_debug' Source/` → empty (Logger.cpp fixed).
  - `rg -n 'Source_Core|Target_Standalone' <excludes>` → empty, OR only the `Source_Core` *comments* in `Build.cs` (:93,:231,:294) + `Whisper/CMakeLists.txt` (:37-38) if you chose to leave them (recommend updating for cleanliness).
  - `rg -n '(^|[^/])UnrealPlugins/|(^|[^./A-Za-z])Plugins/(Mcp|LuaConsole|Whisper)' <excludes>` → empty (no old top-level paths survive un-rooted).
  - **Corruption sentinel** — `rg -n 'UnrealSource|Source/Source|Source/Core/include/Source'` → MUST be empty (catches a botched substring replace).
- **Bucket E (ImGui Test Engine)**: not required — no UI surface changed.
- **Manual residue**: the Unreal `Build.cs`/packaging path (#8) has **no CMake-gate coverage**. Deferred-automation plan: add a CI dry-check (or a unit assertion on the computed `repoRoot`/`sourceCoreDir`) — file a `docs/backlog/agent-self-improvement/tooling.md` entry at ship time. Until then, a one-time manual UE package build verifies the staleness walk + curated-header copy.

## Out of scope (flagged, not designed)

- **Moving `tests/` under `Source/`** — explicitly excluded; revisit only if the tooling that keys off `tests/` (coverage-delta, CI filters, git-janitor whitelist) is migrated in the same pass. (Note: the `tests/Source_Core/` → `tests/Core/` mirror rename **is in scope** per grill decision — only the `tests/` *root* stays put.)
- **Per-component `CMakeLists.txt` split** (true `add_subdirectory` per component instead of the root GLOB) — larger architectural change; not required for the consolidation. *(Since shipped separately — 2026-07: per-component `CMakeLists.txt` files under `Source/*`, pulled in from the root via `include()` rather than `add_subdirectory()` so every target/variable/source-property stays in root directory scope for byte-identical build output.)*
- **Rewriting bare includes to path-qualified (`#include "Tracker/JiraClient.h"`)** — include-hygiene improvement deferred; this plan keeps the flat include namespace.

## Implementation log
- `c71473d3` · refactor: git mv 5 roots under `Source/` (+ `tests/Source_Core`→`tests/Core`); re-root CMake/presets/project.config/tests-CMake/Build.cs/lint+CI gates/docs/agent prose; Logger.cpp include depth; regen both baselines. 638 files, 1003/1003.
- `e0d543b8` · fix(tests/CMakeLists): the `Plugins/Mcp` two-kinds (mirror `tests/Plugins/Mcp/*.test.cpp` vs product `${CMAKE_SOURCE_DIR}/Plugins/Mcp/src`) — blanket sed mis-prefixed mirror entries + missed product Mcp/Whisper refs. Path-only.
- `18ddb03b` · fix(test-lint-rules): delta-gate base-scan now globs legacy roots + canonicalises before `zone_of`, so the `origin/develop` base worktree (old `Source_Core/` layout) is discoverable — else the 3 grandfathered `define-imgui` Tracker violators read as NEW.

## Deviations from plan
- **`Plugins/Mcp` had the same two-kinds footgun as `Source_Core`** (mirror `tests/Plugins/Mcp/*.test.cpp` vs product `Plugins/Mcp/src`) — the plan only called out the `Source_Core` two-kinds. Caught by the `ninja-test-msvc` configure (loud failure), fixed in `e0d543b8`.
- **Delta-gate discovery, not basename-keying, was the lint risk.** Plan § Risks said the move is "basename-safe" so the delta gate stays green; in fact the base-worktree scan couldn't *discover* old-path files (`git ls-files Source/Core/src/**` finds nothing on develop), so grandfathered violators looked NEW. Required a scanner fix (`18ddb03b`), not just `--selftest`. Plan § Verification should run the **delta** gate, not only `--selftest`, before declaring lint green.
- **MSVC 14.38 toolset pin needed but unpinned in tooling.** `with-msvc-env.sh` calls plain `vcvars64` → VS18 BuildTools 14.50 → STL1001. Built via a throwaway pinned wrapper. Durable fix deferred (backlog) — not in this path-only PR's scope.
- **`Build.cs` / Unreal packaging not built** (no CMake-gate coverage; no UE on this box) — edits applied (repoRoot +1 level, `"Source","Core"`) but unverified. Backlog: manual UE package build or a `repoRoot` unit assertion.
- **`.claude/` adapter not re-run** (`setup-harness.sh`) — gitignored, local-only; canonical `docs/harness/` hooks were swept. No commit impact.

## Verification (actual)
- **Dual-target build** `ninja-iter-msvc SmatchetStandalone SmatchetCore_DX12` — **PASS** 677/677 (build-doctor, 2026-05-29). Proves no TU dropped/duplicated, all includes re-resolved.
- **CTest** `ninja-test-msvc` — **PASS** 2/2 (`SmatchetTests` 414/414, `SmatchetLuaTests`). Proves tests-CMake two-kinds re-root.
- **`test-lint-rules.sh --selftest`** PASS; **delta gate** PASS (after `18ddb03b`). **`test-shell-lint.sh`** PASS. **`test-portable-purity.sh`** PASS (baseline holds; run via working `python`).
- **Grep guards** — residual old-token scan empty (excl. intentional plan/ADR); corruption sentinel clean (`Source/Source/` fixed in 2 CI files post-sweep).
- **Not run**: publish build (surface untouched), Unreal package build (no UE/CMake-gate; see Deviations), bucket-E (no UI change).
