# Plan — Merge-time gate-snapshot ledger (lossless gate-escape detection)

> **Slug**: `merge-snapshot-ledger` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

Split out of [`agent-audit-remediation`](agent-audit-remediation.md) § Out-of-scope (grill decision 2026-06-05) as C2-part-1. That plan shipped the mechanical half of the C2 fix (mergedAt-ordered scan window in `postmortem-owed.sh`); this plan delivers the lossless half.

The gate-escape detector `postmortem-owed.sh` reads **live `statusCheckRollup`** for already-merged PRs to find red-check escapes. That is **provably lossy** (verified against the live repo):
- GitHub **overwrites rollup contexts by name on re-run** (the same dedup `merge-gates.sh:273-336` relies on) — a check that was RED at merge but re-ran green later leaves no RED entry to find. There is nothing left to filter by `completedAt`.
- Override labels are **stripped post-merge by policy** (`merge-gates.sh:723` — "MUST NOT stay on the PR post-merge"), so a live `gh pr view --json labels` at SessionStart sees them gone.

So the only lossless capture of merge-time truth is a **snapshot taken at the decision instant** by the merge actor. The design doc for the detector already promises "at merge time" (`gate-escape-postmortem.md:26`) — this closes that semantics gap.

**Intended outcome — one sentence:** after this lands, every `develop` merge writes a one-line gate-verdict snapshot (override-labels + red-checks at the decision instant) to a committed ledger, and `postmortem-owed.sh` reads that ledger first — so a post-merge re-run or a stripped label can no longer hide a gate escape.

## Approach

A committed append-only **JSONL ledger** at `docs/self-improvement/merge-snapshots.jsonl`, one line per merge, written **cooperatively by every merge actor at the decision instant** and read ledger-first by the detector (live `statusCheckRollup` stays as the documented degraded fallback for un-instrumented / pre-ledger merges, so the detector never goes blind).

The distributed-write contract spans the three merge actors: `merge-watcher.py` `handle_pass()` (the daemon path), the orchestrator's in-session `gh api .../merge` (`ship-loops.md:30`), and `git-janitor`. A single shared helper (`merge-snapshot-append.sh`, idempotent on `pr`+`mergeCommit`) keeps all three writers consistent. The ledger is the in-repo committed file (NOT the `%LOCALAPPDATA%` watcher registry — that is machine-local and its entries are deleted on merge, while `postmortem-owed.sh` is a repo-rooted SessionStart hook that runs on any clone).

The trade-off — lossless-but-distributed (every actor must cooperatively write) vs lossy-but-single-reader — is hard to reverse (committed ledger format + a 3-writer contract) and warrants an **ADR** (`docs/adr/NNNN-merge-time-snapshot-ledger.md`).

## Files to modify

1. `docs/self-improvement/merge-snapshots.jsonl` (new) — committed append-only ledger. One compact single-line JSON object per merge: `{"pr":N,"mergeCommit":"<sha>","headSha":"<sha>","mergedAt":"<iso>","gates":"GATES_PASSED","redChecks":[...],"overrideLabels":[...],"mergeActor":"merge-watcher|orchestrator|git-janitor","schema":1}`. Schema documented in the helper header.
2. `agents/scripts/core/merge-snapshot-append.sh` (new) — shared helper `append_merge_snapshot <pr> <mergeCommit> <headSha> <gatesVerdict> <overrideLabelsCsv> <mergeActor>`; composes one single-line JSON object, idempotent `>>`-append (grep-guard on `pr`+`mergeCommit`), atomic. Passes `test-shell-lint.sh`.
3. `agents/scripts/core/merge-watcher.py` (`handle_pass()`, after `squash_merge_pr()` returns `merge_sha` (`:1780`), **before** `maybe_remove_from_registry` (`:1783`)) — append the snapshot with `mergeActor='merge-watcher'`. **Gap to close:** `handle_pass()` currently has neither override-labels nor the gated head SHA in scope at the write-site — add a `gh pr view --json labels` + head-oid fetch there (the cost the ADR weighs).
4. `docs/agent-rules/ship-loops.md` (§ after GATES_PASSED squash-merge, `:30`) — the in-session orchestrator merge + `git-janitor` MUST append a snapshot line (`mergeActor='orchestrator'`/`'git-janitor'`) immediately after their `gh api .../merge`.
5. `agents/scripts/core/postmortem-owed.sh` — before the live `JQ_ROWS` path, read `merge-snapshots.jsonl`; for an in-window PR with a ledger line keyed by `pr`+`mergeCommit`, derive the trigger from the snapshot (`overrideLabels` / `redChecks`); fall through to the live query (documented degraded fallback) only when no ledger entry exists. `has_entry()` dedupe unchanged.
6. `docs/adr/NNNN-merge-time-snapshot-ledger.md` (new) — the lossless-but-distributed vs lossy-but-simple decision; why the committed JSONL ledger (not the machine-local registry, not a CI artifact, not a `completedAt<=mergedAt` rollup filter).
7. `docs/plans/shipped/gate-escape-postmortem.md` — § Implementation-log note: the primary red-check trigger now reads the snapshot ledger first (lossless); live `statusCheckRollup` demoted to fallback.

