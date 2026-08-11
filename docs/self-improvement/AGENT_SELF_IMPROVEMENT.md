# Agent self-improvement

Suggestions emitted by delegated agents for improving the agent ecosystem itself
— prompt tweaks, missing context, redundant steps, new-subagent candidates,
tooling gaps. Live entries split by category; this file is the index + spec.

## Format

```
- <YYYY-MM-DD> · <agent-name> · [<category>] · P<0-3> — <one-line title>
  Details: (single paragraph or short bullet list — context that explains why)
  Concrete next action: (what unblocks the entry)
  Status: open | deferred | observational
  Last-reviewed: <YYYY-MM-DD>   # default = creation date; bump on each sweep
```

**One file per entry (new entries).** Write each new entry as its OWN file at
`docs/self-improvement/categories/<category>/<YYYY-MM-DD>-<slug>.md` — one entry's
§ Format block per file, `<slug>` a short kebab-case of the title. Two concurrent
PRs adding entries then touch disjoint paths, so adds can never merge-conflict (and
archiving = removing/moving that one file, so deletes can't either). The ~135
**legacy entries stay in the monolith `categories/<category>.md` files** untouched
and are still read **in union** by every consumer (the count gate
[`test-backlog-counts.sh`](../../agents/scripts/core/test-backlog-counts.sh) `--list`
and the triggered-follow-up nudge
[`followup-due-nudge.sh`](../../agents/scripts/core/followup-due-nudge.sh) both glob
the monolith *and* the per-category subdir). This is the incremental, new-entries-only
slice of the deferred [`self-improvement-one-entry-per-file`](../plans/deferred/self-improvement-one-entry-per-file.md)
plan — the monolith files are **not** migrated. (`bug.md` is deprecated and takes no
new entries; the `applied.md` archive stays a single union-merged file.)

**Optional `Triggered-follow-up:` line** — for a follow-up GATED ON A FUTURE CONDITION ("re-measure after ~10 PRs", "after a date", "once a plan ships"). Add ONE line to the entry (after `Concrete next action:`); [`followup-due-nudge.sh`](../../agents/scripts/core/followup-due-nudge.sh) surfaces it at SessionStart when the condition is met. Lifecycle: [`process-rules.md`](../agent-rules/process-rules.md) § Triggered follow-ups.

```text
  Triggered-follow-up: when=<kind>:<spec>; action=<one-line>; baseline=<optional metric prose>; fired=never
```

Fields in that order (`;` + space between fields). Four `when=` kinds (`;`-delimited `key=val` inside the spec — **no space** after `;`, which is how the parser tells the within-spec separator from the between-field separator):

- `pr-count:base=develop;since=<YYYY-MM-DD>;n=<N>` — N squash-merged PRs to base since a date.
- `date:<YYYY-MM-DD>` — calendar deadline reached.
- `plan-shipped:<slug>` — `docs/plans/shipped/<slug>.md` exists.
- `file-age:<path>;days=<N>` — `<path>` last touched (git) ≥ N days ago.

Author with `fired=never`; when the orchestrator acts it stamps `fired=<date>` via PR (the nudge is **read-only** — a due-but-unaddressed entry re-nudges every session until stamped, like `postmortem-owed`). Entries with no `Triggered-follow-up:` line are invisible to the nudge — fully backward-compatible.

Applied entries are archived immediately to [`self-improvement/categories/applied.md`](categories/applied.md).

## Categories

- **bug** — **DEPRECATED** (ADR-0014). Product bugs (defects in shipped behaviour)
  now live as **GitHub Issues**, not here — see [`docs/agent-rules/issue-triage.md`](../agent-rules/issue-triage.md).
  `bug.md` stays readable but takes **no new entries**; a bug *in the agentic
  harness/scripts* folds into `tooling`/`infra` instead.
- **debt** — product **tech-debt**: internal maintainability with no user-observable
  defect (god-object, duplication, coupling, missing abstraction, "should refactor").
  A debt item that proves user-observable becomes a GitHub Issue (per issue-triage.md).
- **process** — workflow friction; orchestrator-packet discipline; shortcuts an
  agent finds itself doing manually that should be encoded in its prompt;
  context an agent had to discover that should be pre-loaded.
- **tooling** — missing CLI / lint / semantic-search / harness automation gap.
- **infra** — build system / CI / scaffolding / new-subagent candidates.
- **test** — test coverage gap, fixture, bucket-E wiring.
- **security** — exploitable surface; secret leakage; sandbox escape; defense-in-depth.

`external-blockers.md` carries entries that can only be resolved outside this
repo (GitHub repo settings, upstream tool sources).

## Priority

- **P0** — data corruption · exploitable · merge-block.
- **P1** — load-bearing · silent failure · production regression.
- **P2** — test infra · process gap · cross-agent friction.
- **P3** — doc edit · cosmetic · single-agent prompt tweak.

Mandatory on every `open` entry.

## Workflow

1. Delegated agents end every report with `## Self-improvement`. Empty is fine.
2. Orchestrator reads, dedupes, and **creates a new per-entry file** at
   `docs/self-improvement/categories/<category>/<YYYY-MM-DD>-<slug>.md` (one entry,
   the exact § Format block) — **not** this index file, and **not** the legacy
   monolith `categories/<category>.md` (those stay as-is, read in union; see
   § Format). Disjoint paths mean two concurrent adds can't conflict.
   **Claims about a file's behaviour must cite a verified line.** Before asserting in
   any backlog / self-improvement / plan entry that a specific file does or doesn't do
   X, verify it against the committed tree — `git show origin/<base>:<file>` (never the
   mid-session working tree, which a watcher HEAD-swap can stale) or a targeted `grep`.
   Entries written from assumption get caught downstream at a CI / CodeRabbit round; a
   5-second check at authoring time avoids the re-push. (This is the backlog/plan analogue
   of the CR-reply post-push verification in `docs/agent-rules/process-rules.md`.)
3. **No count to sync.** Counts are on-demand and derive from a directory listing
   — `bash agents/scripts/core/test-backlog-counts.sh --list` counts the monolith
   entry-lines **plus** the per-entry files. There is no stored count column to
   hand-edit (removed 2026-06-03), so a new per-entry file needs no index touch and
   the pre-push gate stays green by construction.
4. When evidence accumulates (mentioned by ≥2 agents OR blocks the same
   workflow ≥3 times), apply: edit the relevant agent prompt(s) in `agents/`
   or AGENTS.md; flip Status to `applied`; archive the entry with

   ```
   bash agents/scripts/core/archive-backlog-entry.sh docs/self-improvement/categories/<cat>/<file>.md
   ```

   which appends the body to the union-merged `applied.md`, `git rm`s the
   per-entry file, and stages both. For a legacy monolith entry, move the block
   as before. (NOT `git mv` — that renames the file, it cannot append one file's
   content into another.)

   **Do not hand-roll this with `cat >> … && git rm`.** That recipe breaks links
   in both directions, silently. Per-entry files live at `categories/<cat>/`
   while `applied.md` lives one level up at `categories/`, so every relative link
   in the body is off by one directory the moment it is appended — and `cat`
   cannot fail, so nothing says so. The `git rm` half is worse: it orphans links
   from *other* documents that cited the entry, and those files are unmodified,
   so a diff-scoped check cannot see it. Archiving one entry this way cost 7
   outbound and 2 inbound dangling links, the latter surfacing as a red required
   check. The script re-depths the body, repoints inbound links at `applied.md`
   at each referrer's own depth, and re-runs `test-markdown-links.sh --all` as a
   self-check — the `--all` is load-bearing, since the default diff scope sees
   neither half. Use `--dry-run` to preview.
   **If the edited agent has eval coverage** (currently `code-review`), score
   the edit base-vs-head per § Optimize against evals before flipping to
   `applied` — attach the advisory delta to the PR.

## Optimize against evals (advisory)

Prompt edits to `agents/*.md` mutate decision quality with **zero before/after
measurement** unless they're scored. For any agent that has eval coverage, the
apply step (Workflow 4) is measured the same way the perf gate measures a
`Source/Core/` change — except the dimension is agent decision quality, not
frame latency:

- run the curated case set once with `--prompt-root=<base worktree>` and once
  with `--prompt-root=<head worktree>`, then diff the two result JSONs with
  `scripts/dev/agent-eval-score.py`;
- the delta table is **advisory** — a malformed artifact FAILs, a quality
  regression WARNs (it does not block the prompt edit) until judge-vs-human
  calibration data exists.

Full contract, the two-worktree recipe, and case-authoring live in
[`../agent-rules/subagent-eval.md`](../agent-rules/subagent-eval.md). Coverage is
`code-review` only in the Phase-1 MVP; broader coverage + the WARN→BLOCK
graduation are deferred (tracked in
[`categories/tooling.md`](categories/tooling.md)).

> **Common failures this prevents:** (a) writing the entry into this index file or
> the legacy monolith `categories/<cat>.md` instead of a new per-entry file
> `categories/<cat>/<date>-<slug>.md`; (b) assuming a count must be hand-synced —
> it doesn't (no stored count column; `--list` derives counts from monolith
> entry-lines **plus** the per-entry files on demand); (c) applying `merge=union`
> to a monolith `categories/<cat>.md` — **never** do this, it wrongly preserves
> both blocks on a parallel-delete (see `.gitattributes`); the per-entry files
> need no merge driver because distinct paths can't conflict.

