# Plan — Reopen: give the files at the Core root directories

> **Slug**: `core-root-directory-layout-reopen` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Plan-link convention (tier-less — write ALL plan references this way)**: link other plans by the **tier-less** form `docs/plans/<slug>.md` — NOT `docs/plans/active/<slug>.md` or `docs/plans/shipped/<slug>.md`. The link gates (`test-markdown-links`) resolve a tier-less path against `active/`, `shipped/`, or `deferred/`, so the reference keeps working when a plan is archived (`git mv active → shipped`) without any ref-sweep. A **tiered** link passes the local check while the target still lives in `active/`, then 404s on CI after the archival move — the `markdown-links-local-passes-ci-fails-after-plan-archive` trap; `test-markdown-links.sh --merge-tree-warn` flags tiered links before they rot.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

**DRAFT — awaiting the owner's accept or reject. No implementation is authorised by this file.**

Earlier in this session the orchestrator recommended moving the files at the root of `Source/Core/src/` and `Source/Core/include/` into named subdirectories. It made that recommendation before reading the project's records. The records show the project already weighed this and chose otherwise. The owner asked for a plan that makes the case for reopening, as strongly as the evidence allows and no stronger.

### Recorded decision

- [`source-core-dir-reorg`](../source-core-dir-reorg.md) § Files to modify, taxonomy table: "residual `Source_Core/src/` root (+ new `Ai/`, `Core/` if useful) | everything else | exempt-by-default". The root was left on purpose as the exempt remainder.
- Same plan, § Files to modify, same table: the `Persistence/` row lists "`LocalCacheManager`, `BackendAuditTrail`, `FieldEditAuditSource`, `*Cache`" as strict. This is the best fact for reopening: the plan's own name pattern `*Cache` would match `AttachmentDiskCache`, which still sits at the root. Whether the cache services were left behind on purpose is unverified; the plan's Deviations section does not mention them (grep for "cache" in that plan finds only the Persistence row).
- Same plan, § Approach: "Trade named: a flat include namespace persists (you can still `#include "JiraClient.h"` from anywhere) — we accept that because the goal is lint **zones**, not include hygiene".
- Same plan, § Risks / non-goals: "**Non-goal**: `#include` hygiene / qualified-path includes (`#include "Tracker/JiraClient.h"`). Explicitly rejected".
- Same plan, § Deviations from plan: "**Ui/ROOT boundary kept inclusive, not perfected.** Light zone == exempt for the gate, so borderline Ui-vs-root placements have zero enforcement consequence; not bikeshedded."
- [`source-root-consolidation`](../source-root-consolidation.md) § Approach: the move re-roots include paths and keeps every component's internal layout; "a flat include namespace persists … accepted, same as the precursor reorg."
- [ADR-0011](../../adr/0011-single-source-root.md) § Decision: "**Flat include namespace is kept**"; § Considered options rejects path-qualified includes ("far larger blast radius and would churn the Unreal-side consumer"); § Consequences: "treat the `Source/` shape as intentional architecture, not an implementation detail." Note: ADR-0011 decides the component roots under `Source/`, not the layout inside `Source/Core/`.
- [`core-include-dag`](../core-include-dag.md) § Approach, phase 1: break infra→ui edges by moving "two leaf enum headers to root `include/`". § Deviations: the final ranking is "`Ui(6) > AppController(5) > Commands(4) > Tracker(3) > Sync/Persistence(2) > Config(1) > root-leaf headers(0)`". Layer-neutral leaf headers belong at the include root by design.
- [`CONTEXT-MAP.md`](../../../CONTEXT-MAP.md) § Code without a leaf: "Some code sits outside every leaf directory, so no leaf auto-loads when it is edited: the subsystem TUs at the `Source/Core/src/` root and the headers under `Source/Core/include/`. Their rules live in the doc named here". The table then routes each root cluster (cache services, grid, P4 annotate, Lua bindings, the ImGui-free units) to its governing doc.

### What this plan asks

Should the owner reopen the flat-root decision for one self-contained cluster, as a pilot with a stop condition? The plan recommends a narrow pilot: move the six cache-service `.cpp` files into the existing `Source/Core/src/Persistence/`, keep their headers at the include root, and reject the wider move.

## Approach

### Measured today (this branch)