## Existing utilities reused

- `agents/scripts/core/merge-gates.sh` (`statusCheckRollup` evaluation `:273-336`; override-label strip `:723`) — the merge-decision moment where the lossless snapshot exists, and *why* the ledger is the only lossless capture.
- `agents/scripts/core/merge-watcher.py` `handle_pass()` / `squash_merge_pr()` / `maybe_remove_from_registry` (`:1753-1783`) — the daemon write-site.
- `agents/scripts/core/postmortem-owed.sh` `has_entry()` + `JQ_ROWS` — the dedupe + scan pipeline the ledger-read augments; live path kept as fallback.
- The `merge-watcher-nudge-persistence` pattern — precedent for a persisted gate-state artifact.

## UX Pillar callouts

- **Pillar 1-4**: no runtime impact — a script + a committed ledger + docs + Python in the watcher daemon. Zero product code. Indirectly strengthens all four by making gate-escape detection (incl. perf / crash escapes) lossless.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

**N/A** — no `Source/Core/` code. The diff is `agents/scripts/**` + a `*.jsonl` ledger + `*.md` (`is-pure-docs-diff.sh` pure-docs-allowlisted); build/ctest/perf gates skip.

## Risks / non-goals

**Risks:**
- **Distributed-write gap** — a merge actor that forgets to append leaves a ledger hole. → mitigated: the live `statusCheckRollup` fallback is kept, so the detector never goes blind; it only loses losslessness for un-instrumented paths until wired. All three actors named in Files-to-modify.
- **Extra `gh` calls at the watcher write-site** — fetching labels + head-oid in `handle_pass()` adds latency to the merge path. → measure; the merge path is not latency-critical (post-gate, pre-cascade).
- **Ledger growth** — append-only JSONL grows unbounded. → accepted (one line per merge ≈ tiny); a periodic prune is a follow-up if it ever matters.
- **JSONL atomicity** — concurrent `>>` from parallel merges could interleave. → single-line compact JSON makes line-granular appends atomic; idempotent grep-guard covers retries.

**Non-goals:**
- A CI-blocking "snapshot required" gate — the snapshot feeds an advisory nudge; blocking merges on it re-introduces ceremony.
- Auto-detecting post-merge *bugs* — out of scope (a separate classifier).
- Backfilling historical merges — the ledger starts empty; pre-ledger PRs use the live fallback.

## Verification

- **Bucket A / E**: N/A — no C++.
- **Helper**: `merge-snapshot-append.sh` writes a well-formed single line; idempotent on re-run (no double line); `test-shell-lint.sh` clean.
- **Detector**: with a seeded ledger entry, `postmortem-owed.sh --list` derives the trigger from the snapshot (override label surfaces even though stripped live); falls back to the live query for a PR absent from the ledger; dedupes via `postmortems.md`.
- **Watcher**: a fixture merge through `handle_pass()` appends one ledger line with the correct `mergeActor` + captured labels/head.
- **Build gate**: N/A — pure-docs/agentic-shell (`is-pure-docs-diff.sh`).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test against the domain model (sharpen "merge-time snapshot" vs "gate verdict" vs "rollup") before finalising; record the outcome.
- **Manual residue**: none designed. If any verification ends up manual, add a `docs/self-improvement/categories/tooling.md` entry.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise / delete them.

- **A `coverage-out-of-band` snapshot field** — depends on that override label existing; add when it does.
- **Trend analytics over the ledger** (which gate-class escapes most) — a later audit once the ledger has volume.
- **Pruning / rotation of the JSONL ledger** — defer until size matters.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