## Triage cadence

Sweep when (a) opening any PR that touches `agents/`, (b) any single live
category file exceeds ~20 open items, or (c) a P0 entry has aged ≥7 days
without movement.

## Gate-escape postmortems

A **gate escape** — something that shipped to `develop` that a gate should have
caught — gets a blameless postmortem in [`postmortems.md`](postmortems.md), not
just a category entry. `agents/scripts/core/postmortem-owed.sh` (SessionStart
nudge) detects escapes (a non-SUCCESS check on a merged head, an override label,
a `Revert`, an overdue deviation); the [`gate-escape-postmortem`](../../agents/_shared/skills/gate-escape-postmortem/SKILL.md)
skill runs the RCA. The postmortem's **mandatory** `### Preventing gate` field —
the concrete new gate/rule/test that catches the *class* — is filed back here as
a **normal category entry** (existing format + priority + apply threshold). The
postmortem is the incident finder; this loop is the applier; there is no second
apply-system. A legitimate override closes with `### Preventing gate: none —
override legitimate (reason)`. The post-merge-bug trigger is **manual** (the
user/orchestrator names it); only check/label/revert/deviation escapes are
auto-detected.

## Index

| Category | File |
|---|---|
| bug         | [self-improvement/categories/bug.md](categories/bug.md) |
| process     | [self-improvement/categories/process.md](categories/process.md) |
| tooling     | [self-improvement/categories/tooling.md](categories/tooling.md) |
| infra       | [self-improvement/categories/infra.md](categories/infra.md) |
| test        | [self-improvement/categories/test.md](categories/test.md) |
| security    | [self-improvement/categories/security.md](categories/security.md) |
| external    | [self-improvement/categories/external-blockers.md](categories/external-blockers.md) |
| applied (archive) | [self-improvement/categories/applied.md](categories/applied.md) |

The `File` column is the **legacy monolith** (existing ~135 entries, read in union).
**New entries go in the per-category subdir** `categories/<category>/<YYYY-MM-DD>-<slug>.md`,
one entry per file — see § Format. (`bug` takes no new entries; `applied` stays a
single union-merged archive, no subdir.)

> **Live counts are on-demand, not stored** — run `bash agents/scripts/core/test-backlog-counts.sh --list` for the current per-category counts. The count is the **union of both sources**: the monolith entry-lines (`grep -c '^- 20' <file>`) **plus** the per-entry files in `categories/<category>/` (one entry each). The count column was **removed 2026-06-03**: a hand-maintained count is edited by *every* entry-adding PR, so concurrent PRs conflicted on that single line on every add (the highest-frequency self-improvement merge conflict). Deriving it on demand removes the shared edit entirely. The gate (`test-backlog-counts.sh`, `test-all.sh` discovery) now **guards against re-introducing a stored numeric count column** rather than verifying one. The per-entry-file layout is the **incremental slice** (new entries only) of the deferred [`self-improvement-one-entry-per-file`](../plans/deferred/self-improvement-one-entry-per-file.md) plan; it kills the *category-content* add/delete conflict for new entries without the 135-entry migration.
