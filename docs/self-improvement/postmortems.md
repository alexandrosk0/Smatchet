# Gate-escape postmortems

> Append-only ledger of **gate escapes** — something that shipped to `develop`
> that a gate should have caught. Per the "gate, don't trust" philosophy, the
> response to an escape is a **new gate**, not a one-off fix.
>
> Filed via the [`gate-escape-postmortem`](../../agents/_shared/skills/gate-escape-postmortem/SKILL.md)
> skill; surfaced by [`postmortem-owed.sh`](../../agents/scripts/core/postmortem-owed.sh)
> (SessionStart nudge). Blameless by construction — entries name the gate hole,
> never an agent/person.
>
> **Entry shape (the `### Preventing gate` field is mandatory — an entry cannot
> close without it):**
>
> ```
> ## <date> · PR #N[, #M …] · <trigger>
> ### What escaped
> <the gate that didn't catch it>
> ### Root cause
> <blameless RCA>
> ### Preventing gate
> <the concrete new gate/rule/test/lint that catches the class — or
>  "none — override legitimate (reason)" for a deliberate, correct override>
> ### Filed as
> <link to the spawned docs/self-improvement/categories/<cat>.md entry>
> ```

<!-- Latest first. Append new entries at the top. -->

## 2026-06-03 · PR #792 · red-check (non-required doc-validation gate) — THIRD recurrence, gate still unapplied

