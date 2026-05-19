# Evaluate current agents — skill conversion candidates

## Context

Smatchet ships **24 agents** (`agents/*.md`) and **3 skills** (`grill-with-docs`, `scratchpad-recall`, `agent-tokens`). No documented decision framework exists for choosing agent vs skill — the only explicit choice in the tree is `DELEGATION.md:128` flagging `grill-with-docs` as "skill, not agent" without rationale. User requested an evaluation of which agents would convert better to skills, with a ranked conversion plan presented in both conservative and aggressive views.

The motivation: skills load deterministic procedures inline on the main thread (no separate context window, no return-summary round-trip); agents run in isolation (own context, parallel dispatch, multi-iteration loops). Several existing agents are small rule-sheets / mechanical procedures that pay the agent-overhead cost (banner + telemetry + isolated context spawn) without using its benefits.

## Proposed decision criteria

Codify in `docs/agent-rules/` (separate work — not part of this eval, but the criteria below should land alongside conversions):

**Skill when ALL hold:**
- Deterministic procedure or rule-sheet (no investigation loop).
- Bounded context footprint (<3 file reads, <2 file edits typical).
- No multi-round user-feedback or PR-comment loop.
- No parallel-dispatchable subtask spawn.
- Output is direct action / inline result, not a return-summary the main thread synthesizes.

**Agent when ANY hold:**
- Heavy exploration / context bloat risk (multi-file investigation, blame, callgraph walks).
- Multi-round loop (hypothesis → instrument → measure → re-hypothesize).
- Isolated worktree / spawned-child orchestration.
- Parallel-dispatchable (orchestrator wants to fan out 3+ at once).
- Return-summary value > inline value (main thread benefits from compressed report).

## Conservative core list — clear wins (5)

Strong skill conversions. Low risk, immediate context-window savings on the main thread, no functional loss.

| # | Agent | Action | Effort | Risk | Rationale |
|---|---|---|---|---|---|
| 1 | `perf-measure` | Convert to skill `smatchet:perf-measure` | S (~1 hr) | None | Read-only CLI wrapper + JSON top-N parse. ~80 lines of prose, deterministic. `read-only: true` in frontmatter. No exploration, no multi-iteration. |
| 2 | `perf-instrument` | Convert to skill `smatchet:perf-instrument` | S (~1 hr) | None | Mechanical marker insert/strip per spec. Already `complexity: low`. Rules + workflow are exhaustive — zero judgement calls. |
| 3 | `agent-tokens` (agent file) | Delete `agents/agent-tokens.md`; skill already exists at `agents/_shared/token-tracking/` | XS (~10 min) | None | Already a skill. Agent file is duplicate documentation; deletion removes the routing ambiguity. |
| 4 | `code-review` (wrap) | Replace with thin skill `smatchet:code-review` that invokes built-in `/review` + adds Smatchet invariant checklist | M (~2 hr) | Low — must preserve dual-target sniff + UI-thread blocking-call sniff | Already explicitly "wraps the harness's standard pre-merge review skill (e.g. Claude Code's `/review`)" per its own prose. The wrap is the natural skill shape. |
| 5 | `security-review` (wrap) | Replace with thin skill `smatchet:security-review` that invokes built-in `/security-review` + Smatchet attack-surface map | M (~2 hr) | Low — must preserve MCP / Lua sandbox / coding-handoff attack-surface coverage | Same pattern as #4 — already wraps `/security-review`. |

**Why these are clear wins**: each one's *primary value* is a fixed procedure or a rule-sheet. None benefit from context isolation. Items #4 and #5 actively duplicate built-in skill plumbing today.

## Aggressive extended list — viable with caveats (+9)

Defensible conversions if accepting moderate main-context bloat or subsystem-specific main-thread loading. Skill load is on-demand, so most users won't pay the cost unless they touch the subsystem.

