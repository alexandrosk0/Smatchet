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

## 2026-06-03 · PR #771, #774, #776, #778 · red-check (non-required gate) + PR #780 · red-check (CR-findings)

### What escaped
The whole `test-docs.sh` doc-validation suite (`test-portable-purity`,
`test-plan-index`, `test-plan-ref-integrity`) runs only in the CI job **"Doc
anchors + agent contract"**, which is **not** in the repo's required-status-check
set. So PRs merged with that job RED. Separately, #780 was admin-merged past a
red **CR-findings** check (also non-required).

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
