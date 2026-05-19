# Evaluate current agents — skill conversion candidates (revised, conservative-only)

## Context

Smatchet ships **24 agents** (`agents/*.md`) and **3 skills** (`grill-with-docs`, `scratchpad-recall`, `token-tracking`). No documented decision framework for choosing agent vs skill — the only explicit choice in the tree is `docs/agent-rules/DELEGATION.md:128` flagging `grill-with-docs` as "skill, not agent" without rationale.

This document was revised after a fact-check pass. The original v1 plan included 5 conservative candidates; v2 reduces to **2** after the following corrections:

- **v1 item #3 dropped** — claimed `agents/agent-tokens.md` was a duplicate to delete. **No such file exists.** The Explorer agent that produced the v1 inventory hallucinated it. `agent-tokens` is a skill only (`agents/_shared/token-tracking/SKILL.md`); there has never been an agent counterpart.
- **v1 items #4 + #5 dropped** — claimed `code-review` and `security-review` were thin wraps of built-in `/review` and `/security-review` and could collapse into skill-wrappers. Their prose does say "wraps the harness's standard pre-merge review skill" — but the wrap is **rhetorical, not structural**. Both agents are heavy investigators (opus/high or sonnet/high model, 140-line Smatchet-specific checklists, multi-file diff reads, parallel sub-tool dispatch via `vexp` + `cppcheck` + `clang-tidy` + `flawfinder` + `semgrep`, delegation to `spike-hunter` / `perf-detective`). Converting them to skills would force the main thread to absorb that load + delegation, losing the isolation that's the whole point.

User authorized: conservative list only, double-check everything.

## Verified facts (v2 fact-check pass)

- **Agent count**: 24. Verified by `Glob agents/*.md` (excluding `README.md`).
- **Agent list**: `architect`, `build-doctor`, `code-review`, `coderabbit-triage`, `command-system`, `debug-detective`, `git-janitor`, `grid-engine`, `handoff-implementer`, `lua-binder`, `mcp-toolsmith`, `mechanic`, `offline-sync`, `p4-blame`, `perf-detective`, `perf-instrument`, `perf-measure`, `pr-iterator`, `security-review`, `spike-hunter`, `test-author`, `test-rig`, `tracker-backend`, `unreal-bridge`.
- **Skill count**: 3 SKILL.md manifests. Locations:
  - `agents/_shared/skills/grill-with-docs/SKILL.md`
  - `agents/_shared/skills/scratchpad-recall/SKILL.md`
  - `agents/_shared/token-tracking/SKILL.md` ← **inconsistent path** (not under `skills/`)
- **`perf-measure.md` frontmatter**: `complexity: low`, `read-only: true`, `model: sonnet`, `effort: low`. Body is a CLI + JSON parse procedure with explicit halt-on-CLI-gap path. No multi-iteration loop.
- **`perf-instrument.md` frontmatter**: `complexity: low`, `read-only: false`, `model: haiku`, `effort: low`. Body is rules-bound mechanical insert/strip per spec. No judgement calls.
- **`code-review.md`** says wraps `/review` (line 3) but is `complexity: medium`, `sonnet/high`, delegates to `spike-hunter` + `perf-detective`, 140-line subsystem invariant checklist. Structurally heavy.
- **`security-review.md`** says wraps `/security-review` (line 3) but is `complexity: high`, `opus/high`, 140-line attack-surface map, runs `flawfinder` + `semgrep` + `gitleaks` + `cppcheck`. Structurally heavy.
- **`scripts/setup-harness.sh`**: exists. Generates harness adapters.
- **Built-in `/review` + `/security-review` skills**: present in the Claude Code session skill list — confirmed at session start.

## Proposed decision criteria

Codify in a separate `docs/agent-rules/AGENT-VS-SKILL.md` (out of scope for this plan — landing the criteria is its own work). Until then, these are the working rules:

