# Agent self-improvement — process

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-08-20 · orchestrator · [process] · P2 — UI feature sourced from a runtime-populated cache shipped non-functional in its headline flow (#2148 → #2149): trace the cache's producers (incl. empty/error paths), prefer interaction-time data capture, and never merge past an outstanding visual-validation pause on a bare "merge"
  Details: The #2148 `reads as` echo named accountIds only via `GetAvailableUsers()` + session search results; the catalog's user list is legitimately empty (warning-only fetch failure, cache-restored catalog), so the headline flow (select user → see name) rendered nothing. Green everywhere: pure tests stocked the catalog by construction, bucket-C/E goldens don't cover the query editor. The autocomplete row the user clicked carried both id and name the whole time — capture at the interaction point needed no cache and no network (what #2149 does). The visual-validation exception (AGENTS.md pause 5) was flagged open on every status update yet a bare "merge" executed without restating it.
  Concrete next action: (1) one sentence in `docs/agent-rules/ship-loops.md` § Visual-validation exception: a pending visual verdict survives a bare "merge" — restate the unverified behavior and require an explicit merge-anyway. (2) review-checklist line (code-review agent prompt or `docs/agent-rules/cpp-rules.md` § Quality): UI output read from an app-owned cache ⇒ PR body enumerates the cache's producers + the empty-state behavior; "renders nothing" is a finding unless intended. (3) promote the postmortem's eval case (postmortems.md 2026-08-20) into the subagent-eval suite.
  Status: open
  Last-reviewed: 2026-08-20

- 2026-06-07 · orchestrator · [process] · P2 — gate/test output must be grepped for FAIL, never `tail`-truncated: a truncated tail hid a red doc-validation sub-test and shipped it to a PR (cost one full CI round-trip on #946)
  Details: The orchestrator ran `test-docs.sh ... | tail -2`, saw the LAST sub-test's "Passed: 1 Failed: 0" + an unrecognized "Fix locally (many auto-fix)" hint line, and committed — but two earlier sub-tests (plan-index drift + a dangling tier-ful plan ref from a `git mv` active→shipped) were red. CI caught it ("Auto-sync plan INDEX" + "Doc anchors + agent contract" both failed on #946). Same mistake pattern nearly recurred minutes later on the fix commit itself. The summary line `test-docs — Passed: N Failed: M` exists and is grep-able; the failure mode is purely how the output was consumed.
  Concrete next action: (1) discipline line in `docs/agent-rules/process-rules.md` § Cadence + verification: consume gate output via `grep -E "FAIL|Failed: [1-9]"` or the suite's summary line, never bare `tail -N`; (2) optional hardening: make `test-docs.sh` exit-code-first in orchestrator recipes (`bash scripts/dev/test-docs.sh && git commit ...`) so truncated reading can't matter. Also fold in the PowerShell commit-message note from Slice 1c: messages containing double quotes break `git commit -m` under PowerShell 5.1 native-arg re-quoting — use `git commit -F <file>` for multiline/quoted messages.
  Status: partially applied (2026-06-20 trap-sweep — shipped: (1) process-rules.md:123 "gate output consumed by verdict, grep -E FAIL, never tail"; remaining: the PowerShell commit-F / double-quote / PS-5.1 note is absent)
  Last-reviewed: 2026-06-20

- 2026-06-03 · orchestrator · [process] · P2 — a doc-file RENAME silently orphans the `test-portable-purity` baseline (keyed on filename); a `md_lint`+`doc-anchors`+`markdown-links` local pass is NOT enough for renames
  Details: `dry-pillar-dup-gate` Slice 3 `git mv docs/agent-rules/ux-pillars.md → quality-pillars.md`. Locally I ran `md_lint --all`, `test-doc-anchors`, and `test-markdown-links` (all green) and pushed. CI's "Doc anchors + agent contract" job then went **RED** on `test-portable-purity`: the baseline (`docs/high-integrity/portable-purity-baseline.txt`) keys grandfathered literals by **filename**, so the renamed file's 4 pre-existing literals (`SMATCHET` / `Smatchet` / `Source/Core` / `ninja-test-msvc`, all carried over verbatim from `ux-pillars.md`) were seen as NEW leaks. The fix was `bash agents/scripts/core/test-portable-purity.sh --refresh` (rename the baseline entries). Separately CodeRabbit flagged one of those literals on the new intro line — a real redundancy I removed. Both were avoidable: `test-portable-purity` (and the rest of the `test-docs.sh` suite — `test-plan-index`, `test-plan-ref-integrity`, `test-agent-contract`) are NOT in `md_lint`/`doc-anchors`/`markdown-links`, so a "local docs verification" that runs only those three misses portable-purity, plan-index, ref-integrity, and agent-contract entirely. Caught only because "Doc anchors + agent contract" is now a required check (good — the #793 structural gate did its job).
  Concrete next action: (1) any doc-file **rename/move** (especially under the portable dirs `agents/{core,_shared}` / `docs/agent-rules` / `docs/harness`) MUST run `bash agents/scripts/core/test-portable-purity.sh` (and `--refresh` if it's a legit rename) before push, plus the full `test-docs.sh` suite — not just `md_lint`. (2) Better: add a `scripts/dev/pre-ship.sh` step (or a `--docs` mode) that runs the WHOLE doc-validation suite (`test-portable-purity` / `test-plan-index` / `test-plan-ref-integrity` / `test-plan-naming` / `test-agent-contract` / `test-doc-anchors` / `test-markdown-links` / `md_lint`) so a single pre-push command mirrors the required CI job — the orchestrator currently cherry-picks 2-3 of these by hand and misses the rest. (3) Consider keying the portable-purity baseline by content-hash or auto-following `git mv` so a pure rename doesn't orphan entries. ~30 min for (2).
  Status: partially applied (2026-06-20 trap-sweep — shipped: (2) pre-ship.sh runs test-docs.sh enrolling all doc gates incl portable-purity; remaining: (3) content-hash/git-mv-following baseline is absent; (1) the rename-specific rule is not separately codified)
  Last-reviewed: 2026-06-20

- 2026-05-17 · code-review · [process] · P3 — PR #146 `Source/Core/src/Commands/Scenarios/*.cpp` manual `extern UiDrawSession g_ui;` duplicated across files
  Details: Duplicated across `CommandPaletteFuzzyScenario.cpp` + `DockGapSentinelScenario.cpp` + `BuiltinCommands_Debug.cpp`.
  Concrete next action: promote to an unconditional `extern` in `SmatchetUiSession.h`. Surfaced by retrospective code-review sweep on PR #146.
  Status: partially applied (2026-06-20 trap-sweep — shipped: central extern (SmatchetUiSession.h:938 `extern UiDrawSession g_ui;`); remaining: the per-file duplicate extern declarations are NOT removed (still in CommandPaletteFuzzyScenario.cpp, DockGapSentinelScenario.cpp, AiChatHistoryRenderScenario.cpp, +6 more) — the dedup goal is unmet)
  Last-reviewed: 2026-06-20

- 2026-05-16 · test-rig + orchestrator · [process] · P3 — Mutation-sanity recipe in test-rig packets needs taxonomy: prod-mutation vs test-mutation
  Details: callstack-adversarial-subcases run (PR #112) hit the auto-mode classifier denying two mutation-sanity recipe steps: (a) production-side substring-prefix relaxation in `ApplyPathRemaps` (legit denial — production was strictly out-of-scope per the packet), (b) test-side fixture mutation that would have removed a load-bearing invariant from a high-risk case (also legit). The current `test-rig` packet language ("one production-side mutation per high-risk case, demonstrably fails the new test, reverted before commit") assumes both options open. In practice, when production code is `Out of scope — refuse if asked`, every prod-side mutation is denied by the classifier. Agent has to argue-from-assertion-shape for 1/4 of the cases and document deferred-with-rationale.
  Concrete next action: split the recipe into (1) production-side mutation when production is in the write set, demonstrably fails, revert; (2) production-side mutation **deferred** when production is out-of-scope — instead, argue from assertion shape + neighbour-test coverage that the production branch is reachable; (3) test-side fixture mutation only when it does NOT remove a load-bearing invariant. Land in `agents/core/test-rig.md` § Mutation-sanity recipe + AGENTS.md § Orchestrator delegation packet.
  Re-scoped 2026-06-02: the "mutation-sanity recipe" this entry refines no longer exists as a documented section — a grep of `docs/agent-rules/`, `agents/core/test-rig.md`, and AGENTS.md finds no `mutation-sanity` text; it was ephemeral packet language. Applying the 3-case taxonomy now would author a brand-new, currently-unreferenced section rather than refine existing guidance. Keep open but re-scope to: "decide whether a documented mutation-sanity recipe is still wanted; if yes, author it WITH the prod-in-scope / prod-deferred / test-side taxonomy baked in from the start."
  Status: open
  Last-reviewed: 2026-06-02

- 2026-05-14 · architect · [process] · P3 · DEFERRED — `TodoWrite` reminder noise during read-only tasks
  Details: System injected three `TodoWrite` reminders into a read-only validation run. Read-only agents (architect, code-review, security-review, perf-measure) rarely benefit from a todo list; the reminder hook could be muted for them based on the agent banner or `tools:` frontmatter (no `Write`/`Edit`).
  Concrete next action: harness-side (Claude Code injects the reminder unconditionally; not configurable via project settings.json). Re-open once a Claude Code release exposes a per-agent toggle, or once a second harness cites the same noise.
  Status: deferred
  Last-reviewed: 2026-05-17

- 2026-05-12 · tracker-backend · [process] · P3 · DEFERRED — `RemoteProject` POD uses lowerCamelCase (`id`, `key`, `displayName`) while most other DTOs use PascalCase
  Details: Style drift introduced in PR 1. Worth normalizing before more call sites accumulate. Architect call.
  Concrete next action: C++ rename touching every `RemoteProject` call site (tracker-backend + grid-engine + bulk-import). Architect should scope the rename inside the next PR that legitimately touches `RemoteProject`. Don't open a standalone rename PR — bundle with adjacent work to minimise diff noise.
  Status: deferred
  Last-reviewed: 2026-05-17

- 2026-05-12 · offline-sync · [process] · P3 · DEFERRED — `SaveFieldCatalogSnapshot` accumulated 4 extra primitive args; a `FieldCatalogSaveContext` struct would prevent future drift
  Details: callers already had each arg in scope; bundling them into one struct keeps the call site narrow as more per-axis state lands.
  Concrete next action: small C++ refactor — bundle into the next PR that touches `SaveFieldCatalogSnapshot`. Don't open a standalone refactor PR; the win shows up only when adding the next per-axis arg, which is when the bundling decision gets reviewed in context.
  Status: deferred
  Last-reviewed: 2026-05-17