| # | Agent | Action | Effort | Risk | Caveat |
|---|---|---|---|---|---|
| 6 | `mechanic` | Skill `smatchet:mechanic` | S | Low-Med | Multi-file rename can balloon context if rename hits 30+ files. Mitigation: skill instructs main thread to do per-file batch reads, not all-at-once. |
| 7 | `test-author` | Skill `smatchet:test-author` | M | Low | Classification + bucket-mapping procedure. No edits typically — emits a bucket-grouped manifest. Pure skill shape. |
| 8 | `git-janitor` | Split — skill `smatchet:merge-this-pr` (single-PR squash + cleanup); keep agent for full session cleanup | M | Med — destructive ops checklist must remain intact | End-of-session cleanup of N PRs in dependency order keeps benefitting from isolation; single-PR merge-gate-poll → squash-merge → branch-delete is a clean inline skill. |
| 9 | `test-rig` | Skill `smatchet:test-rig` | M | Low | doctest + CTest scaffolding is rule-sheet. Multi-file edits stay small (one `.test.cpp` + tests/CMakeLists.txt). |
| 10 | `command-system` | Skill `smatchet:command-system` | M | Med | Subsystem rule-sheet — registry + palette + MCP + Lua + scenarios. Touches 4-6 files per command but each touch is small + scripted. |
| 11 | `lua-binder` | Skill `smatchet:lua-binder` | M | Med | sol2 bindings + stubs parity invariant is rule-bound. Risk: parity violations are silent (build still passes); main-thread loading the procedure is fine but loses the focused-context advantage. |
| 12 | `mcp-toolsmith` | Skill `smatchet:mcp-toolsmith` | M | Low | Schema design + endpoint registration procedure. Bounded scope. |
| 13 | `unreal-bridge` | Skill `smatchet:unreal-bridge` | M | Med | Dual-target gating rules + DX12 packaging. Multi-file but mechanical. Risk: DX12 build failure diagnosis is investigation-shaped and would still benefit from agent isolation. Compromise: skill for the gating rules, escalate to a thinner `dx12-debug` agent for actual failures. |
| 14 | `coderabbit-triage` | Skill `smatchet:coderabbit-triage` | M | Med | Classification + routing. Heavy `gh api` I/O — depending on PR comment count, can balloon main context. Mitigation: skill can still spawn a per-finding sub-agent for the heavy ones; main thread holds the classifier table only. |

