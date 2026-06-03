# GitHub Issues are the canonical tracker for product bugs

**Status:** accepted (2026-06-03)

Product **bugs** — defects in Smatchet's shipped behaviour (crash, UB, wrong output, data corruption, PII leak, UI freeze, race) — are tracked as **GitHub Issues**, not in the internal self-improvement backlog. The internal backlog (`docs/self-improvement/categories/*.md`) is reserved for work on the **agentic system itself** (process / tooling / infra / test / security friction) plus product **tech-debt** (the new `debt` category). The backlog's old `bug` category — chartered as "defect in shipped behaviour" — is **deprecated**, because that is exactly what now lives in GitHub Issues; bugs in the agentic harness/scripts (e.g. the `comment_audit.py` cp1252 crash) fold into `tooling`/`infra`.

This was prompted by issue #734: CodeRabbit auto-created a GitHub Issue from an `@coderabbitai` CR-triage reply on PR #733, duplicating a `bug.md` entry for the same bug — surfacing that the project had **two** trackers for product bugs with no boundary, dedup rule, or autonomous-handling protocol.

## Considered options

- **Internal backlog canonical (suppress CR Issues)** — keep `bug.md` as the single source of truth for all dev-loop-generated bugs; configure CodeRabbit to stop auto-creating Issues; GitHub Issues only for the user-facing `log-a-bug-github` feature. *Rejected*: keeps bug tracking invisible to anyone outside the repo's markdown, no native PR↔bug cross-linking, and fights CR's behaviour rather than using it.
- **Dual-track with backlink** — keep both, cross-linked. *Rejected*: this is exactly the #734 duplication — two places to update, drift risk.
- **GitHub Issues canonical (chosen)** — product bugs are Issues; the backlog is agent-meta + tech-debt.

## Consequences

- **Boundary**: GitHub Issues = product/user-facing intake (bugs + the `log-a-bug` user/crash reports); internal backlog = agent-system self-improvement + product tech-debt. A code-review finding about a real product bug is a GitHub Issue, not a backlog entry.
- **Backlog spec changes**: `bug` category deprecated, `debt` category added (`AGENT_SELF_IMPROVEMENT.md` + `test-backlog-counts.sh`). The ~15 genuine product bugs currently in `bug.md` migrate to Issues; the ~3-4 tech-debt items move to `debt`.
- **Orchestrator behaviour change**: on a confirmed pre-existing product bug during CR triage, the orchestrator now dedup-greps open Issues and (if none) creates a **structured** Issue (`gh issue create` with `bug` + priority `P0–P3` + `area:<subsystem>` labels), instead of appending to `bug.md`. CodeRabbit's stray auto-issues are embraced and reconciled by a sweep.
- **New tooling**: an `issue-sweep.sh` + `issue-janitor` (closeout + periodic) keep Issues labelled/deduped/stale-swept — the role the backlog's triage cadence played.
- **Reversal cost is real**: undoing this means migrating Issues back to markdown, restoring the `bug` category, and retraining the loop — hence this ADR.
- Full protocol + migration + sweep design: `docs/plans/shipped/issue-triage-protocol.md` (and `docs/agent-rules/issue-triage.md` once shipped).