> Same class as the two entries below (#780/#784 and #771/#774/#776/#778). Logged
> separately so `postmortem-owed.sh` dedupes #792, and to record that the
> already-filed preventing gate has now been escaped a **third time in one day** —
> escalation, not a new gate.

### What escaped
#792 (`plan: ci-build-time-reduction`) merged with **"Doc anchors + agent
contract" = `failure`** on its head (`b648abb8`), no override label. It shipped
two doc-validation defects to `develop`: an **MD028** blank-line-in-blockquote in
`docs/plans/deferred/self-improvement-one-entry-per-file.md` (carried by an
active→deferred `git mv`), and a **broken `AGENTS.md §` anchor ref** in
`docs/plans/active/ci-build-time-reduction.md:116` (`§ Scope-reduction edits +
final-check grep` — an inline mid-bullet bold the anchor-collector never registers
as an anchor, compounded by `+` being a `TERMINATOR_CHARS` split point). Both
`md_lint --all` + `test-doc-anchors` scan tree-wide, so the red surfaced on every
subsequently-opened PR. The anchor half was healed independently by a parallel
session in **#795** (merged 14:26, repointed to `§ Process rules § Scope-reduction
edits`); the **MD028** half was still live on develop and is healed by #793 — two
sessions hit the same escape, a side-effect of the third recurrence going unnoticed
long enough for concurrent heals.

### Root cause
**Identical to the filed gate hole** — "Doc anchors + agent contract" is still not
in the `develop` required-status-check set (required: `Test-delta gate`,
`Windows + MSVC` ×2, `Shell lint`). Timeline rules out a stale-pending read: the
doc job was terminally `failure` at 14:05:19, #792 merged at 14:09:05 — ~4 min
later. So the red was visible and terminal at merge. The prose rule ("never merge
past ANY red check") + the filed structural gate both already exist; the gate was
simply **never applied**, so the advisory-only state bit a third time. This is now
a pattern: a prose rule cannot hold a discipline that a one-click/poller merge path
keeps defeating — only the required-check flip removes the path.

### Preventing gate
**No new gate — applied the existing overdue one.** The fix was the already-filed
infra entry "make the doc-validation contexts required": **"Doc anchors + agent
contract"** added to `project.config.json` `branch_protection.required_contexts`
+ `ci.required_checks` and pushed live via `setup-branch-protection.sh`
(develop required set 4 → 5). **APPLIED 2026-06-03** — GitHub now blocks any
merge with that job RED, removing the merge path that defeated the prose rule
three times. The third recurrence escalated the entry **P2 → P1**; applying it
closes the structural hole. No second system; this postmortem is the escalation
evidence that finally drove the fix.

### Filed as
[`docs/self-improvement/categories/infra.md`](categories/infra.md) 2026-06-03
"doc-validation gates are NON-required" — **escalated P2 → P1** with #792 added as
the third recurrence.

## 2026-06-03 · PR #780, #784 · admin-merged past a red check

### What escaped
The orchestrator direct-merged two PRs (`gh api -X PUT … /merge`) while a check
was RED: #780 past a red **CR-findings** check, #784 past a red **"Doc anchors +
agent contract"** doc-validation job. Each shipped real breakage to `develop`
(an unaddressed CR finding; a `SMATCHET_DEVIATION` portable-purity leak + a
dangling `active/` plan ref) that needed a follow-up heal (#781, #785).

### Root cause
The merge decision gated only on the **four required** checks + CR-pass, treating
any **non-required** red check as ignorable. "Non-required" governs what *blocks*
in GitHub — it does not mean the failure is fake. Compounded by using a direct
admin `gh api` merge, which bypasses the gate-poller that would have surfaced the
red job. The orchestrator had local evidence the doc-suite was red but merged on
the required-only signal anyway.

### Preventing gate
Encoded the rule in `AGENTS.md` § Merge gates: **never merge past ANY red check,
required or not** — every check on the head must be terminal-green before a
squash-merge, *especially* a direct admin `gh api` merge; the only exceptions are
a named override label or a positively-confirmed irrelevant flake. Pairs with the
infra-P2 "make doc-validation contexts required" (below) — that makes the gate
*structural* so the discipline can't be forgotten.

### Filed as
`AGENTS.md` § Merge gates (the rule) + [`docs/self-improvement/categories/infra.md`](categories/infra.md)
2026-06-03 "doc-validation gates are NON-required" (the structural fix).

## 2026-06-03 · PR #771, #774, #776, #778 · red-check (non-required doc-validation gate)

> #780's CR-findings escape is a distinct incident — see the "admin-merged past a
> red check" entry above; this entry is the doc-validation-job class only.

### What escaped
The whole `test-docs.sh` doc-validation suite (`test-portable-purity`,
`test-plan-index`, `test-plan-ref-integrity`) runs only in the CI job **"Doc
anchors + agent contract"**, which is **not** in the repo's required-status-check
set. So PRs merged with that job RED.

### Root cause
Branch protection gates only the four required contexts (`Test-delta gate`,
`Windows + MSVC`, `Windows + MSVC (light)`, `Shell lint`). Any other check —
including all doc-validation and CodeRabbit-findings — is advisory, so the
merge-watcher + admin merges let red non-required checks through. Concrete
damage: a `Source/Core` literal leak into a portable dir, a `docs/plans/INDEX.md`
drift (concurrent-archive race), and a dangling plan ref in `.cpp` comments all
reached `develop`; each needed a follow-up heal.

### Preventing gate
Make the doc-validation contexts **required** (add "Doc anchors + agent contract"
to branch-protection's required set + `project.config.json`
`branch_protection.required_contexts`), so a portable-purity leak / INDEX drift /
dangling ref can never merge. Secondary: move INDEX regeneration to **merge time**
to kill the concurrent-archive drift; add an archive helper that repoints **all**
refs (git grep across all tracked files, not just `*.md`) on `git mv`.

### Filed as
[`docs/self-improvement/categories/infra.md`](categories/infra.md) — 2026-06-03
infra P2 "doc-validation gates are NON-required" (shipped in PR #780).

## 2026-06-03 · PR #791 (escape) / #796 (surfaced) · gate-escape (concurrent-PR lint gap)

### What escaped
`docs/plans/deferred/self-improvement-one-entry-per-file.md:6` shipped to `develop`
with an **MD028** markdown violation (bare blank line inside a blockquote) — a rule
the `md_lint.py --all` gate (doc-validation.yml) enforces. No single PR ever went
red on it; it ambushed the next unrelated docs-touching PR (#796, CI build-time
reduction), where `md_lint --all` ran against the merged tree and failed the "Doc
anchors + agent contract" job. Fixed inline on #796 (one `>` continuation line).

### Root cause
A **concurrent-PR gate gap**, not an admin-merge-past-red. The md_lint gate landed
in **#789** (`6987b7d5`). The violating file landed in **#791** (`d8e7c421`), which
had branched *before* #789 merged — so #791's "Doc anchors + agent contract" PR run
executed a `doc-validation.yml` that did **not** yet contain the md_lint step and
reported **pass** (run 26888295853, 16s). GitHub did **not** re-run #791 against
#789's newly-merged gate before merging #791 (branch-protection's "Require branches
to be up to date before merging" is **off**). After both merged, `develop` carried
the gate *and* the violation, but no PR was ever red. The post-merge `push`
doc-validation on #791's merge commit (it matches `**/*.md`) would have gone red on
`develop` — a red post-merge run blocks nothing and went unnoticed. General class:
**Gate added in PR-A + violation added in concurrently-open PR-B → neither PR red
alone, merged tree violates.** Distinct from the "admin-merged past a red check"
incidents above (#780, #784) — here every PR was genuinely green.

### Preventing gate
**ENABLED 2026-06-03** — branch-protection **"Require branches to be up to date
before merging"** turned on for `develop` (`project.config.json`
`branch_protection.strict: true`, applied via `setup-branch-protection.sh`; GitHub
confirms `required_status_checks.strict == true`). Forces PR-B to
rebase onto the latest `develop` — re-running CI **with** any gate PR-A just added —
before it can merge, so a concurrently-introduced violation is caught on PR-B's own
run instead of the next innocent PR. Trade: every PR must be current before merge
(more rebases; the merge-watcher already polls, so it can drive the update). The
lighter, already-shipped half-measure — making doc-validation **required** (the
2026-06-03 infra-P2 entry) — does NOT close this class: #791's run was *green*
(stale workflow), so a required-context check still passes. Up-to-date-before-merge
(or a GitHub merge queue, which re-tests the merge result) is the structural fix.

### Filed as
[`docs/self-improvement/categories/infra.md`](categories/infra.md) — 2026-06-03
infra "require-branches-up-to-date (concurrent-PR gate gap)".