**Why these are aggressive, not core**: each one has at least one of {multi-file edit footprint, subsystem-specific load that's wasted when not touching that subsystem, or partial benefit from isolation that the skill conversion gives up}.

**Explicitly not in the aggressive list — subsystem specialists declined**: `grid-engine`, `tracker-backend`, `offline-sync`, `p4-blame`. Each is a subsystem rule-sheet that *could* be a skill, but they routinely do multi-file edits across 4-8 files (grid columns, Jira field parsers, SQLite cache schema, p4 annotate flow) and benefit from agent isolation more than the ones in the list above. Re-evaluate per subsystem if main-thread context usage proves fine on items #10–13 first.

## Keep-as-agents — context isolation is load-bearing (9)

| Agent | Why it stays |
|---|---|
| `architect` | Cross-cutting design over Source_Core + Plugins + UnrealPlugins. Heavy file-read sweep; return-summary compression is the value. |
| `debug-detective` | Hypothesis-instrument-measure-rehypothesize loop. Multi-iteration. Spawns build + run + log-read cycles per turn. Pause-loop override of ship-loop is documented in AGENTS.md. |
| `perf-detective` | Same iteration shape as `debug-detective`. Delegates to `perf-instrument` + `perf-measure` (which become skills, but the orchestrator stays an agent). |
| `spike-hunter` | p99 outlier hunting. Multi-scenario fan-out. Long-running. |
| `pr-iterator` | Reads N PR comments, classifies, makes edits, commits, pushes. Multi-round. Spawned-child contract. |
| `handoff-implementer` | Spawned-child orchestrator inside isolated worktree. Full diagnose → code → test → commit → push → PR loop. Cannot be inline. |
| `build-doctor` | Investigation loop on cmake / preset / toolchain failures. Context-heavy log reads + multi-build cycles. |
| `code-review` | (Only if #4 deferred.) Heavy multi-file diff read + linter output parsing. |
| `security-review` | (Only if #5 deferred.) Adversarial reasoning over MCP / CLI / Lua / p4 / HTTP attack surfaces. Heavy file-read sweep. |

## Conversion mechanics (per item)

For each item promoted from agent → skill:

1. Author `SKILL.md` at `agents/_shared/skills/<name>/SKILL.md` (or `~/.claude/skills/<name>/` if intentionally user-global). Frontmatter: `name`, `description`, `triggers`. Body: the procedure, lifted near-verbatim from the agent's prose.
2. Update `docs/agent-rules/DELEGATION.md` — move row from agent table to skill table; update the trigger-keyword routing.
3. Delete `agents/<name>.md`. Run `bash scripts/setup-harness.sh claude-code` to regenerate harness adapter symlinks.
4. Edit any agent prose that delegates to the old agent — replace "delegate to `<name>` agent" with "invoke `<name>` skill".
5. Drop frontmatter that doesn't translate (banner / outcome telemetry — these are agent-specific; skills don't need them).
6. Smoke-test: trigger the skill from a fresh session via its keyword + verify behavior matches the old agent.

## Critical files (referenced by this evaluation)

- `agents/perf-instrument.md`, `agents/perf-measure.md`, `agents/mechanic.md`, `agents/agent-tokens.md` — direct conversion targets.
- `agents/code-review.md`, `agents/security-review.md` — wrap conversions.
- `docs/agent-rules/DELEGATION.md` — routing table that must be updated per conversion.
- `agents/_shared/skills/` — destination for shared skill manifests.
- `scripts/setup-harness.sh` — adapter regeneration after agent file deletion.
- `AGENTS.md` § Harness adapter — capability-tag → skill mapping section (may need updates for new skill manifests).

## Verification

For each converted item:

1. **Functional parity** — invoke the new skill against a known input (e.g. for `perf-measure`: a registered scenario; for `perf-instrument`: a 3-tuple insert spec). Confirm output shape matches what the agent produced.
2. **Trigger routing** — issue a user prompt with one of the agent's old trigger keywords. Confirm harness invokes the new skill, not the (now-deleted) agent file.
3. **DELEGATION.md cross-link** — `rg '<old-agent-name>'` over `agents/*.md` returns zero non-archived references. Stale references break the agent ecosystem.
4. **No regression on dependent agents** — `perf-detective` + `spike-hunter` still hand off correctly when `perf-instrument` / `perf-measure` become skills. Verify by running a perf round.
5. **Token cost delta** — for converted items, measure the main-thread input-token cost on a representative task before + after. Confirm net savings (skill load < agent spawn overhead on average).

## Recommended rollout order

**Phase 1 (zero risk)**:
- Item #3 (delete duplicate `agent-tokens.md`) — pure cleanup, no behavior change.
- Item #1 (`perf-measure` → skill) — read-only, smallest scope.
- Item #2 (`perf-instrument` → skill) — small mechanical procedure.

**Phase 2 (wrap built-ins)**:
- Item #4 (`code-review` thin-wrap `/review`).
- Item #5 (`security-review` thin-wrap `/security-review`).

**Phase 3 (extended, gated on Phase 1+2 working)**:
- Items #6–#14 evaluated individually. If main-context bloat from Phase 2 conversions is acceptable, proceed with #7 (`test-author`) and #9 (`test-rig`) next — both are pure procedure + bounded edits. Defer subsystem specialists (#10–#13) until at least one of them is requested in actual work — premature conversion is wasted churn.

**Out of scope for this eval**: actual implementation. This plan stops at the recommendation. Each phase landing is a separate PR.
