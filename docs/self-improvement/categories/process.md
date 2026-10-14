# Agent self-improvement — process

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../../../agent-layer/docs/self-improvement/AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-06-07 · orchestrator · [process] · P2 — gate/test output must be grepped for FAIL, never `tail`-truncated: a truncated tail hid a red doc-validation sub-test and shipped it to a PR (cost one full CI round-trip on #946)
  Details: The orchestrator ran `test-docs.sh ... | tail -2`, saw the LAST sub-test's "Passed: 1 Failed: 0" + an unrecognized "Fix locally (many auto-fix)" hint line, and committed — but two earlier sub-tests (plan-index drift + a dangling tier-ful plan ref from a `git mv` active→shipped) were red. CI caught it ("Auto-sync plan INDEX" + "Doc anchors + agent contract" both failed on #946). Same mistake pattern nearly recurred minutes later on the fix commit itself. The summary line `test-docs — Passed: N Failed: M` exists and is grep-able; the failure mode is purely how the output was consumed.
  Concrete next action: (1) discipline line in `docs/agent-rules/process-rules.md` § Cadence + verification: consume gate output via `grep -E "FAIL|Failed: [1-9]"` or the suite's summary line, never bare `tail -N`; (2) optional hardening: make `test-docs.sh` exit-code-first in orchestrator recipes (`bash scripts/dev/test-docs.sh && git commit ...`) so truncated reading can't matter. Also fold in the PowerShell commit-message note from Slice 1c: messages containing double quotes break `git commit -m` under PowerShell 5.1 native-arg re-quoting — use `git commit -F <file>` for multiline/quoted messages.
  Status: partially applied (2026-06-20 trap-sweep — shipped: (1) process-rules.md:123 "gate output consumed by verdict, grep -E FAIL, never tail"; remaining: the PowerShell commit-F / double-quote / PS-5.1 note is absent)
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