**Skill when ALL hold:**
- Deterministic procedure or rule-sheet (no investigation loop).
- Bounded context footprint (≤3 file reads, ≤2 file edits typical).
- No multi-round user-feedback or PR-comment loop.
- No parallel-dispatchable subtask spawn.
- Output is direct action / inline result, not a return-summary the main thread synthesizes.

**Agent when ANY hold:**
- Heavy exploration / context bloat risk (multi-file investigation, callgraph walks, blame).
- Multi-round loop (hypothesis → instrument → measure → re-hypothesize).
- Isolated worktree / spawned-child orchestration.
- Parallel-dispatchable (orchestrator fans out 3+ at once).
- Delegates to other agents (delegation chains belong in agent context, not main).

## Conservative core — clear wins (2)

| # | Agent | Action | Effort | Risk | Rationale |
|---|---|---|---|---|---|
| 1 | `perf-measure` | Convert to skill `smatchet:perf-measure` at `agents/_shared/skills/perf-measure/SKILL.md` | S (~1 hr) | None | Read-only CLI wrapper + JSON top-N parse. ~105 lines of prose, deterministic. `read-only: true` in frontmatter. No exploration, no multi-iteration. Explicit halt-on-CLI-gap path is rule-bound, not investigative. |
| 2 | `perf-instrument` | Convert to skill `smatchet:perf-instrument` at `agents/_shared/skills/perf-instrument/SKILL.md` | S (~1 hr) | None | Mechanical marker insert/strip per spec. ~70 lines of prose. `complexity: low`, `read-only: false`. Rules + workflow are exhaustive — zero judgement calls. Currently the only "agent" the orchestrator uses purely as a sub-step of `perf-detective` / `spike-hunter`. |

**Why these two and nothing else**: each one's *primary value* is a fixed procedure or a rule-sheet. Neither benefits from context isolation. Neither delegates to another agent. Both are already used as sub-steps of a heavier agent (`perf-detective` / `spike-hunter`), so the inline-skill shape matches how they actually run.

## Not converted — explicit rationale

To prevent the v1 mistake from recurring, list each rejected candidate with the reason it was rejected.

| Agent | Why rejected from conservative list |
|---|---|
| `code-review` | Heavy investigator: 140-line Smatchet checklist, runs cppcheck + clang-tidy + clang-format in parallel, delegates to `spike-hunter` / `perf-detective`. The "wraps /review" rhetoric is conceptual; structurally it consumes large context per PR. |
| `security-review` | Heavier than `code-review`: opus/high, 140-line attack-surface map, adversarial taint-tracing, runs flawfinder + semgrep + gitleaks + cppcheck. Pure agent shape. |
| `mechanic` | Multi-file rename can hit 30+ files; main-thread context would balloon. Conservative threshold rejects. |
| `test-author` / `test-rig` / `command-system` / `lua-binder` / `mcp-toolsmith` / `unreal-bridge` / `coderabbit-triage` / `git-janitor` | All viable under aggressive threshold but each has at least one of {multi-file edit footprint, subsystem-specific load, partial isolation benefit} that disqualifies them from "clear win" status. Re-evaluate after items #1–#2 land + reveal real main-thread cost. |
| `grid-engine` / `tracker-backend` / `offline-sync` / `p4-blame` | Subsystem specialists with multi-file edit footprints (4-8 files typical). Retain meaningful isolation benefit. |
| `architect` / `debug-detective` / `perf-detective` / `spike-hunter` / `pr-iterator` / `handoff-implementer` / `build-doctor` | All have multi-iteration loops, isolated context need, or spawned-child orchestration. Hard agent-only. |
| `agents/agent-tokens.md` (v1 claim) | **Does not exist.** `agent-tokens` is a skill only. Nothing to delete. |

## Critical files (touched by this conversion)

