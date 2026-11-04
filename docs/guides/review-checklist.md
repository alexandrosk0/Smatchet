<!-- tier: project -->
# Smatchet — code-review checklist

**When to read this file**: you are reviewing a Smatchet diff, a merged PR or a subsystem for bugs: the `code-review` subagent before a commit or push, an inline review with the `adversarial-code-review` skill, a review-panel leg, a pre-merge or historical review workflow. The `code-review` subagent finds this file through `project.config.json` → `docs.review_checklist`.

It holds the Smatchet rules a reviewer applies on top of the generic method in the `adversarial-code-review` skill. Two other sources are criteria too and are not repeated here: the every-edit invariants and enforcement contract-card in `AGENTS.md` § Project rules (C++14, `LOG_*`, RAII, no empty `catch (...)`, no GLFW in core headers, …), and the `## Invariants` of the leaf `AGENTS.md` for every directory the diff touches (registry: [`CONTEXT-MAP.md`](../../CONTEXT-MAP.md)). A leaf overrides any summary, including this file.

## Severity in this project

These classes have a fixed severity. Everything else follows the reviewer's severity guide (Critical / High / Medium / Low).

| Finding | Severity |
|---|---|
| Sync I/O reachable from any ImGui render path (Pillar 2) | Critical |
| A verification step that needs human eyes ("user opens X and observes Y", "visually verify") in the diff's test plan, PR body or linked plan `## Verification`, unless it is an explicit bucket-E follow-up already tracked as a `docs/self-improvement/categories/` entry (§ Verification residue) | Critical |
| A DRY refactor whose shared helper couples two otherwise-independent subsystems (Pillar 5) | Critical |
| Offline: a loading-only render while cached data exists; a write lost offline or sent to another pane's tracker; a `TrackerError` flattened without its `Transport` kind (Pillar 6) | High |
| New UI output that reads an app-owned cache or catalog and shows nothing (no text, no cue) when that cache is legitimately empty: after a warning-only fetch failure (never waived: the Ui leaf requires its unknown/unavailable cue), or when the cache was restored without the list, unless the diff or its stated intent (PR body, commit message, linked plan) names that empty state as intended (§ Cache-sourced UI output) | High |
| A network-backed change with no offline test (Pillar 6) | Medium |

**`hard_veto`** (verifier verdicts, `review-ack.sh --verdict`, the pre-merge judge): set `hard_veto=true` for a security issue, a deterministic-gate failure or a project-invariant breach — that, and only that, blocks the commit or push; the continuous score stays advisory until calibrated. Do not set a veto you do not believe, and do not withhold one you do: the veto is trusted precisely because it costs you something to report.

## Change-shape rules

The generic change-shape lenses — fix-scope integrity ("what did this diff take away?"), sibling consistency, and execution order set by the scheduler rather than by lexical position — are in the `adversarial-code-review` skill, Step 4. The Smatchet specifics they apply to:

**Execution-ordering claims.** Order here is set by the `MainThreadDispatcher` drain, the ImGui frame phase a call sits in, callback registration and static-init order. Before raising "A runs before B", trace the frame-phase or scheduler entry point and grep the suspect symbols for an intervening `Post*` / `Dispatch` / `Enqueue` / `OnTick` hop that re-sequences them across a frame boundary.

**Cache-sourced UI output.** When new UI output reads an app-owned cache or catalog (users, options, projects, …), the stated intent (PR body, commit message or linked plan) must name that cache's producers and what the UI shows when it is legitimately empty (a warning-only fetch failure, a cache restored without the list). Tests that stock the cache by construction do not cover the empty case. Prefer capturing the data at the interaction point (the row the user clicked already carries it) over a later cache lookup.

**ImGui ids, dock nodes and handles: existence is not validity.** Ids are hashes — recycled across sessions and, for dock nodes, persisted to `imgui.ini` — so a lookup that succeeds proves an object exists, not that it is the object you meant (a `DockBuilderGetNode(slot) != nullptr` guard passed for exactly the orphan ids it existed to reject, because they were reloaded from the ini). Require the structural relationship too (parent, root or owning container) and a comment naming which relationship the constant must satisfy. The same holds for `FindWindowByName` (it finds a stale window from an earlier layout) and for any `id -> object` map that outlives a session.

## C++ beyond the every-edit invariants

- No designated initializers (C++20), and nothing the in-repo `.clang-tidy` config bans.
- No `using namespace` in headers.
- `LOG_TRACE` / `LOG_DEBUG` in non-trivial branches, and no use of an object after it is moved from (`agent-layer/docs/agent-rules/cpp-rules.md` § Quality).
- Platform- and feature-specific code in `Source/Core/` is gated on `SMATCHET_EMBEDDED_IN_UNREAL` / `SMATCHET_WITH_MCP` / `SMATCHET_WITH_LUA_AUTOMATION`; binding ↔ stub parity and the gating rule itself are in `agent-layer/docs/agent-rules/cpp-rules.md` § Optional plugins.
- `*_DX12` CMake targets stay untouched unless the change explicitly asks for it.