- `Source/Core/src/` root: **107** `.cpp` and **6** `.h`. `Source/Core/include/` root: **181** `.h`.
- Existing subdirs: `src/` has Commands, Config, Diagnostics, Imaging, Persistence, Privacy, Sync, Tracker, Ui, Vcs. `include/` adds Interfaces, Json, Lua, Types.
- Precedents since the reorg. `Diagnostics/`, `Privacy/`, `Imaging/` were added by [`log-a-bug-github`](../log-a-bug-github.md) as new include roots. `Vcs/`, `Interfaces/`, `Types/`, `Json/`, `Lua/` are **not** include roots: their headers are path-qualified (`#include "Json/BoundedJsonParse.h"` 83 times, `"Interfaces/IAppSync.h"` 4, `"Types/FieldEditTypes.h"` 4, `"Vcs/GitHubCommits.h"` 3, `"Lua/LuaHookGuard.h"` 2). The tree has already drifted from "zero path-qualified includes".

Clusters by name prefix at the two roots (files / lines):

| Cluster | src root | include root | Core CMake literals | tests/CMakeLists.txt | tests/Lua | tests/fuzz |
|---|---|---|---|---|---|---|
| AI assistant (`Ai*`, `Anthropic*`, `Ollama*`, `OpenAi*`, `AgentsMd*`, `SmatchetChat*`) | 22 / 3,975 | 30 / 2,042 | 17 | 34 | 0 | 4 |
| Lua bindings (`AppController_Lua*`, `Lua*`) | 10 / 4,099 (incl. 1 `.h`) | 7 / 562 | 14 | 1 | 2 | 0 |
| Grid + field editor (`TicketGrid*`, `Grid*`, `SmatchetGrid*`, `TicketFieldEditor*`, `DataFreshness*`) | 16 / 5,586 (incl. 2 `.h`) | 16 / 1,590 | 0 | 7 | 0 | 1 |
| Cache services (`EditMetaCacheService`, `IssueTransitionsCacheService`, `ProjectComponentsCacheService`, `AttachmentDiskCache`, `AttachmentCachePure`, `LookupPayloadsPure`) | 6 / 1,538 | 6 / 512 | 0 | 12 | 0 | 0 |
| Perforce annotate (`P4Annotate*`, `CallstackParser*`) | 3 / 1,264 | 3 / 311 | 0 | 4 | 0 | 1 |

All root-file literals today: `Source/Core/CMakeLists.txt` 37, `tests/CMakeLists.txt` 114, `tests/Lua/CMakeLists.txt` 5, `tests/fuzz/CMakeLists.txt` 18 (`${_fuzz_core}/src/…`).

Other registrations, checked per cluster:

- **Include roots.** `SmatchetCoreInterface` in `Source/Core/CMakeLists.txt` lists `include` plus Tracker, Sync, Persistence, Config, Ui, Diagnostics, Privacy, Imaging. The private src roots (`:323`-`:330`) already include `Source/Core/src/Persistence`. A new header dir needs a new root in both CMake and `INCLUDE_ROOTS` in `agent-layer/agents/scripts/core/include_cycle_audit.py`, plus `tests/fuzz/CMakeLists.txt` (it lists `include` and `include/Ui` per target).
- **Layer ranks.** `layer_rank()` returns 5 for any basename starting `AppController`, 6/4/3/2/1 for Ui/Commands/Tracker/Sync+Persistence/Config, and 0 for everything else. A new directory (`Ai/`, `Grid/`, `P4/`) ranks **0**, same as the root, unless the audit is edited. Back-edges are checked header→header between two rank ≥ 1 layers only. So a new rank-0 directory changes no back-edge result. A header that lands in an existing ranked dir does: `IssueTransitionsCacheService.h`, `ProjectComponentsCacheService.h` and `LookupPayloadsPure.h` include `Tracker/TrackerFieldSchema.h` (rank 3); moved to `include/Persistence/` (rank 2) they would be three new low→high back-edges.
- **Silent-drop risk.** The audit says an include that resolves to no root "is simply not an edge". A new header dir added to CMake but not to `INCLUDE_ROOTS` drops that cluster's edges from the gate without a failure.
- **Lint zones.** `project.config.json` `lint.zones` and `STRICT_GLOBS` in `agent-layer/agents/scripts/project/lint-rules.d/00-common.sh` are directory globs. Root files fall through to `exempt`. A file moved into `Source/Core/src/Persistence/` becomes strict with no config edit; a new dir stays exempt unless both lists change.
- **Coverage.** `coverage.excluded` in `project.config.json` and `scripts/dev/coverage.sh:313-314` exclude only `Source*Core*src*Ui` / `Source*Core*include*Ui`. Root and Persistence are both measured, so moving cache, AI, Lua or P4 files does not change the measured surface. Moving grid files under `Ui/` would drop them from coverage.
- **Area map.** `docs/area-map.json` routes by globs. `grid-engine` (`**/TicketGrid*`, `**/Grid*`, `**/SmatchetGrid*`), `p4-annotate` (`**/P4Annotate*`, `**/CallstackParser*`) and `lua-binder` (`**/*Lua*`) already route by name. There is **no AI route**. The cache services match no route today; `offline-sync` covers `Source/Core/src/Persistence/**`.
- **Path citations** (`git grep` of `Source/Core/{src,include}/<cluster file>`): AI — Source 17, live Markdown 36, shipped plans 28, scripts/JSON/YAML 6. Lua — 20 / 33 / 13 / 0, plus 1 in `include_cycle_audit.py` selftest. Grid — 3 / 87 / 86 / 4 (incl. `.github/workflows/tsan-linux-nightly.yml:39`, `scripts/dev/perf-scenario-map.json`). Cache — 1 / 9 / 3 / 2 (`scripts/dev/mutation-smoke-corpus.json:134,142`). P4 — 0 / 9 / 11 / 2, plus 16 in `agent-layer/` (the `p4-single-spawn` rule pins `Source/Core/src/P4Annotate.cpp` in `AGENTS.md`, `lint-rules.d/77-p4-single-spawn.sh`, `test-lint-rules.sh` and two bats files).
- **Unreal plugin.** `SmatchetImGuiPlugin.Build.cs:116-125` adds one packaged include dir; packaging copies a flat curated header set. None of the five clusters is in it. No Unreal edit for any option.

