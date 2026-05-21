# Plan — Agentic ripout doc cleanup (v2 follow-up to `github-tracker-backend`)

> **Slug**: `agentic-ripout-doc-cleanup-v2`
>
> **Status**: STUB. Lands after [`github-tracker-backend`](github-tracker-backend.md) PR1 (C++ ripout) merges. Pre-implementation scope sketch only.
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location.

## Context

The v1 plan ([`github-tracker-backend.md`](github-tracker-backend.md)) explicitly accepts **bit-rot in docs** as a known cost — `AGENTS.md` sections, `agents/*.md` agent files, `docs/agentic/`, `docs/agent-rules/DELEGATION.md`, `docs/design/agentic-*.md`, ADRs 0004 + 0005, `scripts/dev/test-agentic-*.sh`, `scripts/dev/test-coderabbit-react.sh`, `scripts/dev/test-ui-agent-*.sh` all stay verbatim while their underlying C++ runtime disappears.

This v2 plan exists to clean up that bit-rot once v1 has shipped + stabilised. **Do not start v2 work until v1 PR1 + PR2 merge** — running them in parallel creates rebase conflicts in the same files.

## Scope (sketch — not locked)

### AGENTS.md — strip what `(agentic)`-titled PRs added

Verified via `gh pr list --search "agentic in:title"` + `git log -S ... -- AGENTS.md`:

