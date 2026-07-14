# Plan — Subagent eval trace flywheel (harvest real traces into golden cases)

> **Slug**: `subagent-eval-flywheel` (matches this file's basename without `.md`).
>
> **Status**: `deferred` — superseded by docs/plans/subagent-eval-agentic-coverage.md (design folded into its Phase 2 via #978); the auto-harvester deliverables remain unbuilt. **Partial (2026-07-14):** the *postmortem→eval hook* (AGENTIC_INFRA_AUDIT.md Proposal P8) shipped separately — the `gate-escape-postmortem` skill now authors an eval candidate case (human-promoted, same curation gate) whenever a gate escape's miss was agent-reviewable. That grows the corpus from real misses without the trace-harvester below.
>
> **Scope clarifier**: evaluates the **development agents** (`agents/*.md`), same as the parent plan. This plan adds **automatic golden-case growth** ("production traces become eval datasets").
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

The parent plan (`subagent-eval-harness.md`) hand-seeds 3-5 golden cases for `code-review`. That doesn't scale and doesn't capture real failure modes as they occur. The video's flywheel lesson: **grow the golden set from real production traces.** Smatchet already accumulates those traces on disk — session scratchpad archives, transcripts, per-agent token logs — so no new capture infra is needed, only a harvester + a curation gate.

**Intended outcome (one sentence):** after this lands, a maintainer can run one command to turn real prior agent runs into **redacted candidate cases**, review them, and promote curated ones into the live golden set — growing eval coverage without hand-authoring every case.

## Approach

A harvester (`scripts/dev/agent-eval-harvest.sh`) scans traces that **already exist** — `.session-context.archive/` (scratchpads), session transcripts, `.claude/.agent-tokens.jsonl` (tells which agent ran) — for runs of covered agents, extracts `(input, agent-output)` pairs, **redacts secrets / PII**, and emits them as *candidate* cases under `tests/agent-eval/<agent>/_candidates/` conforming to the parent plan's `case-schema.json` (minus `referenceOutcome`, which a human supplies).

Candidates are **proposals, not golden**. Promotion is a deliberate, reviewed step: a human attaches the `referenceOutcome`, sanity-checks redaction, and moves the file from `_candidates/` into the live set via PR. This keeps the gate deterministic (only curated cases score) while letting the dataset grow from reality. A dedup ledger (`tests/agent-eval/.harvest-ledger.json`) records already-harvested trace IDs so re-runs don't re-propose the same trace.

No trace-storage / observability infra is built — the harvester reads files already on disk. This is the "flywheel" maturity phase, not a Braintrust-scale platform.

## Files to modify

1. `scripts/dev/agent-eval-harvest.sh` — scan `.session-context.archive/` + transcripts + `.claude/.agent-tokens.jsonl`; emit **redacted** candidate cases; harness-specific adapter like the runner (transcript shape is Claude-Code-specific — isolate the parse seam).
2. `tests/agent-eval/<agent>/_candidates/*.json` — staging area for un-curated candidates (await human `referenceOutcome` + promotion).
3. `docs/agent-eval/harvest-policy.json` — which agents to harvest, redaction patterns (token / email / key / Jira / p4 scrub), per-run candidate cap.
4. `tests/agent-eval/.harvest-ledger.json` — dedup ledger of already-harvested trace IDs.
5. `tests/bats/agent_eval_harvest.bats` — redaction + dedup tests (fixture transcript with planted fake secrets → assert scrubbed; re-run over the same trace → assert no duplicate).
6. `docs/agent-rules/subagent-eval.md` — extend (created by the parent plan) to document the harvest → curate → promote flywheel + the curation gate.

## Existing utilities reused

- `case-schema.json` from the parent plan — `docs/agent-eval/case-schema.json` — candidates conform to it (sans `referenceOutcome`).
- Trace sources — `.session-context.archive/` (via `scratchpad-recall` skill) + `.claude/.agent-tokens.jsonl` (via `agent-tokens` skill) + session transcripts.
- Harness-adapter philosophy — `agents/scripts/core/setup-harness.sh` + `AGENTS.md` § Harness adapter — harvester is per-harness, emitted case format portable.
- Shell-lint gate — `agents/scripts/core/test-shell-lint.sh`.

## UX Pillar callouts

Dev-process tooling only — no product-runtime code. All four N/A (same rationale as the parent plan).

- **Pillar 1**: N/A — offline tool. **Pillar 2**: N/A. **Pillar 3**: N/A — pure shell + the parent's pure-Python validation. **Pillar 4**: N/A — no UI.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A` — touches `scripts/`, `tests/`, `docs/` only; no `Source/Core/`.

## Risks / non-goals

- **SECURITY — secret / PII leakage (primary risk)**: session transcripts + archives can carry tokens, API keys, emails, Jira / p4 content. *Mitigation*: mandatory redaction pass **before any candidate is written**; candidates stage in `_candidates/` and enter the committed set only through human PR review; run the `security-review` agent on the harvester before it ships; the redaction bats test plants fake secrets and asserts they never reach an emitted candidate.
- **RISK — curation burden / candidate quality**: auto-harvested traces are noisy. *Mitigation*: harvester emits *proposals* only — a human attaches `referenceOutcome` and promotes; a per-run candidate cap (`harvest-policy.json`) keeps review tractable.
- **RISK — trace-format coupling**: transcript shape is Claude-Code-specific. *Mitigation*: harvester is harness-specific like the runner; isolate the parse seam, keep the emitted case format portable.
- **NON-GOAL — auto-promotion**: candidates never enter the scored set without human review. No exceptions.
- **NON-GOAL — online / continuous harvest**: this is a manual, on-demand command, not a live pipeline.

## Verification

- **Harvester test (`tests/bats/agent_eval_harvest.bats`)**: fixture transcript with planted fake secrets → asserts redaction scrubs them from emitted candidates; a second run over the same trace asserts the dedup ledger blocks a duplicate.
- **Schema conformance**: emitted candidates validate against `case-schema.json` (sans `referenceOutcome`).
- **Shell-lint**: `scripts/dev/agent-eval-harvest.sh` passes `agents/scripts/core/test-shell-lint.sh`.
- **Security review**: `security-review` agent run on the harvester before merge (redaction completeness, path traversal on trace globs, no secret echo to logs).
- **Build gate**: N/A — no compile.
- **Manual residue**: candidate curation is intentionally manual (the curation gate). Documented in `subagent-eval.md`, not silent residue.

## Out of scope (flagged, not designed here)

- **Everything in the parent plan** (`subagent-eval-harness.md`): schemas, scorer, runner, curated `code-review` cases, advisory CI. This plan assumes those exist.
- **Coverage beyond the parent's covered agents**: harvest only agents the parent plan already scores.
- **Phase 3 — online / continuous eval + dashboard**: no-action.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