**Validation limit.** A Linux container compiles only the `posix-core-check` preset (its description: Lua/MCP/AI/Whisper OFF) and runs the Linux test subset (`ninja-test-linux`). MSVC, DX12, Unreal packaging and Android are CI-only. The AI and Lua clusters therefore cannot be compiled here at all. The cache cluster has no `SMATCHET_WITH_*` guard and is in both `posix-core-check` and the Linux test targets.

### Options

**(a) Do nothing.** Cost: zero. Keeps the recorded decision. The name-prefix convention plus § Code without a leaf already tell an agent which doc governs a root file, and the area map routes grid, P4 and Lua by name. What it leaves: no leaf auto-loads for any root file (the CONTEXT-MAP text says so); the cache services route to no area; AI has no area route. The last two are fixable with `docs/area-map.json` globs, no move needed.

**(b) Move only the `.cpp` of one self-contained cluster; headers and the flat include namespace stay.** Best candidate: the cache cluster into the existing `Source/Core/src/Persistence/`. Touches: 6 `git mv`; 12 literals in `tests/CMakeLists.txt` (`:272,281,286,288,291,292` in the `SmatchetTsanTests` target, which `ninja-test-linux` builds; `:1605,1612,1617,1619,1632,1633` in the `SmatchetTests` target, which starts at `:539` and is not in the Linux subset); 2 entries in `scripts/dev/mutation-smoke-corpus.json`; the cache row of CONTEXT-MAP § Code without a leaf; 1 live plan path citation. Note: the active plan `input-ux-w3-attachments-drop-paste` (§ item 13) also edits that same CONTEXT-MAP Persistence row, so the two will conflict textually. Zero edits to Core CMake, include roots, the include audit, lint config, coverage, fuzz, the area map, or Unreal. Buys: the Persistence leaf `AGENTS.md` auto-loads when these TUs are edited; 1,538 lines enter the strict zone; area routing to `offline-sync` with no new glob. Costs: the cluster is split across `src/Persistence/` and `include/`, the same cpp-only trade the reorg named; headers stay exempt. It contradicts the reorg's "root is the exempt remainder" only for files the CONTEXT-MAP already assigns to the Persistence leaf. Any other cluster as (b) needs a **new** directory, which buys no leaf (no leaf exists for AI, Grid, Lua or P4), no zone (rank 0 / exempt) and no routing beyond what name globs give today.

**(c) Move `.cpp` and `.h` of one cluster with a new include root.** Example AI into `Ai/`: 52 files, 17 Core CMake + 34 test + 4 fuzz literals, a new root in CMake, the audit's `INCLUDE_ROOTS` and each fuzz target, 87 live citations. Buys a home for a future AI leaf. Costs: a new rank-0 dir gains nothing in the DAG; the cluster is OFF in `posix-core-check`, so the move is unverifiable in the container. Cache into `include/Persistence/` is worse: three new back-edges (above). P4 costs an agent-layer rule edit in another repository.

**(d) The full move.** 107 `.cpp` + 181 `.h`; at least 174 CMake path literals; new roots for every new dir; a taxonomy for ~180 headers the include-DAG plan placed at the root on purpose as rank-0 leaves. Contradicts every recorded decision above. Cost is a repeat of the reorg; benefit depends on leaves that do not exist yet.