- **§ Handoff envelope (entire section, lines ~355-426)** — added by PR#248 (`feat(agentic): H2 ...`); modified by PR#299 + #300 (sentinel files table + First-delegate selection subsection).
  - Subsections inside that came from non-(agentic) PRs but reference deleted symbols (§ Spawned-child PR draft requirement from PR#298 references "spawned `claude` child"; § Anti-deception note from PR#283 references `HarnessRunState::IsTransitionAllowed`) — delete with parent; orphans without it.

### AGENTS.md — also-stale-but-not-(agentic)-added (decide at v2 grill time)

These describe deleted runtime but weren't added by `(agentic)`-titled PRs. Whether to strip is a v2 grill-with-docs decision; not pre-locked here.

- **§ Merge gates (lines ~125-197)** — PR#298 (`feat(merge-gates)`). Bash poller `scripts/dev/merge-gates.sh` stays functional; section's gate semantics still describe accurate behaviour. C++ ship-loop auto-invocation goes. Either: (a) strip the C++ auto-invocation language, keep the rest as "useful manual bash poller doc"; (b) strip section entirely.
- **§ Autonomous ship-loop default (lines ~73-109)** — PR#260 (`chore(agents)`). Most of the section is still valid for non-spawned work; the gate-check + handoff references go.
- **§ Post-ship turn-end protocol (lines ~110-124)** — option 3 ("Wait for gates and merge") goes stale; options 1 / 2 / 4 stay accurate. Trim option 3.
- **§ Project rules § Force-push carve-out** — PR#309 (`chore`). `agent/<id>` carve-out moot; `claude/<id>` (orchestrator-spawned) part still applicable. Trim to just the `claude/<id>` case.

### `docs/agent-rules/DELEGATION.md`

Not modified by any `(agentic)`-titled PR but holds:

- **§ Debug-mode pause-loop** — overrides ship-loop for `debug-detective` triggers. The `debug-detective` agent file stays (general-purpose); the spawned-harness debug-trigger surface is gone. Strip pause-loop subsection.
- **§ API-500 mid-run recovery** — entirely about spawned-agent recovery. Strip subsection.
- **§ Trigger auto-activation table** rows for `handoff-implementer` / `pr-iterator` / `coderabbit-triage` — agent files deleted; routing rows orphan. Strip rows.

### `agents/*.md` agent files added by `(agentic)`-titled PRs

- **`agents/handoff-implementer.md`** (PR#248; v-bumped by #299) — **DELETE entire file**.
- **`agents/pr-iterator.md`** (PR#255) — **DELETE entire file**.
- **`agents/coderabbit-triage.md`** (`ac8aeb85` `docs(agentic)`) — **DELETE entire file**.

### Other docs added by `(agentic)`-titled PRs

- `docs/design/agentic-coding-handoff.md` — **DELETE** (PR#217/#240/#259 etc.; describes deleted design).
- `docs/design/agentic-flow-implementation.md` — **DELETE** (PR#217/#225 etc.).
- `docs/design/agentic-triage-flow.md` — **DELETE** (PR#217).
- `docs/design/coderabbit-react-loop.md` — **DELETE** (PR#302 et al.).
- `docs/agentic/TRIAGE_MANUAL.md` + `USAGE.md` — **DELETE** (`docs(agentic)` PRs).
- `docs/agentic/` directory itself — **DELETE**.

### ADRs

- **ADR 0004 (pluggable-coding-harness-runner)** — `docs/adr/0004-pluggable-coding-harness-runner.md`. Decision was about a runtime that no longer exists. Action: flip status Accepted → **Withdrawn** with a one-line note pointing at v1 ripout commits.
- **ADR 0005 (force-push carve-out for spawned-agent recovery)** — same shape; flip → **Withdrawn**.
- **ADR 0006 (orchestrator-pr-stays-draft-by-default)** — STAYS Accepted (general rule, not agentic-specific).
- **ADR 0003 (github-as-itrackerclient)** — STAYS Accepted; v1 PR2 honors it for the tracker-only role. Optional one-line update noting the original "agentic triage half" rationale is partially obsolete; new tracker stands on the same interface choice.
- **ADR 0007 (audit-trail-actor-column)** — already Withdrawn from prior session.

### Scripts added by (agentic)-titled PRs — DELETE all 11

Verified via `gh pr view $pr --json files --jq '.files[] | select(.path | startswith("scripts/"))'` over the 42-PR set. All drive deleted C++ surface; non-functional post-v1-ripout.

| Script | Introduced by | Notes |
|---|---|---|
| `scripts/dev/test-agentic-triage-cli.sh` | PR#230 | T5 triage CLI smoke |
| `scripts/dev/test-ui-agent-proposals.sh` | PR#231 (modified #239) | T6 UI panel smoke |
| `scripts/dev/test-agentic-approve-reject.sh` | PR#233 | T8 proposal approve/reject smoke |
| `scripts/dev/test-agentic-handoff-cli.sh` | PR#251 (modified #252, #267) | H3 ClaudeCodeLocalRunner smoke |
| `scripts/dev/test-agentic-handoff-clarification.sh` | PR#253 (modified #267) | H5 clarification dual-channel smoke |
| `scripts/dev/test-agentic-handoff-iterate.sh` | PR#255 | H7 PR-iteration smoke |
| `scripts/dev/test-ui-agent-handoff.sh` | PR#256 | H8 handoff UI panel smoke |
| `scripts/dev/test-ui-agent-proposals-handoff-button.sh` | PR#257 (modified #267) | H9 Start-handoff button smoke |
| `scripts/dev/test-agentic-handoff-scenario.sh` | PR#259 | H10 handoff scenario step smoke |
| `scripts/dev/test-ci-react.sh` | PR#302 | phase-9 CI react loop smoke |
| `scripts/dev/test-coderabbit-react.sh` | PR#302 (modified #303) | phase-9 CodeRabbit react loop smoke |

### Scripts NOT touched by (agentic)-titled PRs — KEEP per strict reading

- `scripts/dev/merge-gates.sh` + `.graphql` + `-prompt.sh` — added by PR#298 (`feat(merge-gates)`) — bash poller, no Smatchet C++ deps; runs against any PR via `gh api graphql`. Still useful for manual PR-gate polling.
- `tests/bats/merge_gates.bats` + `tests/fixtures/merge_gates_*.json` — test the bash poller; stay.

### Workflows

- `.github/workflows/*` — audit each file. Workflows that dispatch `claude` / `codex` subprocesses or react to CodeRabbit bot patterns: **DELETE**. Generic build / test / perf workflows: **KEEP**.
- `.coderabbit.yaml` — **KEEP** (CR review of PRs still useful for non-agentic work).

### Backlog

- `docs/backlog/agent-self-improvement/*.md` — sweep entries that reference deleted C++ surface; either delete the entry or move to `applied.md` with a note. Don't touch general-purpose entries.

## Sequencing

1. **Wait for v1 PR1 + PR2 to merge.**
2. **Grill v2 plan** via `grill-with-docs` once concrete decisions need locking (which non-(agentic)-titled sections to strip; how aggressive on workflow deletion; ADR status policy).
3. **Architect pre-code review** before opening v2 PR.
4. **One squashed v2 PR** for all doc + agent file + ADR + script deletions. Net negative LOC; reviewable as a single mechanical sweep.

## Risks / non-goals

**Risks**:
- **Conflict with future re-introduction** — if the agentic flow comes back later, this v2 deletion makes recovery slightly harder (more files to re-author vs un-strip). Accepted; v1 already deleted the C++ side. Git history is the authoritative archive.
- **AGENTS.md doc-anchor regressions** — AGENTS.md has anchor-audit CI; verify every cross-link survives. Section deletions break links from other files (CONTEXT.md, agents/*.md, docs/design/*.md). Mitigation: anchor-audit CI gate catches it.

**Non-goals**:
- **Re-introducing agentic features** — not this plan.
- **Touching tracker code** — v1 PR2 already shipped the clean GitHub tracker.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