## Offline-first (Pillar 6, ADR-0026)

The four blocking `offline-*` / `tracker-error-kind-collapsed` lint rules stop the known regressions; the three WARN heuristics are yours to confirm or dismiss. For each new or changed network-backed read or write, picture the tracker unreachable and apply the Ui and Sync leaf invariants, with the severities in the table above. The offline test the table asks for is bucket A with `GlobalFakeNetwork()`, or bucket E via `scripts/dev/test-ui-offline-first.sh`. The write seams are `FieldEditPipelineService::CommitOrQueue` (field edits) and `PendingActionQueueService::SubmitOrQueue` (comments, worklogs, watches); a toast must state the real outcome ("Queued offline" vs "Saved").

## UI thread (Pillar 2)

The full enumeration of blocking calls is in `Source/Core/src/Ui/AGENTS.md`; it applies to any code reachable from an ImGui render path, wherever the file lives. A hit is a Critical correctness finding, not a perf nit.

## DRY (Pillar 5, ADR-0015)

The reviewer is the reviewer-of-record for duplication findings and exemption sign-off. `dup_audit.py --diff` is blocking and stops a NEW clone on its own; the judgement the gate cannot make is yours: confirm a `[dup] FAIL` is real copy-paste (the gate already ignores mere structural similarity) and decide between de-duplication and an exemption. The gate fails closed until one of the two lands.

- An exemption is cheap and is the default for unrelated contexts: `SMATCHET_DEVIATION(rule=duplication; reason=…; owner=…; revisit=…)` on or above either occurrence.
- Standing-exempt classes: dual-target forward declarations, per-backend tracker client boilerplate, generated code.
- Over-abstraction that couples two subsystems is the opposite failure of small focused functions: flag it Critical, the same way as a missed invariant.

## Hot paths (Pillar 1)

Flag only when the change touches a known hot path — the grid, JQL, or per-frame ImGui code:

- Per-cell allocations (`std::string` building, map probes) inside `Render()` / `Display()`.
- Lua called per frame or per cell per frame. The cached cell-renderer path (`register_field_display_cached`, `scripts/SmatchetHooks.lua`) runs Lua about once per cell per refresh and is about 50-60× cheaper than per-frame Lua; a new binding on a per-frame path needs a reason.
- `std::regex` on a hot path.
- `std::map` where ordering does not matter (prefer `std::unordered_map`).

When perf risk is the dominant concern, measure instead of guessing: [`perf-workflow.md`](perf-workflow.md) § Steady state or spike? and the `perf-measure` skill; an intermittent-stall risk goes through § Spikes before merge.

## Verification residue

Scan the diff's test plan, PR body and linked plan `## Verification` for any item that reads "user opens X and observes Y", "click and check", "visually verify" or otherwise needs human eyes. Flag each as **Critical** under "Manual verification residue": the change is not mergeable until the item is automated with the `test-authoring` skill. Exception: an explicit bucket-E follow-up already tracked as a `docs/self-improvement/categories/` entry.

## Scripts, gates and docs

- A gate or test script that fails OPEN or false-PASSes (`passed=0 && failed=0` exits 0, an unanchored exclude token, a comment classifier that exempts real code) is High; one that lets bad code through a blocking gate is Critical.
- Correctness and data-loss bugs: a wrong external mutation, dropped or clobbered user edits, a migration key mismatch.
- Docs: broken cross-references, moved-script paths, stale line pins, doc-vs-code drift on a gated literal (allow-lists, ADR or plan status).

## Security

A diff that crosses a trust boundary also gets a security review against [`docs/security/attack-surface.md`](../security/attack-surface.md) (the app's surfaces, its crash classes and its security checklist). The `security-review` subagent finds that file through this section.

## Static analysis

Run in parallel over the changed files, capturing stderr. Skip vendored and generated paths: any `ThirdParty/` (`Source/Core/ThirdParty/`, `Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/`), `build/`, `.fetchcontent-src/`, `.fetchcontent-msvc/`, `*-build-dir/`.

```bash
cppcheck --enable=warning,style,performance,portability --inline-suppr --suppress=missingIncludeSystem --language=c++ --std=c++14 --quiet <changed .cpp/.h>
clang-tidy -p build/ninja-iter-clang <changed .cpp>        # tests/: build/ninja-test-clang; no clang dir: the -msvc twin
clang-tidy <changed .cpp> -- -std=c++14 -ISource/Core/include   # only when no compile_commands.json exists
clang-format --dry-run --Werror <changed .cpp/.h>
python3 agent-layer/agents/scripts/core/dup_audit.py --diff origin/develop
python3 agent-layer/agents/scripts/core/function_size_audit.py --diff origin/develop
```

The last two are the DRY and function-size gates: blocking in CI, missed by the three analyzers above, read-only and seconds-fast (the full `test-lint-rules.sh --diff` takes 10+ minutes). Report each new clone or cap breach as a finding. In the agent-layer repo itself, drop the `agent-layer/` prefix.