### Recommendation

Take **(b), the cache-cluster pilot only**: move the six cache `.cpp` files into `Source/Core/src/Persistence/`, headers stay put. It is the one move whose benefit (a leaf that already exists and already claims these files) needs no new structure, and whose registrations are all visible. The six `.cpp` compile in the container (`posix-core-check`) and the six `SmatchetTsanTests` literals are exercised there; the six `SmatchetTests` literals (`tests/CMakeLists.txt:1605-1633`) are CI-only. Reject (c) and (d). Pair (a)'s cheap gap fix: if the owner wants AI routed, add an `ai` route to `docs/area-map.json` as a separate change.

**Stop condition.** Stop after the pilot. Do not plan a second cluster unless (1) the pilot merged with no gate override and no strict-zone deviation, and (2) a second cluster has a leaf `AGENTS.md` with real content first (CONTEXT-MAP § Growth path: "Add a leaf when content exists"). Revert the pilot if any strict-zone rule needs a new `SMATCHET_DEVIATION` to land it.

## Interface contracts

N/A — moves six `.cpp` files with no content change; no header, command, binding, MCP schema or storage format changes.

## Files to modify

### File-level changes

N/A for a Symbol / CMake-variable table — no symbol or CMake variable is added or renamed.

**Moves (`git mv`, pure renames, own commit):**

1. [`Source/Core/src/EditMetaCacheService.cpp`](../../../Source/Core/src/EditMetaCacheService.cpp) → `Source/Core/src/Persistence/EditMetaCacheService.cpp`
2. [`Source/Core/src/IssueTransitionsCacheService.cpp`](../../../Source/Core/src/IssueTransitionsCacheService.cpp) → `Source/Core/src/Persistence/`
3. [`Source/Core/src/ProjectComponentsCacheService.cpp`](../../../Source/Core/src/ProjectComponentsCacheService.cpp) → `Source/Core/src/Persistence/`
4. [`Source/Core/src/AttachmentDiskCache.cpp`](../../../Source/Core/src/AttachmentDiskCache.cpp) → `Source/Core/src/Persistence/`
5. [`Source/Core/src/AttachmentCachePure.cpp`](../../../Source/Core/src/AttachmentCachePure.cpp) → `Source/Core/src/Persistence/`
6. [`Source/Core/src/LookupPayloadsPure.cpp`](../../../Source/Core/src/LookupPayloadsPure.cpp) → `Source/Core/src/Persistence/`

None includes a src-local header, so the existing private root `Source/Core/src` still resolves everything. `file(GLOB_RECURSE CORE_SOURCES …)` at `Source/Core/CMakeLists.txt:30` picks them up.

**Registrations (second commit):**

7. [`tests/CMakeLists.txt:272`](../../../tests/CMakeLists.txt) — 12 literals at `:272,281,286,288,291,292,1605,1612,1617,1619,1632,1633` gain `Persistence/`.
8. [`scripts/dev/mutation-smoke-corpus.json:134`](../../../scripts/dev/mutation-smoke-corpus.json) — `file` of `EMC-01` and `EMC-02` (`:134`, `:142`).
9. [`CONTEXT-MAP.md:41`](../../../CONTEXT-MAP.md) — the cache row keeps only `AppController_LocalCacheDb.cpp`; the six files are now under the leaf.
10. [`docs/plans/active/pane-stale-delete-collision.md:75`](../pane-stale-delete-collision.md) — path citation. Historical ledgers (`docs/self-improvement/historical-review-findings*`, shipped plans) stay as written.

## Existing utilities reused

- `git mv` — keeps blame; [`source-core-dir-reorg`](../source-core-dir-reorg.md) § Risks asks for pure-rename commits.
- `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")` — `Source/Core/CMakeLists.txt:30`; no source-list edit.
- The private include root `Source/Core/src/Persistence` — `Source/Core/CMakeLists.txt:328`; already present.
- `zone_of()` — `agent-layer/agents/scripts/project/lint-rules.d/00-common.sh`; classifies the moved files strict with no edit.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — moves whole files; nothing is extracted or split.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — same object code, new path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no code change.
- **Pillar 3 (never crash)**: no impact — no code change.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

The pilot touches `Source/Core/` paths only by rename. Per [`pillar-1-2-perf-review-system`](../pillar-1-2-perf-review-system.md):