- `agents/perf-instrument.md` → delete after conversion
- `agents/perf-measure.md` → delete after conversion
- `agents/_shared/skills/perf-instrument/SKILL.md` → new
- `agents/_shared/skills/perf-measure/SKILL.md` → new
- `agents/perf-detective.md` → edit: replace "delegate to `perf-instrument` agent" / "delegate to `perf-measure` agent" prose with "invoke `perf-instrument` skill" / "invoke `perf-measure` skill"
- `agents/spike-hunter.md` → same edit
- `docs/agent-rules/DELEGATION.md` → move two rows from agent table to skill table; update trigger-keyword routing for `instrument` / `perf-scope` / `perf-marker` / `perf-cleanup` / `measure` / `snapshot` / `scenario` / `perf-run`
- `AGENTS.md` § Harness adapter → no changes expected (capability-tag table is generic)
- `scripts/setup-harness.sh` → run after deletions to regenerate adapter symlinks. **Verify first** whether the script knows how to install skills as well as agents — may need extension.

## Conversion mechanics (per item)

1. **Author SKILL.md** at `agents/_shared/skills/<name>/SKILL.md`. Frontmatter: `name`, `description`, `triggers`. Body: the procedure, lifted near-verbatim from the agent's prose.
2. **Drop agent-specific telemetry** that doesn't translate to skill: banner lines, `## Outcome` line, `## Self-improvement` block. Skills don't have the per-invocation telemetry contract that agents do.
3. **Delete `agents/<name>.md`**.
4. **Edit dependent agents** (`perf-detective.md`, `spike-hunter.md`) to invoke skill instead of delegating to agent.
5. **Update `docs/agent-rules/DELEGATION.md`** routing tables.
6. **Run `bash scripts/setup-harness.sh claude-code`** to regenerate adapter junctions. Verify the script handles skills correctly first; extend it if it only knows about agents today.
7. **Smoke-test**: trigger the skill from a fresh session via one of its keywords + verify behavior matches the old agent (see Verification below).
8. **Commit per item** — one PR per skill conversion so a regression bisect is single-skill-scoped.

## Verification

Per converted item:

1. **Functional parity** — invoke the skill against a known input:
   - `perf-measure`: pick a registered scenario (e.g. `grid_open_close`), run a 60-frame snapshot, confirm top-5 rows printed sorted by `lastTotalMs`. Compare against the agent's last known output for the same scenario.
   - `perf-instrument`: feed a 3-tuple spec (`file, function, scope-name`), confirm `SMATCHET_UI_PERF_SCOPE("perf_temp:<name>");` inserted at line 1 of function body, include added if missing, build passes.
2. **Trigger routing** — issue a user prompt with one of the agent's old trigger keywords (`measure scenario X`, `instrument Y for perf`). Confirm harness invokes the new skill, not the deleted agent file.
3. **DELEGATION.md cross-link** — `rg 'perf-instrument' agents/` returns matches only where the prose now says "skill", not "agent". Same for `perf-measure`.
4. **No regression on dependent agents** — run `perf-detective` against a known hot path; confirm it correctly invokes `perf-instrument` skill + `perf-measure` skill in sequence + uses their results.
5. **Token cost delta** — measure main-thread input-token cost on a representative perf-round before + after. Expect net savings (one skill-load < two agent-spawn round-trips on average).
6. **Skill location consistency** — flag `agents/_shared/token-tracking/SKILL.md` (current odd-one-out path) for a follow-up move to `agents/_shared/skills/token-tracking/SKILL.md`. **Not in scope for this conversion** — separate cleanup PR. Adding two new skills under the correct path strengthens the case.

## Rollout

**Phase 1** — `perf-measure` conversion (read-only, lowest risk). Single PR.
**Phase 2** — `perf-instrument` conversion. Single PR. Gated on Phase 1 functional-parity verification.

No Phase 3. If main-thread context cost from Phase 1+2 is acceptable, a follow-up evaluation can consider the aggressive list — but that's a new plan, not an extension of this one.

## Out of scope

- Implementation of the conversions (this doc is recommendation only).
- Skill-location normalization for `token-tracking`.
- Authoring `docs/agent-rules/AGENT-VS-SKILL.md` (criteria doc).
- Any agent in the rejected list.