1. **PR-fast CI** — N/A: no cache file is in `scripts/dev/perf-scenario-map.json`; no code changes.
2. **Pillar 2 static scanner** — N/A: no new sync I/O.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` added.

**Pre-push local check**: N/A — no scenario exercises a renamed path differently.

**Override**: `perf-out-of-band` not needed.

## Risks / non-goals

- **Strict-zone exposure.** 1,538 lines become strict. `grep` found no `printf`, `std::cerr`, `#define ImGui`, `Ui/` or `AppController` include in the six files, and no `SMATCHET_DEVIATION`. The Windows post-merge `narrowing-conversions` clang-tidy lane on the strict zone can't be run here (unverified); mitigation: the stop condition reverts the pilot rather than adding deviations.
- **Delta gates on renames.** ADR-0011 § Consequences says the high-integrity delta gate keys on `(rule, basename, hash)`, so a pure rename adds no violations. Not re-probed for this pilot (unverified); `test-lint-rules.sh --diff origin/develop` on the branch settles it.
- **Split cluster.** `.cpp` under `Persistence/`, `.h` at the include root. Accepted: moving the headers would add three rank 2 → rank 3 back-edges via `Tracker/TrackerFieldSchema.h`.
- **Save formats, tracker interfaces, MCP wire, localization**: untouched.
- **Unreal vs standalone; dual-target**: no `SMATCHET_WITH_*` gate in these files; Unreal consumes a flat packaged header set that excludes them. `SmatchetCore_DX12` compiles the same glob, so a missed path would fail CI's dual-target build, not ship.
- **What would make this proposal wrong.** (1) If the Persistence leaf's invariants don't really govern these services — but CONTEXT-MAP:41 already says they do. (2) If the owner values one uniform rule ("root = exempt remainder") over one leaf auto-loading; then (a) is right. (3) If the Claude Code leaf shim is not generated in the owner's harness, the main benefit disappears for that harness. (4) If a hidden path consumer exists that `git grep` on `Source/Core/src/<name>` misses (a computed path in a script); CI would expose it.
- **Non-goals**: path-qualified includes; any new directory or include root; header moves; any other cluster; ADR edits.

## Implementation handoff

N/A — single slice, owner `Source/Core/src/Persistence/AGENTS.md`. Write set: items 1–10 above. Order: move commit first (pure renames), registration commit second. Inline inventory: the 12 `tests/CMakeLists.txt` lines and two corpus lines listed in § Files to modify. Pre-resolved: headers stay at the include root (back-edge reason above).

## Verification

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**: the existing cache tests (`Core/EditMetaCacheService.test.cpp`, `Core/IssueTransitionsCacheService.test.cpp`, `Core/ProjectComponentsCacheService.test.cpp`, `Core/LookupPayloadsPure.test.cpp`, `Core/AttachmentCachePure.test.cpp`, `Core/AttachmentDiskCache.test.cpp`) green on `ninja-test-linux` and the TSan target; no new tests (no behaviour change).
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no UI change.
- **Bash-driver scenario / screenshot / sanitizer**: `posix-core-check` compiles the moved TUs in the container; `ninja-tsan-linux` covers `SmatchetTsanTests`. `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` clean; `include_cycle_audit.py` unchanged result. `git grep -nE 'Source/Core/src/(EditMetaCacheService|IssueTransitionsCacheService|ProjectComponentsCacheService|AttachmentDiskCache|AttachmentCachePure|LookupPayloadsPure)\.cpp'` returns only historical ledgers and shipped plans.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — CI-only from a Linux container.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete. **Still owed**: this draft skipped the grill pass; it must run before the plan is final.
- **Manual residue**: none.

## Open questions

- Does the owner's harness generate the leaf `CLAUDE.md` shim for `Source/Core/src/Persistence/`? — owner. In this checkout `Source/Core/src/Persistence/CLAUDE.md` exists and is gitignored (`.gitignore:144`); whether it auto-loads for the six moved files was not tested.
- Should an `ai` route be added to `docs/area-map.json` regardless of this decision? — owner; separate change.
- **Decision request:** accept option (b), the cache-cluster `.cpp` pilot into `Source/Core/src/Persistence/` with the stop condition above; or pick another listed option ((a), (c) for a named cluster, (d)); or reject the reopen and keep the recorded decision. — owner.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- AI, Lua, Grid and P4 cluster moves — no-action unless the stop condition is met; each would need its own plan and, first, a leaf.
- Path-qualified include policy (the existing `Json/`, `Interfaces/`, `Types/`, `Vcs/`, `Lua/` drift from ADR-0011) — no-action here; a separate evidence-driven plan if wanted.
- Header moves for the cache cluster — blocked by the back-edges; would need the Tracker schema types relocated first.

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
