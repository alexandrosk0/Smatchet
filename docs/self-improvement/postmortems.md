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
> **Entry shape (the `### Preventing gate` and `### Eval case` fields are both
> mandatory — an entry cannot close without them):**
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
> ### Eval case
> <a subagent-eval candidate case when the miss was agent-reviewable (a code
>  smell / logic bug / policy violation a review agent scores) — the RCA is the
>  spec: missed defect = input, "a competent reviewer flags X" = reference
>  outcome; suggestion-only, promoted by a human. Or "none — not
>  agent-reviewable (reason)" for a pure CI-config / infra / required-context gap.
>  Closes the postmortem→eval flywheel (docs/plans/deferred/subagent-eval-flywheel.md)>
> ### Filed as
> <link to the spawned docs/self-improvement/categories/<cat>.md entry>
> ```

<!-- Latest first. Append new entries at the top. -->

## 2026-08-07 · PR #1962 · Merged past a CodeRabbit review that never happened (`cr-out-of-band` + `cr-disposition:cr-rate-limited`), and the escape detector reported the window clean

### What escaped
`fix(screenshot): make bucket-C user-info captures deterministic` (#1962) merged
at `2026-08-06T13:50:08Z` as `a7fa2a32` with 34 checks green and **zero
CodeRabbit review of the diff** — CR's account quota was exhausted, so it
returned a status-only SUCCESS with no inline findings. The merge-gate poller
correctly refused (`BLOCK: CodeRabbit rate-limited on a CODE PR — pausing for CR
to re-review on quota recovery`), the block was escalated to the user, and the
user authorised the documented escape hatch: `cr-out-of-band` +
`cr-disposition:cr-rate-limited`. Gate behaviour was exactly as designed.

The escape is the **second** gate: an override label on a merged PR is trigger 2
of `postmortem-owed.sh`, which exists to nudge for this postmortem. It reported
`no gate escapes owed a postmortem (last 20 merges clean)`. Four consecutive
invocations agree — a deterministic miss, not a flake. Had the user not asked,
the escape would have gone unrecorded and the CR-quota class would have shipped
again with no ledger trace.

### Root cause
Two independent holes, only the second of which is a gate defect.

**1. CR quota exhaustion has no lane.** The poller's rate-limit pause assumes
quota recovers and CR re-reviews. When the account budget is spent for the
period, "wait for CR" is unbounded, so the only terminal moves are *block
indefinitely* or *override*. The override is the right call under
`AI_POLICY.md` § Escalate-when-unvalidatable, and it was made with explicit user
attestation — but it means a CODE diff reached `develop` with no bot review, and
nothing downstream distinguishes that from a reviewed one.

**2. The detector false-dedups on prose.** `has_entry()`
([`postmortem-owed.sh:240-251`](../../agents/scripts/core/postmortem-owed.sh))
runs two probes. Probe 2 is deliberately scoped to a heading line, its comment
stating the intent — *"scoped to `^#+ …` so a #N mention in prose body can't
false-suppress a real owe"*. Probe 1 is unscoped and scans the whole file:

```
grep -cE "PR #1962([^0-9]|$)"        → 2     # prose, inside an unrelated entry
grep -cE "^#+ .*PR #1962([^0-9]|$)"  → 0     # no entry is actually filed
```

Both hits are body prose in the 2026-08-05 `#1937` entry (lines 2171, 2184),
which cites #1962 as the determinism work its instance ratchet rests on. Citing
a sibling PR is how these entries are normally written, so the failure mode is
general: **any PR named in an existing entry's prose is permanently and silently
exempted from ever being nudged.** The output is indistinguishable from a clean
window — the same "mask discards the verdict rather than downgrading it" shape
as the bucket-C golden mask. Because this is the detector *for* escapes, the
hole doesn't leak one defect; it suppresses the mechanism that turns escapes
into gates, and the exempted set grows as entries accumulate cross-references.

### Preventing gate
Scope probe 1 to entry headings exactly as probe 2 already is
(`^#+ .*PR #N([^0-9]|$)`, matching the documented `## <date> · PR #N …` shape),
splitting the `commit <sha>` alternation out to stay whole-file — `has_sha_entry`
documents bare-sha matching as deliberate for triggers 3+4. Back it with a
`tests/bats/` case asserting that a ledger containing only a *prose* `PR #N`
mention still reports #N as owed while a real heading dedupes it — the property
both probes are trying to express and neither tests. Optionally add an explicit
`### Escaped PRs: #A, #B` field per entry and dedup on that, so heading prose
style can drift without re-opening the hole.

For hole 1 the override was legitimate — no gate is owed for the merge decision
itself, but the CR-quota state deserves a distinct poller verdict
(`cr-quota-exhausted`) separate from the recoverable rate-limit pause, so the
attestation path is reached deliberately instead of by waiting out a timeout.

### Eval case
None — not agent-reviewable. Both holes are shell-level detector logic and
CI-config gaps; there is no code smell or policy violation in a diff for a
review agent to score. The regression is expressible as a bats case (above),
which is the stronger form here.

### Filed as
[`categories/applied.md` § postmortem-owed-prose-mention-false-dedup](categories/applied.md)
## 2026-08-06 · PR #1948 · override: `cr-out-of-band` used to unwedge a required CR gate that could not reach a verdict

### What escaped
`fix(build): resolve font assets from the main worktree when a linked worktree lacks them` (#1948)
merged to `develop` 2026-08-05 (`2602340e`) carrying `cr-out-of-band`. The label was strictly
**load-bearing**: the required StatusContext `CR findings (0 actionable)` sat PENDING with every other
check green (22/22), `mergeStateStatus=BLOCKED`, and re-running the workflow re-posted PENDING. No
CodeRabbit review of the merged diff ever completed — the override, not a verdict, cleared the gate.
Two gates missed it: the CR finding gate (no terminal state for this input) and `postmortem-owed.sh`,
whose `--list` reports "no gate escapes owed" for this PR.

### Root cause
Blameless — two independent holes on the same CR path.

**(1) The gate cannot terminate.** `.github/actions/cr-finding-gate/action.yml` `decide()` takes the
`n_reviews > 0` branch as soon as any CR review node exists on the head, which skips the
`n_reviews == 0` disambiguation via CR's own `CodeRabbit` StatusContext (which *was* SUCCESS). It then
greps the review body for `Actionable comments posted:[[:space:]]*[0-9]+`. CR's last on-head review was
its post-resolve acknowledgement — an **empty body** — so `n` is empty and `decide()` returns
non-terminal (correctly fail-closed since #524, where a preamble line above the header produced a
fail-*open*). After 12×15 s the action posts `pending — awaiting CodeRabbit review on current head`.
No path leads from "CR's final word on this head is body-less" to a terminal verdict, and the state is
not self-healing: only a new push or a fresh CR review clears it, and neither is guaranteed to arrive.
The fail-closed instinct is right; the missing piece is a terminal arm for the one input where
fail-closed can never resolve.

**(2) The nudge that should have flagged the override is scope-gated, not evidence-gated.**
`postmortem-owed.sh:156-163` drops the trigger `override: cr-out-of-band` whenever
`pr_touches_core_cpp()` is false, on the rationale (:149-155) that the label "only waives the
(advisory) CodeRabbit review", making a non-Core diff a false positive. #1948 touched CMake, a bats
file, a wrapper script and a README — zero `Source/Core/src/*.cpp` — so it was dropped. That rationale
holds for an advisory-verdict waiver but not for a **wedged required gate**, where diff scope says
nothing about whether the override mattered. The script already owns the right test
(`override_is_moot()` / `gate_conclusion()`, applied to `tests-out-of-band` / `perf-out-of-band` /
`coverage-out-of-band` / `intent-out-of-band`); `cr-out-of-band` is the one override routed to the
scope heuristic instead.

### Preventing gate
Two, one per hole.

**(a) Terminal arm in the CR finding gate** — in the `n_reviews > 0` / empty-`n` path, disambiguate the
way the `n_reviews == 0` path already does, but only for a **body-less** review: if the latest on-head
CR review body is empty/whitespace **and** CR's `CodeRabbit` StatusContext on that head is SUCCESS,
post success ("CR settled with nothing actionable"); keep the non-terminal retry for a **non-empty**
body that merely lacks the header, so the #524 fail-open (a preamble line *above* a real header, i.e. a
non-empty body) stays closed. An empty body cannot hide a finding count. Pin the empty-body +
StatusContext-SUCCESS combination in the harness covering the action's decision table.

**(b) Evidence-gate the nudge's `cr-out-of-band` drop** — replace the `pr_touches_core_cpp()` scope
heuristic with `gate_conclusion "$pr" 'CR findings'`: SUCCESS on its own → moot, drop; PENDING /
non-SUCCESS / absent → load-bearing, keep and owe a postmortem, whatever the diff touches. This folds
`cr-out-of-band` into the `override_is_moot()` machinery the other four labels already use and deletes
the special case. Without (b), future regressions of (a) would ship equally invisibly.

### Eval case
none — not agent-reviewable. Both holes are CI-config / gate-logic gaps (a GitHub Action's decision
table and a nudge script's trigger filter), not a code smell or logic bug in a reviewable diff. No
reviewer reading #1948's diff — CMake, a bats file and a README — could have surfaced either; the
defect lives in the gate machinery, not the change under review.

### Filed as
(a) [`categories/process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md`](categories/process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md)
(P1, process — shipped in PR #1953, `ec41fe86`) ·
(b) [`categories/process/2026-08-06-postmortem-owed-cr-override-denoise-hides-wedged-gate.md`](categories/process/2026-08-06-postmortem-owed-cr-override-denoise-hides-wedged-gate.md)
(P2, process).

Sibling hole found while auditing this one — a poller **false-block**, opposite sign, **not** an escape
(no unsafe merge; masked on current `develop` by the 2026-06-20 self-improvement auto-exemption), so no
postmortem is owed for it. Filed as
[`categories/tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](categories/tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md)
(P2, tooling) plus the stale-checkout hazard that made it visible,
`2026-08-06 · [process] · P1 — gate tooling run from a long-lived session branch`,
since archived as applied in [`categories/applied.md`](categories/applied.md).

## 2026-08-06 · PR #1941 · squash-merged with `--admin` past 22 required checks that never ran (repo-wide Actions queue jam) — and no detector reads that state

### What escaped
`docs(plan): preferences IA re-segmentation + global search` (#1941) merged at
`2026-08-06T19:25:12Z` as `c7fb2236` via `gh pr merge --squash --admin`, with **all 22
branch-protection-required contexts absent from the head rollup**. The head `4617a034` was
never built at all — `gh api repos/<o>/<r>/actions/runs?head_sha=4617a034…` returned
`total_count: 0`. The feature (a ~4-slice rewrite of the Preferences window) landed on
`develop` with zero CI validation. `postmortem-owed.sh --list` run immediately afterwards
reported "no gate escapes owed a postmortem (last 20 merges clean)".

### Root cause
Two independent holes, plus a diagnostic one.

**(a) The ship-loop has no defined move for "CI is structurally unavailable."** Actions was
jammed repo-wide: 75 runs stuck `queued` since 18:13 UTC, no new run created repo-wide after
19:16 UTC, and a `gh pr close && gh pr reopen` (to re-fire the `pull_request` event) produced
0 runs. There was no reachable green head. The loop's only defined behaviour is to keep
polling, so the available options collapse to "wait indefinitely" or "override" — the second
being precisely what the gates exist to prevent. The escape is the missing third option, not
the choice made between the two that existed.

**(b) The class is invisible to the detector.** `postmortem-owed.sh` keys on a non-SUCCESS
check at merge, an override label, a `Revert` commit, or an overdue deviation. An `--admin`
merge past *absent* checks emits none of those: no red check (there is no check), no label,
no revert. This is the **third** recurrence of the same detector hole — `2026-07-10 · PR
#1698` and `2026-08-05 · PR #1937` both reached it from the other direction (green on the PR
head, red on develop) and both proposed a develop-tip required-green assertion that never
landed.

**(c) Diagnostic misdirection, lower severity but it cost the most wall-clock.** An earlier
poll on the pre-merge head reported 22 `required-missing` checks with the generic hint
"never ran; e.g. a GITHUB_TOKEN bot push that did not re-trigger CI". The real state was
`mergeStateStatus: DIRTY` / `mergeable: CONFLICTING` — GitHub will not build a conflicted
head — from one conflict in `docs/plans/INDEX.md`. `merge-gates.sh` already fetches both
fields in its existing GraphQL response and does not branch on them, so the actionable cause
was available on poll 1 and surfaced only after a full 90-poll timeout and manual digging.

### Preventing gate
1. **`postmortem-owed.sh` fifth signal (primary).** For each merge commit on `develop` in
   the scanned window, resolve the merged PR's head sha and flag the merge when
   `actions/runs?head_sha=<sha>` yields `total_count == 0`, or when the head rollup carries
   fewer contexts than the branch-protection required set. Catches admin merges, zero-rollup
   merges, and the "CI never triggered" class in one check — and unlike the label-keyed
   signals it needs no cooperation from whoever performed the merge. Testable in
   `tests/bats/`.
2. **`merge-gates.sh` names the real cause.** Branch on `mergeStateStatus == DIRTY` /
   `mergeable == CONFLICTING` before reporting `required-missing`, emitting a distinct
   blocked reason ("head is conflicted — CI will not build it; merge origin/develop
   first"). Same for `BLOCKED` with a zero-length rollup.
3. **Escalate on an Actions outage.** When the poller observes zero runs created repo-wide
   inside the poll window, stop polling and escalate with that diagnosis instead of timing
   out at 90 polls with a per-check message — per [`AI_POLICY.md`](../../AI_POLICY.md)
   § Escalate, don't assume, an unvalidatable state is an escalation.

Compensating verification actually performed on the merged head, for the record: dual-target
build (`SmatchetStandalone` + `SmatchetCore_DX12`) EXIT=0 and `test-lint-rules.sh --diff
origin/develop` EXIT=0 (advisory WARNs only). That is not CI and does not substitute for it;
the next `develop` post-merge run, once Actions drains, is the backstop to watch.

### Eval case
none — not agent-reviewable. This is a CI-availability + detector-coverage gap, not a defect
in a diff that a review agent would score.

### Filed as
[`2026-08-06-admin-merge-past-absent-checks-undetected` (archived in `categories/applied.md`)](categories/applied.md)

## 2026-08-05 · PR #1937 · `Doc anchors + agent contract` was GREEN on the PR head and RED on `develop` the instant the squash landed — `test-plan-index` derives row dates from git history the merge itself rewrites

### What escaped
`feat(about): About Smatchet dialog under Help, with generated build provenance` (#1937) merged at `2026-08-05T11:33:25Z` as `fce0951c` with every check terminal-green, including the required `Doc anchors + agent contract`. The develop tip went **RED on that same check** immediately after, with `test-plan-index: DRIFT — shipped-plan index out of sync (182 plans in archive)`. The PR archived `docs/plans/shipped/about-dialog-help-menu.md` and committed the matching `docs/plans/INDEX.md` row dated `2026-08-04`; after the squash the generator wanted `2026-08-05`. No override label, no admin merge, no flake — the merge was correct against every signal available at merge time. There was **no state of the PR branch that could have passed both pre- and post-merge**, because the value being checked did not exist until the merge created it.

### Root cause
Blameless — **a gate whose expected value is a function of git metadata that squash-merge rewrites.** `agents/scripts/core/test-plan-index.sh:122-143` derives each row's date from `git log --follow --format=%ad --date=short -- <path>`, i.e. the file's *first-commit* date. `--follow` is what makes this normally stable: a plan created in `plans/active/` months ago keeps its original date when it moves to `plans/shipped/`. But a squash-merge collapses the entire branch into one commit, and the pre-merge per-file history is not reachable from `develop` — so `--follow` finds exactly one commit and returns the **squash date**:

```
$ git log --follow --format='%ad %h %s' --date=short -- docs/plans/shipped/about-dialog-help-menu.md
2026-08-05 fce0951c feat(about): About Smatchet dialog under Help, ... (#1937)
```

The failure therefore needs three conditions, all of which held: (1) the PR both *creates or moves* a plan file into `shipped/` and *commits the index row* for it; (2) the repo squash-merges; (3) the branch work and the merge fall on different calendar days. #1937's plan was authored `2026-08-04` and merged `2026-08-05` — one midnight boundary.

Three defences did not fire, and none of them could have: **(a) pre-merge CI** computed the date from branch history where `2026-08-04` was correct; **(b) local `pre-ship.sh`** does the same and also passed; **(c) `postmortem-owed.sh`** keys on merge signals — non-SUCCESS checks at merge, override labels, `Revert`, overdue deviations — and this escape emits **none** of them (it reports "no gate escapes owed" for #1937 today), so the class is structurally invisible to the nudge. That blindness is the same one named in the `2026-07-10 · PR #1698` entry, and this is its **second instance**: a required check goes red on the develop tip, nothing announces it, and the next author inherits the block.

Note the two workarounds already sitting in this script for the *same underlying fragility* — a shallow-clone guard (`:45`, `:105`) and a staged-rename sibling-tier fallback (`:135-143`, citing the #1061 / #1092 archive date-drift "twice"). Each patched one way git metadata can move under the generator. Squash-merge is the third. The recurring shape is the defect, not any one of the three.

### Preventing gate
**Stop deriving the value from mutable git metadata.** `test-plan-index.sh --fix` should write the resolved date **into the plan file itself** (an explicit `<!-- plan-date: YYYY-MM-DD -->` marker, authored once when a plan is archived) and have the generator prefer that marker, falling back to `git log --follow` only for legacy plans with no marker. The marker travels through squash, shallow clone, and staged rename identically, because it is content rather than history — so it does not add a fourth special case, it **retires the existing two** (`:45` shallow guard, `:135-143` sibling tier) whose whole job is to paper over a history lookup that should not be load-bearing. Enforcement: `test-plan-index.sh --selftest` gains a case asserting that a `shipped/` plan whose only commit is the current HEAD still resolves a stable date, and that a marker date disagreeing with the index row FAILs.

**Second, close the detection hole:** the develop-tip required-green assertion proposed in the `2026-07-10 · PR #1698` entry and never landed. It is now two-for-two — extend `agents/scripts/core/postmortem-owed.sh` (or add `develop-tip-required-green.sh`) to query the develop tip's required-check conclusions at SessionStart and raise a loud nudge naming the introducing PR. A gate that can only go red *after* the merge needs a detector that looks *after* the merge; merge-instant signals cannot see this class by construction.

### Eval case
None — not agent-reviewable. The miss is a determinism property of a CI generator's input (git history vs file content), not a defect visible in #1937's diff; a reviewer reading that PR would have seen a correct index row, because it *was* correct until the merge rewrote its basis.

### Filed as
This entry + the one-line index resync in PR #1944 (instance) + [`categories/applied.md` § plan-index-date-derived-from-mutable-git-history](categories/applied.md) (P1, tooling — the marker-based fix and the develop-tip assertion).

## 2026-08-03 · PR #495 · a shipped plan's § Deviations closed a planned file-row by asserting a delivery that was never made

### What escaped
`docs/plans/shipped/msvc-build-onboarding-hardening.md:85` (§ Deviations from plan) states: "`build_standalone.ps1` (plan file 1) already had the MSVC bootstrap from slice 1." That bootstrap **does not exist and never did** — `git log -S 'vcvars' -- scripts/dev/local/build_standalone.ps1 scripts/dev/build_standalone.ps1` is **empty across all history**. The file's only `vswhere` use is `Get-VsWherePath` (:71-83), which locates `MSBuild.exe`; there is no `vcvars64.bat` import anywhere in it. PR #493 (`a9058b96`) planned the capability, PR #495 (`da36b45f`, 2026-05-28) closed the row as already-done, and the plan was archived `shipped` on 2026-06-06. **No gate reads a plan's post-ship prose against the tree**, so a false record of delivered capability sat load-bearing for ~2 months and seeded a false § Context premise into a downstream plan (`dev-onboarding-first-run-quickstart`).

### Root cause
Blameless — a **name conflation with no verification surface**. `build_standalone.ps1` did contain an env-bootstrap call at the time: `Use-Msys2Ucrt64Environment` (an **MSYS2 UCRT64** environment setup), which #495 replaced with the msys2-retirement `throw`. That pre-existing call was read as "the bootstrap", so the planned **MSVC/vcvars** row was closed as already-satisfied rather than deferred. Three things that should have caught it did not: (1) the plan's own § Verification (actual) records `test-build-wrapper.ps1` 3/3 green, but its three cases cover the msys2-retirement throw, the `Exe :`/`Time:` print, and the stale-sibling table — **none exercises an MSVC env bootstrap**, so a passing verification block is fully consistent with the capability being absent; (2) the plan-doc gates (`test-plan-index.sh`, `test-plan-ref-integrity.sh`, `test-markdown-links.sh`) validate structure, index state, and link resolution — never claim truthfulness; (3) `postmortem-owed.sh --list` reports "no gate escapes owed" because the nudge keys on merge signals (non-SUCCESS checks, override labels, `Revert`, overdue deviations) — an untrue prose claim in a doc emits no merge signal and is structurally invisible to it. AGENTS.md § Process rules already requires deferred § Files-to-modify rows to carry a § Deviations entry; the hole is that a § Deviations entry claiming **"already done elsewhere"** needs no evidence, so the cheapest way to close a row is an unverified assertion.

### Preventing gate
New delta-gated rule **`plan-claim-anchor`** — `agents/scripts/core/test-plan-claim-anchors.sh`, joining the existing plan-doc gate family in the "Doc anchors + agent contract" doc-validation job. Scoped to a plan's **§ Deviations** / **§ Implementation log** sections only: a line asserting pre-existing delivery (`already had|has|have|exists|existed|implemented|landed|shipped`, `was already`) must carry a verifiable citation — a link or backticked ref with a `:<line>` suffix, or a `#<PR>` / commit-sha reference. Delta-gated vs `origin/develop` and baseline-grandfathered (measured 2026-08-03, claim pattern above + anchor pattern `:<line>` / `#<2+ digits>` / 7-40-char hex sha, over `## Deviations` / `## Implementation log` sections in `plans/{shipped,active}`: **38** claims across 30 files, **25 unanchored** — all grandfathered; only NEW claims must anchor. The gate's `--selftest` re-derives this baseline rather than hardcoding it), with `SMATCHET_DEVIATION(rule=plan-claim-anchor; …)` for claims about state outside the repo (e.g. GitHub branch-protection API state, which has no `file:line`). The gate cannot prove a claim true; it forces the author to point at the code — and **there is no line to point at for a vcvars import that does not exist**, which is precisely where #495 would have stopped. Deliberately **not** an extension of `postmortem-owed.sh`: this class carries no merge signal, so detection belongs at doc-gate time.

### Filed as
[`categories/applied.md` § plan-post-ship-claims-unverified](categories/applied.md) (P1, tooling)

## 2026-06-28 · PR #1574 (introducer), #1576, #1577 (rode past) · red non-required, OFF-meant-to-block-allow-list `Bucket-E Jira fixture-backend (Mesa GL, hard)` merged on 3 PRs

### What escaped
`Bucket-E Jira fixture-backend (Mesa GL, hard)` went red on `develop` at #1574 (`ea9134e7`, `2026-06-27T17:18:40Z`) and stayed red through #1576 (`870702de`, `2026-06-28T07:57:13Z`) and #1577 (`14b77f4f`, `2026-06-28T11:44:47Z`) — all three squash-merged past it. The lane is a **deterministic** fixture-backend `ui_test.run --spawn` check (its driver declares failures "a fixture-backend regression, not a render flake"), but it sits OFF the poller's meant-to-block allow-list, so `merge-gates.sh` treated its terminal `failure` as advisory and emitted `GATES_PASSED`. `postmortem-owed --list` read "clean" — an off-allow-list non-required red is never recorded as an escape, so the post-merge net was blind too. The underlying product break: #1566's `PathConfinement` hardening (`32392e32`) makes the spawned child reject the parent-absolutized `--outLog` (the same parent-absolutizes / child-confines collision class as the perf-harness `--outPath` break in the 2026-06-27 sibling entry below) — child exits with `handler-error / "outLog rejected: absolute paths are not allowed"` ~0.4 s in, before MCP. Product fix tracked as GitHub Issue #1579 (fix PR in flight). NOT a Mesa/GL boot failure (every other Mesa bucket passes on the same runner), NOT the #1566 loopback-token 401 (child dies before MCP auth).

### Root cause
Blameless — **over-broad allow-list pruning**. The broad `Bucket-` token was deliberately removed from `MERGE_GATES_BLOCK_ALLOWLIST_RE` on 2026-06-15 to stop the poller-jam from the *flaky* Mesa lanes (`Bucket-C screenshot diff`, `Bucket-E UI tests`) whose software-GL exe can't boot (`bucket-mesa-exe-boot` P1; remedy for those = the `bucket-out-of-band` downgrade label, #1218 below). The pruning was correct for the flaky lanes but **collateral** for `Bucket-E Jira fixture-backend (Mesa GL, hard)`, which shares the `Bucket-E` prefix yet is the opposite kind of lane — deterministic and boot-capable. The allow-list re-added exactly one bucket lane afterward (`Bucket launch-smoke (Mesa GL)`, #1370) but not the fixture-backend lane, so a deterministic, regression-catching check was left advisory. Compounding: #1574 fixed the *sibling* `--outPath` confinement collision (perf harness) but not this `--outLog` one, so the same class of break survived in a lane that could no longer block.

### Preventing gate
PRIMARY (prevention) — add the literal `Bucket-E Jira fixture-backend (Mesa GL, hard)` job name to `MERGE_GATES_BLOCK_ALLOWLIST_RE` (`merge-gates.sh:163`), beside the already-blocking `Bucket launch-smoke (Mesa GL)`. This lane is deterministic + boot-capable (unlike the genuinely-advisory `Bucket-E UI tests` / `Bucket-C screenshot diff` lanes kept advisory by #1218's `bucket-out-of-band` remedy), so blocking it does NOT re-introduce the stochastic-flake jam the 2026-06-15 removal protected against. **Sequencing precondition: land only AFTER the #1579 product fix makes the lane green** — adding it while red blocks every PR. COMPANION (test) — a `merge_gates.bats` case asserting the deterministic fixture-backend lane IS allow-listed while the flaky bucket-C/E render lanes are NOT, so a future blanket `Bucket-` edit can't silently re-advisory it.

### Filed as
`bucket-e-fixture-lane-advisory-by-prefix` — archived to [`categories/applied.md`](categories/applied.md) (superseded by the all-gates-blocking flip: `MERGE_GATES_BLOCK_ALLOWLIST_RE="."` blocks every non-advisory check, retiring the curated allow-list)

## 2026-06-27 · PR #1566 (escape) · PR #1571, #1572, #1574 (collateral) · CANCELLED `Perf PR-fast` (meant-to-block, not GH-required) merged via a human native-merge

### What escaped
`fix(security): C++ security hardening — remediate SECURITY_AUDIT.md (6 slices)` (#1566, merge `32392e32378128a4b5341750ee19713fb2d680a9`, `2026-06-27T09:47:29Z`, `mergedBy:alexandrosk0`) merged to `develop` while its `Perf PR-fast (windows-2022)` check was terminal **CANCELLED** — a single rollup entry with no SUCCESS twin (the token-401 idle-to-`timeout-minutes` cancel signature, not a concurrency supersede). `Perf PR-fast` is on the custom poller's **meant-to-block** allow-list but is **not** a `branch_protection.required_contexts` entry. #1566 shipped two breaking changes to the `--spawn` perf harness (a) `McpRequireTokenOnLoopback` default `false→true` → tokenless `WaitForMcpReady` probe gets HTTP 401 → 30 s poll → job idle-cancelled; (b) `PathConfinement` rejects the perf harness's absolute `--outPath` → `kExitValidation`. Because CI builds the `pull_request` merge ref, the break landed on every open PR's `--spawn` gates the instant #1566 hit develop — #1571/#1572 wedged 250+ `merge-watcher` cycles.

### Root cause
Blameless — two compounding gate holes, one at prevention and one at detection. **(1) Prevention:** a native GitHub merge (human direct-merge here; also native auto-merge) does **not** consult the custom `merge-gates.sh` poller, so only GitHub's ~5 *required* contexts gate it. `Perf PR-fast` is meant-to-block in the poller but is not a required context, so its terminal CANCELLED never blocked the native merge. This is the same poller-bypass class as #1438/#1428 below, but for the perf gate rather than the intent gate. **(2) Detection:** the post-merge net was structurally blind too — `postmortem-owed.sh` trigger-1 deliberately excludes CANCELLED as a "supersede" (lines 314-326, to drop concurrency twins), and a human native-merge writes no `merge-snapshots.jsonl` line (the lossless authority at line 388), so the escape was invisible on BOTH the snapshot and the CANCELLED-excluding live path — `postmortem-owed --list` self-reported "clean." The CANCELLED-supersede heuristic is correct for a twin (a later SUCCESS for the same context wins the latest-run dedup), but wrong when the *latest and only* run for a meant-to-block context is CANCELLED.

### Preventing gate
PRIMARY (prevention) — promote `Perf PR-fast (windows-2022)` to a `branch_protection.required_contexts` entry on develop so native merges (human + auto) cannot bypass it; **precondition** (must land first or it wedges every non-perf PR): the check is gated by the `Detect perf-relevant changes` path filter, so it must emit a terminal neutral/success status on non-perf diffs before it can be required (the conditional-skip-wedge trap flagged in postmortem-owed.sh lines 314-318). COMPANION (detection, no precondition) — refine `postmortem-owed.sh` trigger-1 so a meant-to-block context whose collapsed latest-run conclusion is CANCELLED *with no later SUCCESS run for the same context* counts as an escape (the existing group_by-name/max-startedAt dedup already drops the concurrency twin, so this stays false-positive-safe). PR A (#1574) is the immediate unblock (env override + PathConfinement-safe harness capture); PR B carries the secure-by-default product fix.

### Filed as
This entry (the intended standalone backlog file `categories/infra/2026-06-27-perf-pr-fast-not-required-cancelled-escape.md` was never created — the RCA lives here; de-linked so the delta-gated `test-markdown-links` stops tripping on the dangling reference).

## 2026-06-20 · PR #1438 · red `Intent section` (block-allowlisted) merged — PR opened out-of-band via the GitHub API with no `## Intent` section

### What escaped
`docs(plan): command-input hardening (CLI · MCP · Palette · Lua)` (#1438, head `a6888573`) merged to `develop` while the **block-allowlisted** `Intent section` doc-validation check was terminal `failure`, with no `intent-out-of-band` override label. The PR was opened via the GitHub MCP `create_pull_request` API (Claude Code on the web) and its body never contained a `## Intent` section, so the check correctly red-flagged a missing intent — and the merge proceeded anyway (native GitHub auto-merge, squash; `Intent section` is deliberately NOT a branch-protection required context per [ADR-0022](../adr/0022-intent-gate-promotion.md), so native auto-merge does not consult the custom poller's block-allowlist).

### Root cause
Blameless — a **PR-creation path with no intent-capture step**, compounded by native auto-merge bypassing the custom poller. The ship-loop's intent capture (`capture-intent.sh` → `.session-intent/<branch>.log` → templated `## Intent`) runs only for PRs opened through the local ship-loop. A PR opened **out-of-band via the GitHub API** — the only path on a web session with no local git push to the live branch — has no mechanism to inject `## Intent`, so the body ships without it and `Intent section` reds. Native GitHub auto-merge then merged past the red because `Intent section` is intentionally non-required (ADR-0022, to avoid a merge_group deadlock); the custom `merge-gates.sh` block-allowlist that *would* treat it as blocking is not consulted by GitHub-native auto-merge. Distinct from #1428 below — there a stale daemon ran an out-of-date allow-list; here the body was simply never populated.

### Preventing gate
Make the out-of-band PR-creation contract require a hand-authored `## Intent`: a rule in [`ship-loops.md`](../agent-rules/ship-loops.md) § Intent capture that any agent calling the GitHub API/MCP `create_pull_request` (i.e. with no local ship-loop) MUST include a filled `## Intent` section in the PR `body`. This catches the *class* (API-created PRs lacking intent) at authoring time — the only point an agent controls when there is no local hook. Promoting `Intent section` to a branch-protection required context is explicitly NOT the fix — ADR-0022 keeps it off to avoid a merge_group deadlock.

### Filed as
[`process/2026-06-20-intent-section-api-created-pr` (archived → applied.md)](categories/applied.md)

## 2026-06-19 · PR #1428 · red `Intent section` (block-allowlisted) merged by the `merge-watcher` daemon running STALE gate logic — the poller *was* consulted, but its allow-list predated `Intent section`

### What escaped
`docs(skill): surface squash-sha ancestry as signal (c) in stale-branch cherry-trap` (#1428, merge
`092b23480c5f429eb42c1349984976e5e81fffa7`, `2026-06-19T17:47:52Z`, head `4f6d6624`) merged to `develop`
while the **block-allowlisted** `Intent section` doc-validation check was terminal `failure` (completed
`17:46:44Z`, ~68 s before the merge) with **no** `intent-out-of-band` override label. Crucially — unlike the
sibling #1406/#1414/#1415 escape below — this PR did **not** bypass the poller: `mergeActor:merge-watcher`,
the sanctioned `smatchet-merge-watcher` daemon ran `merge-gates.sh`, which returned `GATES_PASSED` and armed
the merge. The daemon's own audit row self-reported clean:
`{"pr":1428,…,"gates":"GATES_PASSED","redChecks":[],"overrideLabels":[],"mergeActor":"merge-watcher"}`
([`merge-snapshots.jsonl`](merge-snapshots.jsonl) line 105).

### Root cause
Blameless — a **long-running daemon enforcing out-of-date gate logic**, not a poller bypass and not a defect
in the gate's *current* source. The watcher ([`merge-watcher.py`](../../agents/scripts/core/merge-watcher.py),
Scheduled Task `SmatchetMergeWatcher`) runs `merge-gates.sh` from **its own host checkout** — the integration
tree `C:/Dev/Smatchet`, parked on `feat/tsan-subset-sync-layer`, a branch predating #1391. `Intent section`
was added to `MERGE_GATES_BLOCK_ALLOWLIST_RE` on **2026-06-18** (#1391, [ADR-0022](../adr/0022-intent-gate-promotion.md));
the daemon's blob of `merge-gates.sh` predated that, so its allow-list regex did **not** match `Intent
section`. A non-required RED check that is not on the allow-list is treated as advisory → not flagged →
`GATES_PASSED`. The daemon had run continuously since 2026-06-15 and never re-synced, so it silently executed
rules two days stale. This is **distinct** from the #1406/#1414/#1415 escape (the poller was *never*
consulted; remedy = a poll-gated merge wrapper) — here the poller **was** consulted, so that wrapper would
**not** have caught it: it would invoke the *same stale poller*. Compounding blind spot: the
`merge-snapshots.jsonl` audit row is written *by the stale poller itself*, so it reports `redChecks:[]` — the
audit trail cannot detect its own staleness.

### Preventing gate
A **gate-logic self-freshness guard** in [`merge-gates.sh`](../../agents/scripts/core/merge-gates.sh) (this
PR): before emitting `GATES_PASSED` it compares the git blob of its own running file against
`origin/develop`'s blob for the same path and, on divergence (or when unverifiable), **refuses
`GATES_PASSED`, fail-closed**. Gated by `MERGE_GATES_FRESHNESS` ∈ `{off (default) | warn | block}`;
`merge-watcher.py` sets `block`. Default `off` leaves every existing caller + local-dev run unaffected.
Backed by 5 bats cases (off-inert, block+stale→refuse, block+fresh→pass, warn→warn-only,
block+unverifiable→fail-closed). A self-guard **cannot** retro-protect a daemon *already running* the
pre-guard file, so the operational complement is mandatory and was applied: the stale daemon was **stopped +
its Scheduled Task disabled** (fail-safe), to be **restarted only from an up-to-date `develop` checkout**
(deferred — cannot safely move the shared integration tree's HEAD this session; the daemon stays
stopped+disabled until a develop-current checkout is available).

### Filed as
New tooling per-entry backlog file
[`tooling/2026-06-19-merge-watcher-runs-stale-gate-logic` (archived → applied.md)](categories/applied.md)
(P1) — carries the freshness self-guard (shipped here) plus the residual *restart-from-fresh-checkout* +
*periodic daemon self-resync* operational gate, cross-ref'd to #1428 and distinguished from the
PRs #1406/#1414/#1415 poller-bypass entry.

## 2026-06-19 · PR #1406, #1414, #1415 · red `Intent section` (block-allowlisted) merged via non-poller paths — bare `gh pr merge --auto` / direct REST bypass the poller-only gate

### What escaped
Three PRs merged to `develop` in one `07:55–07:58Z` batch while the **block-allowlisted** `Intent section`
doc-validation check was non-green, **none** carrying the `intent-out-of-band` override label:
- `docs(self-improvement): postmortem for #1390/#1409 tests-out-of-band escapes` (#1414, merge `96e79412`,
  `2026-06-19T07:55:15Z`) — PR body has **no** `## Intent` section; armed via native `gh pr merge --auto`.
- `docs(self-improvement): reconcile perf-gate-absolute-p99 entry …` (#1415, merge `f6bb3972`,
  `2026-06-19T07:57:58Z`) — PR body has **no** `## Intent` section; merged by a **direct REST merge**
  (`autoMergeRequest` was null — no `--auto` involved at all).
- `feat(tooling): archive-plan.sh one-shot plan-archival helper` (#1406, merge `7bb77daa`,
  `2026-06-19T07:55:33Z`) — body carries a **filled** `## Intent` now, but the failed `Intent section`
  run executed against an earlier body revision lacking it (PR `updatedAt` `07:57:47Z` is **after**
  `mergedAt` `07:55:33Z`), so its red is **stale**, not a content gap. Included because it merged on the
  same non-green-block-allowlist-check-via-`--auto` path and `postmortem-owed.sh` nags it under the same
  `red-check: Intent section` trigger — a ledger reference discharges it.

`Intent section` is on the [`merge-gates.sh`](../../agents/scripts/core/merge-gates.sh)
`MERGE_GATES_BLOCK_ALLOWLIST_RE` *meant-to-block* allow-list (added 2026-06-18, ADR-0022) yet is
deliberately **not** a `develop` branch-protection required context — so only the merge-gates poller /
watcher enforces it. The `intent-out-of-band` label is its override hatch; none was applied to any of the
three. (This is **not** the `postmortem-owed` non-blocking/cancelled-twin over-report class — the failed
runs are single `fail` conclusions on a genuinely block-allowlisted check, not advisory or cancelled.)

### Root cause
Blameless — a **merge-path hole**, not a defect in the gate or the PRs. The block-allowlist is enforced
**only** by `merge-gates.sh`: the sanctioned watcher polls it and arms `--auto` *only on PASS*
([`merge-gates.md`](../../docs/agent-rules/merge-gates.md):84). Any merge path that does **not** consult
the poller honors only the branch-protection required contexts (`Test-delta gate`, `Windows + MSVC` ×2,
`Shell lint`, `Doc anchors`, `Perf PR-fast`, `Coverage`, `Sanitizer` ×2 — `Intent section` is **not**
among them), so it merges the instant those green, ignoring a red `Intent section`:
- bare native `gh pr merge --auto` (#1414, #1406) — GitHub auto-merge waits only on *required* contexts;
- direct REST `PUT …/merge` (#1415) — no gate poll at all.

This is the documented sharp edge — `merge-gates.md`:84 and the 2026-06-11 `process.md` entry "authorized
auto-merge armed via raw `gh pr merge --auto` … `--auto` only waits on the required status checks" (filed
then for **CodeRabbit**, the other poller-only block-allowlist gate) — now recurring **3×** for the
`Intent section` gate in a single batch. That 2026-06-11 entry's remedy was **advisory** ("DEFAULT to the
merge-gates poller path rather than a bare `--auto`"); the triple recurrence is the signal that advisory is
insufficient and the discipline must be **enforced**, not recommended.

### Preventing gate
A **non-admin poll-gated merge wrapper** made the *only* sanctioned agent merge entry-point — a sibling of
the existing [`safe-admin-merge.sh`](../../agents/scripts/core/safe-admin-merge.sh) on the non-admin path:
it runs `merge-gates.sh` (which blocks on the full block-allowlist **incl. `Intent section`**) and arms
`gh pr merge --auto` **only after** a PASS; bare `gh pr merge --auto` and direct REST merge are forbidden
in the ship-loop. Backed by a bats test asserting the wrapper **refuses** when a block-allowlist gate is
red without its override label. ADR-0022 deliberately kept `Intent section` off branch-protection
required-contexts (merge-queue-deadlock reversibility), so the enforcement must live on the **merge-actor**
side, not branch-protection — this promotes the 2026-06-11 advisory "use the poller" into an enforced gate.

### Filed as
New process per-entry backlog file
[`process/2026-06-19-intent-gate-bypassed-via-non-poller-merge` (archived → applied.md)](categories/applied.md)
(P2) — carries the non-admin poll-gated merge wrapper above as its `Concrete next action`, cross-ref'd to
#1406 / #1414 / #1415 and the 2026-06-11 raw-`--auto` advisory entry it supersedes.

## 2026-06-19 · PR #1390, #1409 · override: tests-out-of-band (load-bearing) — behaviour-changing concurrency-correctness fix with no headless test home

### What escaped
Two PRs merged to `develop` with a `tests-out-of-band` label waving a `Test-delta gate` that passed
**only because the label told it to**:
- `fix(commands): marshal debug.window.* g_ui writes onto the UI thread (audit target 3)` (#1390, merge
  `b546e125`, `2026-06-18T19:57:58Z`) — touched `Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp`
  + `docs/self-improvement/categories/security.md`.
- `ci(arm64): Windows-on-ARM native test leg + std::atomic audit (Phase 3)` (#1409, merge `ad0b34a1`,
  `2026-06-19T01:15:40Z`) — touched `AnthropicClient.cpp` / `OllamaClient.cpp` / `OpenAiClient.cpp` (the
  `std::atomic` audit), `.github/workflows/build-and-test.yml` (the new native leg), and docs.

Both carried **zero** `tests/Core/*.test.cpp` delta. The deterministic load-bearing test in
[`postmortem-owed.sh`](../../agents/scripts/core/postmortem-owed.sh) `override_is_moot` —
`tests-out-of-band` is moot iff `Test-delta gate == SUCCESS` **AND** the diff touched a `.test.cpp` — is
FALSE for both (gate green, no test file), so the override dismissed a real coverage requirement, not a
no-op. (Distinct from #1317 / #1308 below, which also touched no test but were behaviour-**preserving**
body relocations → "none — override legitimate". These two change behaviour.)

### Root cause
Not a defect in either fix — both are correct, genuinely behaviour-changing **concurrency-correctness**
changes whose invariant is a *threading* property the headless pure-logic doctest rig cannot express:
- #1390 wraps the `debug.window.resize` / `debug.window.screenshot` handlers' `g_ui` request-flag writes
  (`requestWindowResize/Width/Height`, `requestScreenshot`, `requestScreenshotPath`) in
  `RunOnUiThreadAsCommandResult(app, …)`. Pre-fix, an MCP/Lua **worker thread** dispatching the command
  wrote those fields directly, racing the standalone main-loop poll (non-atomic `int`/`bool`; the
  `std::string requestScreenshotPath` was genuine UB on a concurrent realloc). The invariant is "the write
  lands on the **UI thread**, not the dispatching thread."
- #1409's atomic audit converts racy plain reads/writes of shared cross-thread flags in the AI clients to
  `std::atomic`. The invariant is "the shared flag is accessed **atomically**."

Neither is assertable in the doctest TUs: the rig is headless and single-threaded — no UI thread, no
`g_ui`, no second thread, no `AppController` command-queue marshalling — so a pure-logic test cannot
observe "this write marshalled" or "this access is atomic." A behaviour-changing concurrency fix has **no
in-rig test home** and is routed through `tests-out-of-band` by necessity. That the gate keeps firing on
this exact class is the signal: the class needs a gate the pure-logic rig structurally cannot be.

### Preventing gate
Two concrete gates that catch the class **without** depending on the rig that can't host it:
1. **#1390 class — a static strict-zone lint** (extend `test-lint-rules.sh`) forbidding direct writes to
   the `g_ui` request-flag fields (`requestWindowResize`, `requestWindowWidth`, `requestWindowHeight`,
   `requestScreenshot`, `requestScreenshotPath`) from command-dispatch TUs (`Source/Core/src/Commands/**`)
   outside a `RunOnUiThread*` closure. Sibling handlers (`debug.dock.*`, `bug.report`) already conform, so
   the rule is green on HEAD and fires only on a NEW off-thread-write regression — the #1390 pre-fix shape,
   caught statically with zero test-infra dependency.
2. **#1409 class — a runtime leg, not a unit test.** The Windows-on-ARM **native test leg** #1409 itself
   added is the structural answer (atomics execute on real hardware threads there); extend it (or the
   in-flight `feat/tsan-subset-sync-layer` TSan subset) to exercise the AI-client request paths so a
   non-atomic shared-flag regression surfaces at runtime, plus a lint flagging plain (non-`std::atomic`)
   shared mutable cross-thread flags in the AI-client TUs.

### Filed as
New tooling per-entry backlog file
[`categories/tooling/2026-06-19-concurrency-correctness-no-headless-test-home.md`](categories/tooling/2026-06-19-concurrency-correctness-no-headless-test-home.md)
(P2) — carries both concrete gates above as its `Concrete next action`, cross-ref'd to #1390 / #1409 and
`feat/tsan-subset-sync-layer`.

## 2026-06-16 · PR #1328 · override: cr-out-of-band (CodeRabbit false-positive finding it later retracted in-thread)

### What escaped
`feat(core): AppController fan-in Phase 2 — close the json doors (json_fwd)` (#1328, head
`97480e23`, merge `15:08:59Z`) merged to `develop` with a `cr-out-of-band` label downgrading
CodeRabbit's `COMMENTED + 1 actionable` block to WARN. The single actionable (Major) finding was on
`Source/Standalone/main.cpp:96`: CR claimed `CommandRegistry.h` declares
`Dispatch(const nlohmann::json&)` without including the json header, "forcing main.cpp to include it"
— i.e. a spurious / redundant include introduced by the PR.

### Root cause
Not a defect — the CR finding was a **verified false positive**. `Commands/Command.h:19` owns the
full `#include <nlohmann/json.hpp>`; `CommandRegistry.h:4` includes `"Commands/Command.h"`, so it
inherits the `nlohmann::json` type transitively and correctly omits a direct include (its
`Dispatch(..., const nlohmann::json&, ...)` at line 57 compiles fine). main.cpp:96's
`<nlohmann/json.hpp>` is unrelated to `CommandRegistry` — it exists because *that TU uses
`nlohmann::json` directly* now that Phase 2 closed `AppController.h`'s transitive json door
(`json.hpp` → `json_fwd.hpp`), and the include carries an inline comment saying exactly that. The
author posted an in-thread rejection ("Triaged — rejecting (out of scope + inaccurate premise …)")
and **CodeRabbit itself acknowledged the premise was wrong** ("Acknowledged — premise was wrong.
`Command.h:19` already owns the full `<nlohmann/json.hpp>` include …"). But CR does not re-flip its
review state from `COMMENTED + 1-actionable` to `APPROVED` after retracting a finding in-thread, so
the merge-gate poller still counted it as a CR block — the only escape hatch is the blanket
`cr-out-of-band` label.

### Preventing gate
**none — override legitimate** (the sole CR actionable was a factually-wrong premise, refuted from
the code and retracted by CR in-thread; CI was green; the include the finding objected to is correct
and necessary). The gate hole — CR leaving a retracted finding as a live `actionable` so the poller
blocks until a blanket downgrade is applied — is real but not worth a bespoke auto-detector here:
trusting CR's "Acknowledged" reply text to auto-clear the block is exactly the
"never trust a CodeRabbit ✅-annotation blindly" foot-gun AGENTS.md § Merge gates warns against (it
matches phrasing, not the diff). The durable mitigation is the **machine-readable disposition trail**
already tracked as `cr-out-of-band-disposition-trail` (process.md): #1328's rejection rationale
existed only as PR-thread prose, so the poller couldn't see it and the override's legitimacy was
reconstructable only from the thread — the same evidence-gap that entry exists to close, now with a
new sub-case (CR-retracted-but-still-actionable, not just CR-skipped/no-actionables).

### Filed as
Recurrence note appended to [`process.md`](categories/process.md)
`cr-out-of-band-disposition-trail` (P3; recurrence set now #945 / #953 / #1046–#1072 / #1265 /
#1328 — #1328 is the first instance where a human-readable disposition was present in-thread but not
in the grep-able `cr-disposition:` marker form, reinforcing concrete-action (1)). No new category
entry — the gate is already tracked.

## 2026-06-16 · PR #1317 · override: tests-out-of-band (red Test-delta gate) — behaviour-preserving header→cpp body relocation

### What escaped
`refactor(debt): move dictation hook body out of hot localized-imgui header` (#1317) merged to
`develop` with a `tests-out-of-band` label dismissing a genuinely-RED `Test-delta gate`. The gate
fired because the diff touched `Source/Core/include/SmatchetLocalizedImGui.h` plus a new
`Source/Core/src/SmatchetDictationHook.cpp` with **zero** paired `tests/Core/*.test.cpp` delta.

### Root cause
Not a defect — the diff relocates one function body across the header→TU boundary with **no
behaviour change**. `SmatchetLocalizedImGui.h` drops the `inline` definition of
`HookDictationOnLastItem(char*, std::size_t)` for a bare declaration (header annotated "Behaviour is
identical to the prior inline body."); the byte-identical body moves into a new TU
`SmatchetDictationHook.cpp` in the same `namespace SmatchetLocalizedImGui`. The function is
ImGui-coupled (Class-C — pulls `imgui.h`), so it has no headless unit-test home: the moved body
can't be exercised by a pure-logic doctest TU on the desktop test target. The win (de-inlining a
hot header → recompile-blast-radius shrink) is verified by the build itself, not by a test. Same
behaviour-preserving cross-TU class as #1016 / #1083 / #1096 / #1308 (residue (a) of the Test-delta
override family): `coverage-delta-gate.sh` `_classify_diff` counts a new production `.cpp` with no
`*.test.cpp` delta as a fail and still has no auto-exemption for a pure body relocation, so the
`tests-out-of-band` override was applied — correctly.

### Preventing gate
**none — override legitimate** (byte-identical body relocated across the header→cpp boundary, no
behaviour change, ImGui-coupled Class-C with no headless test home; the de-inline win is verified by
the green dual-target × dual-toolchain build, not a unit test). This is the **5th** ledgered
instance of residue (a) — the behaviour-preserving cross-TU refactor the Test-delta gate cannot
distinguish from an untested logic change (after #1016 / #1083 / #1096 / #1308). No NEW gate is
filed; the existing follow-up (`coverage-gate-platform-else-arm-exemption`, residue (a), P2) gets a
5th-recurrence note naming **header→cpp body relocation** (inline-in-header → out-of-line definition
in a new TU, body unchanged) as a second clean mechanical sub-case to auto-detect alongside the
fwd-decl-only diff (#1308).

### Filed as
5th-recurrence note appended to [`tooling.md`](categories/tooling.md)
`coverage-gate-platform-else-arm-exemption` residue (a) (P2; recurrence set now
#1016 / #1083 / #1096 / #1308 / #1317). No new category entry — the gate is already tracked.

## 2026-06-16 · PR #1308 · override: tests-out-of-band (red Test-delta gate) — behaviour-preserving header-lift refactor

### What escaped
`feat(core): AppController fan-in Phase 1` (#1308) merged to `develop` with a `tests-out-of-band`
label dismissing a genuinely-RED `Test-delta gate`. The gate fired because the diff touched
`Source/Core/src/AppController*.cpp` (3 TUs) with **zero** paired `tests/Core/*.test.cpp` delta.

### Root cause
Not a defect — the diff is a pure include / forward-declaration restructuring with **no behaviour
change**. `AppController.h` swaps `<nlohmann/json.hpp>` → `<nlohmann/json_fwd.hpp>` and
`#include "LocalCacheManager.h"` → a `class LocalCacheManager;` forward-decl (the
`unique_ptr<LocalCacheManager>` member is fine with the incomplete type — `~AppController` is
out-of-line, identical to the ~16 other fwd-declared `unique_ptr` members), plus IWYU direct
includes pushed into the 3 calling TUs and a new `appcontroller_fan_in_audit.py` ratchet gate.
There is no testable logic delta to pair a unit test with — the win (recompile blast-radius
shrink) is verified by the build itself: dual-target × dual-toolchain (`SmatchetStandalone` GL
json-less PCH + `SmatchetCore_DX12`, MSVC 14.38 + Clang) all link clean. Same
behaviour-preserving-header-lift class as #1016 / #1083 / #1096 (residue (a) of the Test-delta
override family): `coverage-delta-gate.sh` `_classify_diff` counts production `.cpp` churn with no
`*.test.cpp` delta as a fail and still has no auto-exemption for a behaviour-preserving cross-TU
refactor, so the `tests-out-of-band` override was applied — correctly.

### Preventing gate
**none — override legitimate** (pure include/forward-decl restructuring, no behaviour change, no
testable-logic delta; the recompile-blast-radius win is verified by the green dual-target ×
dual-toolchain build, not by a unit test). This is the **4th** ledgered instance of residue (a) —
the behaviour-preserving cross-TU refactor the Test-delta gate cannot distinguish from an untested
logic change (after #1016 / #1083 / #1096). No NEW gate is filed; instead the existing follow-up
(`coverage-gate-platform-else-arm-exemption`, residue (a), already bumped to P2 on #1096) gets a
4th-recurrence note reinforcing that an auto-honoured "behaviour-preserving-refactor" exemption is
overdue.

### Filed as
4th-recurrence priority note appended to [`tooling.md`](categories/tooling.md)
`coverage-gate-platform-else-arm-exemption` residue (a) (P2; recurrence set now
#1016 / #1083 / #1096 / #1308). No new category entry — the gate is already tracked.

## 2026-06-16 · PR #1301 · discretionary (advisory-red, NOT an owed escape) — merged past a RED `Fuzz smoke` whose libFuzzer driver failed to compile

> Flagged **discretionary**, not owed: `Fuzz smoke (Linux libFuzzer)` was a non-required ADVISORY
> lane, so `postmortem-owed.sh` correctly owes nothing for #1301 (advisory red ≠ gate escape).
> Logged here only because the red was a *real* broken-develop build, not a false red — exactly
> the breakage an advisory lane is meant to eventually gate. This PR promotes the deterministic
> half so the next #1301 blocks.

### What escaped
#1301 merged to `develop` past a RED `Fuzz smoke` check. The check went red because a libFuzzer
driver **failed to compile** (a stale `include/` path after a header move) — a genuinely broken
fuzz-driver build on `develop` — but the check was non-required and absent from the meant-to-block
allow-list, so the gate-poller waved it through.

### Root cause
The `Fuzz smoke` job folds a DETERMINISTIC half (configure → build drivers → `ctest -runs=0`) and
a STOCHASTIC half (time-boxed libFuzzer run) into ONE check name. It was kept fully advisory
because naively allow-listing the whole check would also gate the stochastic fuzz run — the exact
poller-jam that got the Mesa `Bucket-` lanes removed 2026-06-15. So the deterministic compile —
which IS a real-breakage signal — had no enforcement.

### Preventing gate
Make the deterministic build/ctest a merge blocker WITHOUT gating the stochastic run, in one
atomic change: (1) add `continue-on-error: ${{ github.event_name != 'schedule' }}` to the
workflow's time-boxed fuzz STEP — a stochastic PR crash is masked (advisory) while configure /
build-drivers / `ctest -runs=0` carry NO `continue-on-error`, so a real compile break still reds
the check; the nightly `schedule` keeps the step HARD-fail so the tracking Issue still opens;
(2) add `Fuzz smoke` to `MERGE_GATES_BLOCK_ALLOWLIST_RE` in `merge-gates.sh`. The check now reds
(and blocks) ONLY on a deterministic build/compile/ctest failure — the #1301 class — and the
paths-scoped `pull_request` trigger means it only appears on fuzz-relevant PRs. Both halves MUST
ship together (allow-listing without the step-guard would re-create the poller-jam); they do, in
this PR, with two `merge_gates.bats` cases locking the FAILURE-blocks + IN_PROGRESS-pending
contract.

### Filed as
This entry + the workflow + allow-list change shipped together in this PR (discretionary hardening
— no category backlog entry; the gate ships with the postmortem).

## 2026-06-15 · merge-watcher daemon (latent post-merge bug; fix branch `fix-merge-watcher-daemon-crash`) · unhandled `gh pr merge --auto` subprocess timeout crashed the whole daemon, stranding every registered PR

> Shared-infra outage (not a red-check escape): the long-lived `smatchet-merge-watcher`
> daemon died mid-run ~2026-06-15 and stopped polling ALL registered PRs. A
> `subprocess.TimeoutExpired` from `squash_merge_pr`'s `gh pr merge --auto` (`timeout=60`)
> escaped `handle_pass`'s `except RuntimeError`, unwound `daemon_loop`'s unguarded per-PR
> body past `except StopSignal`, reached `main`, and crashed the process.

### What escaped
No gate covered the daemon's crash-resilience. `squash_merge_pr` (merge-watcher.py) ran
`gh pr merge --auto` under `timeout=60` with NO try/except, while every caller relies on
the single-`RuntimeError` contract (`handle_pass`: `except RuntimeError`).
`subprocess.TimeoutExpired ⊂ subprocess.SubprocessError`, NOT `RuntimeError` — so it
slipped the contract. `daemon_loop`'s `for entry in entries:` body was likewise unguarded
(only `write_state` caught `OSError`), so the escaped exception unwound the whole loop past
the outer `except StopSignal` into `main`. No bats test exercised a squash timeout or a
per-PR exception — the crash path was untested.

### Root cause
Two-layer gate hole. (1) **Source contract not honored** — `_gh_json` (merge-watcher.py:644)
already normalizes `(OSError, subprocess.SubprocessError) -> RuntimeError` precisely so
callers' `except RuntimeError` holds; `squash_merge_pr` was added later as the queue-safe
`--auto` path and did NOT adopt that normalization, leaving a timeout/launch failure as a
raw non-`RuntimeError`. (2) **No loop backstop** — the per-PR body had no per-iteration
guard, so ANY unhandled exception from ANY sub-handler (not just this one) crashed the whole
daemon and stranded every other registered PR. The blast radius (all PRs, one shared daemon)
is the structural defect; the timeout was only the trigger.

### Preventing gate
Five, all in this PR. (1) **`squash_merge_pr` normalizes launch/timeout to `RuntimeError`**
(mirrors `_gh_json`) — the timeout now degrades to a clean `merge_failed` + retry. (2)
**Per-PR backstop** — the loop body is extracted to `process_registered_pr()` and wrapped:
`except StopSignal: raise` (clean Ctrl-C/SIGTERM shutdown MUST propagate) BEFORE
`except Exception: WARN + continue` — so one PR's failure can never crash the daemon or
strand the others. (3) **Per-CYCLE backstop** — the cycle body (`read_registry()` + the
per-PR loop) is wrapped in the same `except StopSignal: raise` / `except Exception: WARN +
skip-cycle` shape, because `read_registry` runs at cycle scope OUTSIDE the per-PR try and
raises a bare `RuntimeError` (malformed/non-list registry) or an uncaught `OSError`
(a concurrent session's tempfile-rename write / a transient Windows file lock) — the SAME
all-PRs-stranded crash class. Gates (2)+(3) together are the load-bearing structural gate:
every unhandled exception at PR scope OR cycle scope now degrades to a retry, never crashes
the daemon. (This cycle-scope hole was caught by an adversarial Workflow verification pass on
the diff BEFORE merge — itself the "gate, don't trust" pattern working.) (4) **Cascade
narrow-`except` widened** — a second adversarial Workflow pass surfaced a sibling instance one
layer down: `cascade_update_child`'s `gh api PUT update-branch` runs `subprocess.run(timeout=30)`
un-normalized, and the post-merge cascade loop guarded it with `except TimeoutError` ONLY.
`subprocess.TimeoutExpired` is a `subprocess.SubprocessError`, NOT a `builtins.TimeoutError`,
so a hung child slipped that clause and unwound out of `handle_pass` MID-cascade — contained by
gate (2) (daemon survives) but masked: every sibling after the hung child silently lost its
update-branch dispatch, surfacing only as a coarse per-PR WARN. Widened to
`except (TimeoutError, OSError, subprocess.SubprocessError)` so one hung child degrades to a
per-child ERR row and the cascade still dispatches to its siblings. (5) **7 bats regression
tests** (merge_watcher.bats): squash `TimeoutExpired`→`RuntimeError`; `handle_pass`
→`merge_failed` on squash timeout; per-PR backstop logs + continues across PRs; per-PR
StopSignal re-raise; per-cycle `read_registry` raise logs + continues; per-cycle StopSignal
re-raise; cascade hung-child `TimeoutExpired` degrades per-child + loop continues to siblings —
the two StopSignal tests are ordering guards that fail if the `except` clauses are swapped.

### Filed as
[`infra.md`](categories/infra.md) 2026-06-15 `daemon-loop-per-iteration-backstop-audit`
(P2) — audit the other agentic daemons for a missing per-iteration backstop + the
timeout-bearing `subprocess.run` sites that feed `except RuntimeError`-only callers.

## 2026-06-15 · PR #1265 · `cr-out-of-band` after CodeRabbit rate-limit skipped a code/CI harness review

### What escaped
PR #1265 (`Stabilize spawned UI test runner`, head `b271e5373`, merge
`9fd92514`) changed five files across CI, the spawned UI-test command path, the
standalone hidden loop, and a regression test. CodeRabbit selected those files
for processing, then posted its `Review limit reached` rate-limit comment and did
not produce an inline review. The merge gate was run and all CI was green
(including Bucket-E and the label-triggered Perf PR-fast rerun), but the
CodeRabbit condition was waived with `cr-out-of-band`. The escaped class is not a
known product defect; it is a code-bearing PR landing without the intended
automated CodeRabbit review signal.

### Root cause
CodeRabbit review capacity is external and the hard signal arrived only after the
PR existed. The existing `pr-burst-guard` cheapest form is advisory and counts
current open-PR pressure; it does not detect CodeRabbit's own rate-limit comment,
pause a code PR until review capacity returns, or require an explicit
rate-limit disposition before accepting `cr-out-of-band`. Once CR had skipped the
review, `cr-out-of-band` was the documented narrow escape hatch. The waiver
decision was deliberate and bounded by local validation, user validation, and a
green merge-gate run, but it still suppressed the CR signal the gate normally
expects.

### Preventing gate
Add a code-PR CodeRabbit rate-limit pause/disposition gate: when CR posts a
rate-limit / `review-skipped` comment on a non-pure-docs PR, the ship loop should
either wait/retry after the reported cooldown or require a `cr-disposition:`
marker recording the substitute review evidence before `cr-out-of-band` can be
treated as sufficient. This is the richer, load-bearing follow-up to the existing
`pr-burst-guard`; the pure-docs auto-downgrade remains a separate safe case.

### Filed as
[`infra.md`](categories/infra.md) `cr-rate-limit-code-pr-auto-pause` (new) +
[`process.md`](categories/process.md) `cr-out-of-band-disposition-trail`
(recurrence: #1265 had no disposition marker because CR skipped before review).

## 2026-06-14 · PR #1230, #1235, #1240 · direct/manual merge past PENDING→RED `Sanitizer (ASAN via MSVC)` — the non-required-Sanitizer direct-merge-bypass the #1242 poller fix structurally could not reach (now closed by the live required-context bind)

### What escaped
Three security-hardening PRs squash-merged to `develop` in a 6-min window — #1230
`fix(logging): redaction hardening` (22:09:06Z), #1235 `fix(security): network/bounds
hardening (Whisper redirect, cleartext base, MCP SSE bound)` (22:12:33Z), #1240
`harden(ui): cap image-preview dimensions at 16384` (22:15:36Z) — each with `Sanitizer
(ASAN via MSVC)` non-terminal/red at merge. All three: `autoMergeRequest=null`,
`mergedBy=alexandrosk0` → landed via a **direct/manual `gh pr merge` / `gh api … PUT
…/merge`**, NOT `--auto`. They merged in the window AFTER #1237 (22:01Z) but BEFORE the
#1242 `$blocking`-pending poller fix landed (22:33Z). Same non-required Sanitizer lane as
#1220/#1229/#1237; the product fixes are sound (security-hardening, each carries its own
coverage — cross-ref security.md #1230/#1235), no GitHub Issue owed.

### Root cause (blameless)
The #1242 fix that closed the four sibling escapes (#1237/#1232/#1227/#1220/#1198) extended
the **poller's** pending count to the `$blocking` set — but the poller is consulted ONLY on
the `--auto` / scripted path. These three merged via a **direct/manual merge**, which never
invokes the poller at all: GitHub-native branch-protection enforced only the 6 required
contexts at that moment, and `Sanitizer (ASAN via MSVC)` was NOT among them (the #1130
option-A Sanitizer half, still unbound at 22:09–22:15Z). So a direct PUT resolved "all
required green" and merged past the in-flight/red Sanitizer — the exact non-poller-merge-path
residual the #1237 entry explicitly predicted ("GitHub's native auto-merge has the same blind
spot … those checks are non-required") but that #1242 (a poller-only fix) structurally could
not reach. The escape is the MECHANISM (a direct merge bypasses every poller-side guard), not
a poller bug.

### Preventing gate
The gate already existed as a filed-but-unbound action — `sanitizer-required-context`
(tooling.md, the #1130 option-A Sanitizer half) — and it is now **bound live**: develop
branch-protection `required_status_checks.contexts` enforces **9** contexts incl. `Sanitizer
(ASAN via MSVC)`, `Sanitizer (UBSan via Clang)`, and `Coverage (windows-2022 +
OpenCppCoverage)` (confirmed `gh api …/required_status_checks` 2026-06-15). A required context
gates EVERY merge path — `--auto`, the merge button, AND a direct `gh api … PUT …/merge`
(GitHub refuses the merge until the required context is terminal-green) — so it closes the
direct-merge-bypass the poller never could. No new poller knob needed: promotion-to-required
is the correct hammer for the direct-merge path (vs the `Bucket-*` lane, intentionally
advisory → #1218's inverse downgrade-label route). **Residual now LIVE:** the bind landed
BEFORE its documented dependency — the ASAN-unsafe deep-nest fixture (`kDeepAdfDepth=400`,
still unhardened) — so the now-required Sanitizer lane can false-red and deadlock-by-reflex
every merge until the fixture is hardened. That prerequisite is therefore elevated P2 → P1.

### Filed as
[`tooling.md`](categories/tooling.md) `sanitizer-required-context` — status flipped to
**applied + bound (live — 9 required contexts confirmed 2026-06-15)**, escape list extended
#1230/#1235/#1240 (direct-merge-bypass variant); [`test.md`](categories/test.md)
`adf-deep-nest-fixture-asan-unsafe` — elevated **P2 → P1** (deadlock risk now live: Sanitizer
required while the fixture is still unhardened). No GitHub Issue — the three product fixes are
sound.

## 2026-06-14 · PR #1218 · force-merged via direct REST PUT past a CANCELLED `Bucket-E` UI-tests check (no scoped override label exists)

> User-authorized force-merge while landing the issue-comments feature (Jira
> read+count slice, PR-B of #1217/#1218/#1219). #1218 squash-merged `1aa25cf15`
> (2026-06-14T22:02:09Z) via `gh api -X PUT …/pulls/1218/merge` while `Bucket-E`
> was terminal **CANCELLED** (the 30-min job timeout killing a ~21-min Mesa
> software-GL startup hang, reproduced 2×). All 6 required branch-protection
> checks — incl. `Sanitizer (ASAN via MSVC)` — were green.

### What escaped
The poller's meant-to-block `Bucket-*` rule. `merge-gates.sh:374` blocks a red
`Bucket-*` (block-list `Coverage|Sanitizer|Bucket-|Perf PR-fast|Android security
gate`). The direct `gh api …/merge` path gates on the 6 GitHub *required* contexts
only — it never consults the poller — so the CANCELLED Bucket-E (non-required) did
not stop the merge. Same non-poller-merge-path class as #1130 (Coverage) and
#1220/#1229 (Sanitizer).

### Root cause
Two layers. (1) **Env-outage producer** — Bucket-E is dead env-wide
(`bucket-mesa-exe-boot` P1, infra.md): the Mesa software-GL exe can't boot / hangs
~21 min at the scenario step until the 30-min job timeout kills it (GH reports a
timeout as CANCELLED); zero terminal bucket-E in ~40 runs across all branches; CI's
own label "dead harness, not a flaky test". PR-B adds zero UI and bucket-E
exercises only mock/standalone backends, so the lane never touched the diff.
(2) **No scoped override** — of the five block-list members only `Test-delta gate`
and `Perf PR-fast` carry a downgrade label (tests/perf-out-of-band, `:376-378`);
`Bucket-*` (and `Coverage`/`Sanitizer`/`Android security gate`) have none. With the
lane un-greenable and no label to wave it through the poller, the only path to land
an otherwise-green PR was a raw direct PUT — which bypasses the ENTIRE block-list,
not just the one dead lane (the same keystroke would equally have passed a real red
Coverage/Sanitizer). The override DECISION was legitimate (confirmed env-outage,
zero diff exposure, all required green); it was executed via an unscoped mechanism
only because the scoped one does not exist.

### Preventing gate
Add a `bucket-out-of-band` (UI-lane) downgrade label to `merge-gates.sh` mirroring
tests/perf-out-of-band, so a confirmed env-outage on an advisory `Bucket-*` lane is
waved through the AUDITABLE poller path (labeled, logged in the `WARN: out-of-band
label(s) downgraded …` line, post-merge-strippable, postmortem-tracked) instead of
a raw direct PUT that escapes every block-list entry. NOT the #1130/#1220 "promote
to required" route — a `Bucket-*` lane is intentionally advisory (a 30-min Mesa lane
on every PR is wrong), so it needs the inverse: a scoped downgrade. Belt-and-
suspenders: a direct-PUT guard that refuses the merge while any block-list check is
non-green absent its matching out-of-band label (the general form, closing the
raw-PUT hole for all block-list members). The root-cause boot fix
(`bucket-mesa-exe-boot`) is the real cure — it removes the NEED for any override;
this gate makes the rare, legitimate override safe until then.

### Filed as
[`docs/self-improvement/categories/tooling.md`](categories/tooling.md) (2026-06-14, P2 — `bucket-ui-lane-out-of-band-label`: scoped downgrade label for advisory `Bucket-*` lanes + optional direct-PUT guard).

## 2026-06-14 · PR #1229 · merged while `Sanitizer (ASAN via MSVC)` was still PENDING — the non-required Sanitizer lane (same #1130 option-A hole as #1220) + the inherited ASAN-unsafe fixture

### What escaped
`fix(security): canonicalize alternate IP encodings before SSRF denylist (audit #14)` (head `a87ba4ea`) squash-merged to `develop` at 17:18:02Z while **Sanitizer (ASAN via MSVC)** was still running — the check started 17:17:28Z and did not reach terminal FAILURE until 17:23:50Z, ~6 min AFTER the merge. No override label. This is the merge-while-pending variant of the #1220 escape (which merged past a *terminal* red): the same non-required Sanitizer lane neither blocks a red NOR makes the merge wait for it to finish. `Sanitizer (UBSan via Clang)` passed (17:25:22Z). The product change (SSRF IP-canonicalization + 68 lines of new `AiEndpointSanitize.test.cpp`) is sound and tested.

### Root cause (blameless)
Same two holes as #1220 — the product fix is NOT implicated (no GitHub Issue owed):
1. **The red is a pre-existing fixture stack-overflow in the shared doctest rig, not a heap finding in the new SSRF parser.** The ASAN log shows `AddressSanitizer: stack-overflow` (no symbolized frames; MSVC ASAN aborts a stack-overflow without a backtrace) inside the `smatchet_tests` binary — the recursion-depth fixture class, same as #1220 / #1183 / #1215. The new IP-canonicalization code is non-recursive string parsing; ASAN found nothing in it. So #1229 *inherited* a red from a fragile fixture it did not touch.
2. **Sanitizer is non-required on every merge path, so the merge never waited for it.** Because `Sanitizer (ASAN via MSVC)` is absent from `project.config.json` `required_contexts` and live GitHub branch-protection, `gh pr merge --auto` resolved "all required contexts green" and merged at 17:18Z without waiting the ~6 min for Sanitizer to finish — a required context would have BLOCKED the merge until the lane reached terminal-success. This is the #1130 option-A Sanitizer half (never applied), the same hole that hit #1183 / #1210 / #1211 / #1220 on the ASAN lane.

### Preventing gate
Identical to #1220 — #1229 is a second same-day datapoint, not a new class:
- **Promote `Sanitizer (ASAN via MSVC)` (+ `Sanitizer (UBSan via Clang)`) to required contexts** (the #1130 option-A Sanitizer half). A required Sanitizer cures BOTH variants: it blocks a terminal red (#1220) AND forces the merge to wait for the lane to finish (#1229's merge-while-pending). Filed in `tooling.md` (`sanitizer-required-context`) — #1229 added to its escape list. NOTE the ordering dependency: harden the ASAN-unsafe fixture FIRST, else a required-but-flaky Sanitizer deadlocks merges.
- **Make the ADF deep-nest regression fixture ASAN-safe** (the inherited overflow source) so the shared rig stops false-reddening unrelated PRs. Filed in `test.md` (`adf-deep-nest-fixture-asan-unsafe`).

### Filed as
Folded into the #1220 filings (no new entries needed — same class): [`tooling.md`](categories/tooling.md) `sanitizer-required-context` (P1; escape list now #1220 + #1229) + [`test.md`](categories/test.md) `adf-deep-nest-fixture-asan-unsafe` (P2). No GitHub Issue — the SSRF product fix is sound and carries its own regression test.

## 2026-06-14 · PR #1227 · merged past RED `Coverage` + `Pillar 2 scanner` — #1130 `coverage-required-context` applied in config but never bound on GitHub, + two CI-infra false-reds

### What escaped
`fix(config): harden POSIX secret-at-rest perms` (head `d59e975d`) squash-merged to `develop` at 16:40Z with TWO terminal-RED checks on its head and **no override label**: **Coverage (windows-2022 + OpenCppCoverage)** (FAILURE 16:20Z) and **Pillar 2 scanner** (FAILURE 16:17Z). A third, non-required **Mobile texture-guard smoke (Mesa headless GL)**, completed FAILURE at 16:47Z — after the merge. `postmortem-owed.sh` flagged only Coverage; the Pillar-2 red was under-counted by the nudge. All three reds are CI-infra / environment fragility, verified — no product defect.

### Root cause (blameless)
1. **The #1130 `coverage-required-context` gate is applied in config but NOT bound on GitHub — so it does nothing for the exact merge paths that keep escaping.** #1130's preventing gate landed `Coverage` into `project.config.json` `required_contexts` + `ci.required_checks` (marked `applied 2026-06-14`), but its own resolution flagged that a **maintainer must run `setup-branch-protection.sh`** to make the context binding for GitHub-native merge paths — and that bind never ran. Live `repos/.../branches/develop/protection/required_status_checks.contexts` still enforces only six contexts (Test-delta gate · Windows + MSVC · Windows + MSVC (light) · Shell lint · Doc anchors + agent contract · Perf PR-fast) — Coverage (and Sanitizer) absent. So `gh pr merge --auto`, which waits on GitHub-required contexts only, sailed straight past the red Coverage exactly as in #1130 / #923. The fix is code-complete but GitHub-inert.
2. **The Coverage red was an OpenCppCoverage infra crash, not a real threshold miss** — `0% - 1 hit, 544 misses`, `No files were found … coverage/coverage.xml` (the instrumented binary produced no coverage data). The job reports an instrumentation crash identically to a genuine threshold failure, so an infra crash is indistinguishable from real breakage at the rollup.
3. **The Pillar-2 red was a transient base-ref failure, fail-closed by design** — `Pillar 2 scanner: FAIL — 'git diff origin/develop...HEAD' exited 128. Refusing to skip the scan on a diff error.` (the #518 fail-closed guard tripping on a fetch / shallow-clone failure, NOT a real UI-thread sync-I/O finding). Correct to fail closed; but a transient infra failure on a non-required scanner then merged past.

### Preventing gate
- **Bind the already-applied Coverage required-context** — maintainer runs `setup-branch-protection.sh` so live GitHub branch-protection matches `project.config.json`. Until bound, the "applied" `coverage-required-context` gate is inert for `--auto` / direct-merge. (Reinforced in the `coverage-required-context` tooling entry — recurrence logged, escalated P2 → P1, `Status` corrected to "applied (config only — GitHub bind owed).")
- **Harden the flaky producers so a transient infra failure cannot masquerade as a real red** — OpenCppCoverage "no / empty `coverage.xml`" must be a distinct, retried, clearly-labelled infra failure (not a `0%`-threshold red); diff-based gates (Pillar-2 scanner) must fetch the base ref robustly / retry before fail-closing on exit-128. Filed in `infra.md`. **Diff-gate half SHIPPED 2026-06-14 (feat/pillar2-fetch-depth):** the exit-128 was self-inflicted — the gates re-shallowed develop with `git fetch --depth=1 origin <base_ref>` after a `fetch-depth: 0` checkout, breaking the three-dot merge-base. Dropping `--depth=1` across all 5 diff consumers (`pillar2-scan.yml`, `dup-scan.yml`, `build-and-test.yml` ×2, `perf-pr-fast.yml`) + a `tests/bats/delta_gate_base_ref_fetch.bats` reintroduction-sweep closes it; no retry/unshallow needed. Coverage-producer half (a) still open.

### Filed as
[`infra.md`](categories/infra.md) (2026-06-14, P2 — CI-infra-flake reds masquerade as real breakage: OpenCppCoverage crash-vs-threshold + diff-gate base-ref robustness + the recurring Mesa Launch-smoke flake) + reinforced [`tooling.md`](categories/tooling.md) `coverage-required-context` (recurrence #1227; GitHub bind owed; P2 → P1).

## 2026-06-14 · PR #1220 · merged past RED `Sanitizer (ASAN via MSVC)` — Sanitizer never got the #1130 option-A promotion, + an ASAN-unsafe regression fixture

### What escaped
`fix(tracker): bound ADF parser recursion depth (security MEDIUM / Pillar 3)` (final head `2294521b`) squash-merged to `develop` at 16:17Z with **Sanitizer (ASAN via MSVC)** terminal-FAILURE (16:08Z, on the final commit — 9 min before merge, not a stale-check race) plus a non-required **Mobile texture-guard smoke (Mesa headless GL)** Launch-smoke flake. No override label.

### Root cause (blameless)
Two holes; the product fix itself is sound (no GitHub Issue owed):
1. **The escaped red is the regression FIXTURE, not the walker.** The walker is correctly bounded (`kMaxAdfRecursionDepth = 256`, threaded through both `CollectAdfText` + `ExtractAdfTextToStream`, graceful truncate + one-shot `WarnAdfDepthCapped()` WARN). The ASAN stack-overflow (`std::_Char_traits<char,int>::move`) is in the *test fixture*: a deep `nlohmann::json` tree whose recursive ctor/dtor overflows under ASAN's inflated frames. The author already diagnosed the fixture-vs-walker split (capped `kDeepAdfDepth` 5000 → 400) but 400 is still too deep under ASAN's instrumentation. Same ASAN-frame-fragility class as #1183 / #1215 (a regression guard that itself overflows under ASAN), NOT an escaped product bug.
2. **Sanitizer is non-required on every merge path — #1130 option-A promoted Coverage ONLY, never Sanitizer.** `Sanitizer (ASAN via MSVC)` is absent from both `project.config.json` `required_contexts` and live GitHub branch-protection, so `gh pr merge --auto` (required-contexts-only) merged straight past the red. **Lane-recovery confirmation (owed by the #1210/#1211 postmortem):** #1220 is the first PR with a head built past #1215 to run the ASAN lane — it confirms the #1183/#1210/#1211 ReDoS-timing false-red is GONE (no `CHECK(elapsedMs < …)` red), but the lane re-reddened for a *new* cause (the fixture overflow), and the same non-required structural hole let the new red through.

### Preventing gate
- **Give `Sanitizer (ASAN via MSVC)` (+ `Sanitizer (UBSan via Clang)`) the same option-A promotion Coverage got** — into `project.config.json` `required_contexts` + `ci.required_checks` with a docs-only self-gate companion so they don't deadlock, then the maintainer branch-protection bind. This is the #1130 option-A **Sanitizer half**, never applied. Filed in `tooling.md` (`sanitizer-required-context`).
- **Make the ADF deep-nest regression fixture ASAN-safe** — build the deep tree without `nlohmann::json`'s recursive ctor/dtor (manual iterative teardown), or guard the fixture depth under `__SANITIZE_ADDRESS__`, so the regression guard cannot itself stack-overflow under ASAN. Filed in `test.md`.

### Filed as
[`tooling.md`](categories/tooling.md) (2026-06-14, P1 — `sanitizer-required-context`: promote Sanitizer ASAN/UBSan to required contexts; #1130 option-A Sanitizer half) + [`test.md`](categories/test.md) (2026-06-14, P2 — ASAN-safe ADF deep-nest fixture).

## 2026-06-14 · PR #1212 · `tests-out-of-band` shipped a security behaviour change with no regression test (#1124 class)

### What escaped
`fix(tracker): disable redirect-following so the API token can't leak cross-host (security H4 / E2)` squash-merged to `develop` at 14:44Z under a `tests-out-of-band` label — changing product (`TrackerHttpUtils.{h,cpp}`, `JiraIssueMutation.cpp`) with **no `*.test.cpp` delta**. The `Test-delta gate` was FAILURE @ 10:27Z then SUCCESS @ 11:16Z on the *same* SHA (no new commit between). A non-required **Mobile texture-guard smoke** Mesa flake was also merged past.

### Root cause (blameless)
The new no-follow redirect policy (`MakeTrackerRedirectPolicy()`, `TrackerHttpUtils.h:27`, applied at `JiraIssueMutation.cpp:593`) is a **security property** — an attacker-controlled cross-host redirect must not carry the API token to the redirect target. It shipped with zero regression test (`tests/` contains no redirect-policy test — confirmed by recursive search; every `redirect` hit is unrelated `TestEnvGuard` directory-redirect noise), the test-delta requirement dismissed by `tests-out-of-band`. The override may itself be legitimate — a redirect-no-follow regression test needs an HTTP-redirect fixture that did not exist — but `tests-out-of-band` is a blunt instrument: it suppresses the requirement and creates **no durable obligation** to add the deferred test, so a security fix's regression test silently never gets written. Same class as #1124 (a legitimate override whose self-declared follow-up never happened). The preventing gate is therefore **not** "none / moot" — the owed test must be *tracked*, not evaporate.

### Preventing gate
A `tests-out-of-band` / `perf-out-of-band` override on a diff touching a **strict-zone / security trust-boundary** path must auto-file a tracked deferred-test obligation (a `test.md` entry or GitHub Issue) at ship — wired into the existing override-snapshot machinery (`mandatory-merge-snapshot-on-override-merge`) so the merge that consumes the label also records the owed test. Plus the concrete owed test here: a regression asserting tracker requests do not follow a cross-host redirect (auth header / token not re-sent to the redirect target). Filed in `tooling.md` + `test.md`. (The non-required Mesa Launch-smoke flake is the same structural non-required hole tracked by `sanitizer-required-context` / `coverage-required-context`.)

### Filed as
[`tooling.md`](categories/tooling.md) (2026-06-14, P2 — `out-of-band-on-trust-boundary-owes-tracked-test`; reinforces #1124 + `postmortem-owed-moot-override-false-positive`) + [`test.md`](categories/test.md) (2026-06-14, P2 — redirect-no-follow security regression test).

## 2026-06-14 · PR #1210, #1211, #1207, #1198 · auto-merged past RED Sanitizer (ASAN via MSVC) — same flaky ReDoS-timing class as #1183, in the gap before the fix (#1215) landed

### What escaped
Two PRs squash-merged to `develop` with the meant-to-block **Sanitizer (ASAN via MSVC)** lane RED and **no override label**: **#1211** (`fix(tracker): escape server-supplied values in JQL literals`, head `402998ea`) at 10:33Z, and **#1210** (`fix(security): contain AgentsMd override paths`, head `aa9e4f92`) at 11:31Z. Both had all six GitHub-**required** contexts green (Test-delta gate · Windows + MSVC · Windows + MSVC (light) · Shell lint (shellcheck) · Doc anchors + agent contract · Perf PR-fast). `Sanitizer` is on the merge-gates "meant-to-block" allow-list (`merge-gates.sh`) but is **not** a GitHub-required context — so the GitHub-native auto-merge path (`gh pr merge --auto`, which waits on required contexts only) merged both straight past the non-required ASAN red. Same flaky assertion as #1183: the `CallstackParser.test.cpp` ReDoS-sentinel wall-clock cap blown by ASan's instrumentation slowdown (a timing false-red — no `AddressSanitizer:` marker in either log). **Two further PRs (added on the owed-nudge's second pass) hit the identical sentinel false-red in the same pre-#1215 window and merged past the non-required ASAN lane the same way:** **#1207** (`feat(shortcuts): rebindable keyboard-shortcut foundation (PR1, behavior-neutral)`, head `3c5cc519`) at 10:58Z, and **#1198** (`fix(ui): reset layout rebuilds docking live instead of orphaning nodes`, head `f55f62bb`) at 11:37Z — both red on the exact same `CallstackParser.test.cpp(304) CHECK(elapsedMs < 2000)` (verified in both ASAN logs; no `AddressSanitizer:` marker). #1207's head predated #1215; #1198 merged ~7 min after #1215 reached `develop` but its own head was built before the fix, so the lane stayed red — a reminder that strict-off "merge the moment own-head checks pass" lets an in-flight PR escape on a flake already fixed on `develop`. Neither PR's product code (keybindings foundation / docking-reset) is implicated.

### Root cause (blameless)
Identical false-red to #1183 (the entry directly below): the ASAN lane's first hard step **"Run ctest under ASAN"** (`build-and-test.yml:956`, `ctest --output-on-failure`) runs the FULL `smatchet_tests`, including the ReDoS sentinel whose fixed `CHECK(elapsedMs < 2000)` deterministically blows under ASan's ~2-3× instrumentation slowdown. The structural hole is also identical: the **automated required-only merge path** (`--auto`) does not block on the non-required Sanitizer lane — so, unlike #1183's human-manual override, #1210/#1211 escaped with **no human in the loop and no override label** to mark the decision. The aggravating factor is **timing**: #1183's postmortem (filed 00:45Z, landed via #1209 at 09:47Z) had ALREADY diagnosed this exact flake and prescribed the fix, but the fix (#1215) did not land until 11:30Z. #1211 (10:33Z) and #1210 (11:31Z — one minute after #1215, but on a head built before it) both escaped inside that ~10 h window between diagnosis and fix-landing.

### Preventing gate
The root-cause false-red fix **landed as #1215** (`36521f72`, merged 11:30Z; verified an ancestor of `develop` HEAD): it wraps the ReDoS sentinel in `#if defined(__SANITIZE_ADDRESS__)` and widens the budget `2000 ms → 20000 ms` (`CallstackParser.test.cpp:309-315`). This is the exact "sanitizer-aware budget" remedy the #1183 postmortem prescribed, applied at the **test level** — which is strictly better than the prescribed "move the `--test-case-exclude` onto the `:956` step", because changing the assertion itself fixes the sentinel in BOTH the full-`ctest` (`:956`) and clean-subset (`:972`) steps at once; that half of #1183's prescription is therefore **mooted, not skipped**. **Verification residual (NOT yet confirmed green):** no completed post-#1215 ASAN run has been observed pass — `develop` merged four PRs in ~10 min (1211 → 1215 → 1210 → 1198) and each post-merge develop ASAN run was cancelled/superseded by the next push, while the only post-#1215 PR that ran the lane (#1198) carried a pre-#1215 head and stayed red. The lane-recovery confirmation is owed by the first PR rebased past #1215 (filed below). **Structural residual (accepted, same as #1183):** binding the automated required-only merge path to the non-required Sanitizer lane — promote it to a required context with a skip companion, or `enforce_admins=true` — is the deferred **#1130 option-A** branch-protection change (deadlock-risky while the lane was flaky; safer to land now that #1215 removed the false-red).

### Filed as
`docs/self-improvement/categories/test.md` — the 2026-06-07 P2 ReDoS-timing entry closed `applied (#1215)`; the 2026-06-13 P1 entry's ReDoS-timing subcase closed via #1215 (its INT_MAX-clamp subcase stays open). Both carry the post-#1215 **lane-green confirmation** as their residual (owed by the first PR rebased past #1215).

## 2026-06-14 · PR #1183 · manually merged past a pending→RED Sanitizer (ASAN via MSVC) check — a known flaky timing test

### What escaped
PR #1183 (`feat(mobile): CI smoke gate for the #1122 texture-guard crash class`) was **manually** squash-merged to `develop` at 00:45 by the human authority while the **Sanitizer (ASAN via MSVC)** check was still PENDING; it completed FAILURE at 00:53 (8 min post-merge). `Sanitizer` is on the merge-gates "meant-to-block" allow-list (`merge-gates.sh`) but is **not** a GitHub-required context, so no GitHub-native merge path blocks on it. `cr-out-of-band` was on the PR — that label downgrades **CodeRabbit only**, never a CI Sanitizer red. The merge landed **37 min after** the `safe-admin-merge-guard` (#1193) reached develop.

### Root cause (blameless)
Two layers, neither a product defect:
1. **The red was a false-red, not a memory-safety finding.** The "Sanitizer (ASAN via MSVC)" job's first step, **"Run ctest under ASAN"** (`build-and-test.yml:956`, `ctest --output-on-failure`, hard — not continue-on-error), ran the FULL `smatchet_tests` and failed on a known-flaky wall-clock assertion: `tests/Core/CallstackParser.test.cpp:304 CHECK(elapsedMs < 2000)` (a ReDoS sentinel — ASan's ~2-3× instrumentation slowdown blows the 2000 ms bound, measured ~3002 ms). No `AddressSanitizer: heap-*` marker anywhere in the log. This subcase was ALREADY known-flaky and `--test-case-exclude`'d — but only from the LATER **"Run sanitized doctest rig (ASan, clean subset)"** step (`:972`), NOT from the full `ctest --output-on-failure` at `:956` that actually produces the lane's red. The exclusion sits on the wrong step.
2. **`safe-admin-merge-guard` is opt-in.** `safe-admin-merge.sh` refuses a merge past a non-green allow-listed check (pending counts as non-green) — but ONLY when the merge is routed through the script. A manual GitHub-UI merge / raw `gh pr merge --admin` / `gh pr merge --auto` (which waits on required contexts only) never invokes it. A human manual merge bypasses the guard structurally, independent of its allow-list logic.

### Preventing gate
The merge itself was a **legitimate human-authority override** of a known-flaky check (AI_POLICY.md — humans own quality + cost): no new merge-time gate is warranted for the human-manual path. Binding that path would require `enforce_admins=true` / promoting the lane to a required_context with a skip companion — the deferred #1130 option-A item, a branch-protection change needing explicit human authorization and deadlock-risky while the lane is flaky. The concrete preventing action targets the ROOT TRIGGER (the false-red that invited the override): **fix the flaky `CallstackParser.test.cpp:304` ReDoS-timing assertion (sanitizer-aware budget / non-sanitized lane) AND move its `--test-case-exclude` onto the load-bearing "Run ctest under ASAN" step (`:956`)** so the meant-to-block Sanitizer lane stops manufacturing false-reds that train override-by-reflex. Residual (accepted, not fixed here): the opt-in nature of `safe-admin-merge-guard` — binding all merge paths is the deferred #1130 option-A branch-protection change.

### Filed as
`docs/self-improvement/categories/test.md` — updated the existing `CallstackParser` ASan-timing entry (2026-06-13, `asan-doctest-rig` wrap-up): the `:304` exclusion does not cover the full-ctest meant-to-block lane (`:956`), which red on #1183; elevated P2 → P1 (now a confirmed gate-escape trigger, not a latent flake).

## 2026-06-13 · PR #1180 · admin-merged past RED Bucket-C + Bucket-E (allow-listed blocking checks)

### What escaped
PR #1180 (the bucket-lane gate-hardening PR) was squash-merged to `develop` at 18:53 via `gh pr merge --squash --admin` while its **Bucket-C screenshot diff** and **Bucket-E UI tests** checks were RED (the new launch-smoke step it added was failing on the Mesa exe-boot). Both `Bucket-*` checks are on the merge-gates "meant-to-block" allow-list (`merge-gates.sh`, #923 fix). `--admin` bypassed GitHub branch protection. Net: a gate the PR itself introduced shipped to develop while red, red-walling bucket-C/E develop-wide (remediated by #1187 making the launch-smoke advisory).

### Root cause (blameless)
The admin-merge had **no programmatic green-check gate**. The orchestrator ran a pre-merge re-confirm as an informational `echo` (`... | if 0-failures "all green" else "FAIL: …"`) and then ran `gh pr merge --admin` as a **separate unconditional statement** — the echo's "FAIL: Bucket-C, Bucket-E" output did not, and could not, stop the merge. The `cr-out-of-band` justification (CR findings verified-fixed in the diff) was sound and unrelated; the failure was that the bucket-C/E reds were **live, real, non-overridden** checks, and the "admin only when everything is actually green" carve-out was not *enforced* — it was asserted in prose and printed, not gated on an exit code. Bare `--admin` is a foot-gun: it bypasses the very rollup the poller checks.

### Preventing gate
A `safe-admin-merge` guard the orchestrator MUST use instead of bare `gh pr merge --admin`: it reads the head's `statusCheckRollup`, and **refuses (non-zero exit, no merge)** if ANY required-or-allow-listed check (`Bucket-*` / `Coverage` / `Sanitizer` / the required contexts) is non-green — only a genuinely stale-BLOCKED-green PR can be admin-merged. This makes "admin-merge past a real red" structurally impossible while preserving the legitimate stale-BLOCKED-green carve-out. Plus the discipline rule: **never gate a merge on an `echo`** — the green assertion must be an exit code (`if ! all_checks_green <pr>; then abort`), never advisory text in the same command as the merge.

### Filed as
`docs/self-improvement/categories/tooling.md` — `safe-admin-merge-guard` (the wrapper + the never-gate-on-echo rule).

## 2026-06-13 · Issue #863 · config-skew sanitizer-nightly break (`-Werror,-Wunused-function`) reached `develop`

> Filed retroactively (the product fix already landed in `61b17427` / PR #945). The
> escaped class — not the specific symbol — is what this gate closes. GitHub Issue #863
> ("Sanitizer nightly failed (ASan+UBSan)"); CI runs `26997373785` / `27053572861` /
> `27083859902` (2026-06-05 → 06-07, three consecutive red nights); green again
> `27118115403` (06-08) onward.

### What escaped
The **PR-time CI gate** (the 5 required Windows MSVC checks). A regression that made
`Source/Core/src/AppController.cpp` fail to **compile** in the Lua-OFF config
(`-Werror,-Wunused-function` on the free function `LogLuaScriptFileProbe`) sailed through
every PR check green and was only caught post-merge by the **nightly Clang ASan+UBSan**
job — which builds the Lua-OFF config the PR jobs never compile. The sanitizer gate did
its job (it red-flagged the break) but **too late** (post-merge, on `develop`, for three
nights). Note also: the nightly auto-Issue (#863) mislabelled a **compile** failure as a
"runtime AddressSanitizer / UBSan report" — the binary never linked.

### Root cause (blameless)
`LogLuaScriptFileProbe(const char*, const std::string&)` had its **two call sites**
wrapped in `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` but its **definition** left
unguarded at file scope. With Lua OFF the definition has zero callers; Clang at `/WX`
promotes `-Wunused-function` to a hard error and the TU never compiles. **Config skew**:
PR-time CI compiles only Lua-ON / MSVC configs, and MSVC `/W4` does not warn on an unused
internal-linkage free function the way Clang `-Wall` does — so no PR-gated job ever
exercised the Lua-OFF `-Werror` path that breaks. The asymmetry (def unguarded, refs
guarded) is invisible to every gate that ran pre-merge.

### Preventing gate
**New PR-time lint `unused-symbol-under-config-guard`** in
`agents/scripts/project/test-lint-rules.sh` (contract-card row in `AGENTS.md` §
Enforcement contract-card). It flags a column-0 free-function **definition** that sits
unguarded while **every** in-file reference is inside the TRUE branch of a **positive**
`#if defined(SMATCHET_WITH_*)` guard — exactly the dead-in-the-feature-OFF-build shape
that trips `-Werror,-Wunused-function`. A pure-bash preprocessor-depth heuristic (no
compiler / AST), modelled on the existing `cmake-local-gate-ci-scope` /
`no-glfw-in-core-headers` lints. `--selftest` + `tests/bats/lint_rules.bats` replay the
`61b17427~1` pre-fix shape (detected on line 293) and the #945 fixed shape (clean); a
regression-replay against the pre-fix `AppController.cpp` confirms it would have flagged
#863.

**Shipped WARN-first (advisory; calibration phase, same path as the DRY `duplication`
gate), scoped to the files CHANGED in the diff — NOT absolute-0.** This is a deliberate,
documented down-scope from the plan's preferred absolute-0: a clean-tree scan during
implementation surfaced benign idioms the per-file text proxy cannot statically separate
from the #863 shape — an out-of-line member def (`Type::method`, excluded via the `::`
discriminator), a real impl in the `#else` of a `#if !defined(SMATCHET_WITH_*)` (excluded
via the positive-guard discriminator), and a helper called only in a `SMATCHET_WITH_MCP`
path inside a TU that is itself MCP-gated (`CliCommandRunner.cpp` — irreducible from text;
3 residual advisory hits). Shipping absolute-0 over those would have **red-walled
develop** — the exact failure the plan's § Verification flagged as CRITICAL ("do NOT ship
a false-positive-prone gate"). WARN-first surfaces the #863 shape at PR time (the signal
the gate exists for) without that risk; the nightly Lua-OFF sanitizer build stays the
authoritative backstop, and the rule graduates to blocking once the FP rate is calibrated
low. A full `SMATCHET_WITH_*` permutation compile matrix was rejected as too costly for
prerelease (revisit if a second config-skew escape lands). `// SMATCHET_DEVIATION(rule=
unused-symbol-under-config-guard; …)` above the def suppresses.

### Filed as
[`docs/self-improvement/categories/infra.md`](categories/infra.md) — "`unused-symbol-under-config-guard`
PR-time lint (config-skew `-Werror` escape, #863)". Plus a separate one-line `infra.md`
backlog entry for the nightly auto-Issue mislabelling compile failures as runtime
sanitizer findings (not implemented this PR).

## 2026-06-11 · PR #1130 · merged past a RED "Coverage" check (non-poller merge path — #923 recurrence)

> Surfaced by the improved `postmortem-owed.sh` (this session's
> `feat/postmortem-owed-cleanup`): PR #1130 ("fix(mobile): guard ImGui 1.92
> dynamic-texture orphans on Android", merge `5fc21b34`) merged at 03:34:54 UTC
> while `Coverage (windows-2022 + OpenCppCoverage)` was terminal **FAILURE** (run
> completed 03:21:42, ~13 min before merge). No override label.

### What escaped
The meant-to-block **Coverage** check. Since #923 (2026-06-06) `Coverage` is on
the curated non-required-but-blocking allow-list in `merge-gates.sh`
(`Coverage|Sanitizer|Bucket-|Perf PR-fast|Android security gate`), so the
**poller** blocks a red Coverage. #1130 reached `develop` past a red Coverage
anyway — i.e. via a merge path that did NOT consult the poller.

### Root cause
#923's chosen remedy was **option (B)** — the poller-side allow-list — and its
**option (A)** (make `Coverage` a GitHub *required* context) was deferred. The
allow-list therefore binds **only poller-mediated merges** (the orchestrator's
`handle_pass` / the merge-watcher's `merge-gates.sh` poll). `Coverage` is still
**non-required** in `branch_protection.required_contexts`, so every GitHub-native
merge path that gates on *required* contexts only — `gh pr merge --auto` (GitHub
auto-merge gates required-only), an admin / direct `gh api …/merge`, or the merge
button — sails past a red non-required Coverage. #1130 took one of those paths.
The #923 fix closed the watcher-poller hole but left the non-poller paths exposed;
this is the same class recurring through the very gap option (A) was meant to close.
(Detection worked as designed — the post-merge detector flagged it; that is how
this entry exists.)

### Preventing gate
Take #923 **option (A)** — promote `Coverage (windows-2022 + OpenCppCoverage)` to
`project.config.json` `branch_protection.required_contexts` + `setup-branch-protection.sh`,
paired with a `coverage-skip.yml` companion (Pattern B) so docs-only / path-filtered
PRs don't deadlock on a never-run Coverage. A *required* Coverage binds **all**
merge paths (`--auto`, button, REST), not just the poller — closing the non-poller
hole. (Branch-protection change → maintainer approval.) Belt-and-suspenders: keep
the now-improved `postmortem-owed.sh` allow-list detection as the post-merge backstop.

### Filed as
[`docs/self-improvement/categories/tooling.md`](categories/tooling.md) (2026-06-11, P2 — promote Coverage to a required context + coverage-skip companion; #923 option A, the deferred half).

## 2026-06-11 · PR-less direct push `90cfd5d6`, `578d21ea` · docs(backlog) commits to develop bypassing PR/CI/CR

> Surfaced by the improved `postmortem-owed.sh` trigger 4 (direct-push detection,
> this session). Two `docs(backlog)` commits reached `develop` with no PR
> (`commits/{sha}/pulls == 0`): `578d21ea` (2026-06-04, "merge-gates poller scores
> absent required-check as pass") and `90cfd5d6` (2026-06-05, "file 2 more
> reduce-agent-prompt-bloat session learnings").

### What escaped
The **PR-only / branch-protection gate** for `develop` — no PR, no CI, no
CodeRabbit. Content was benign (backlog docs), but the gate was bypassed via the
repo owner's admin credentials (which branch protection does not stop).

### Root cause
Identical to the 2026-06-05 `a678741f` escape (same class): branch-state drift /
convenience direct-push to `develop`. The **preventing gate for this class was
already filed** on 2026-06-05 (the `pre-push` develop-guard hook, `tooling.md`)
but has **not yet been implemented** (still `Status: open`), so the class kept
recurring. Both commits predate this detection (2026-06-04/05) but were invisible
until trigger 4 existed — not a new escape *path*, just newly *seen* ones.

### Preventing gate
The already-filed **`pre-push` hook rejecting a `develop`/`main` same-named push
unless `SMATCHET_ALLOW_DEVELOP_PUSH=1`** (a local hard stop admin creds can't
silently bypass) — `tooling.md` 2026-06-05, P2. Recurrence (≥3 instances now:
`a678741f`, `578d21ea`, `90cfd5d6`) → **bump to P1**. No second system.

### Filed as
[`docs/self-improvement/categories/tooling.md`](categories/tooling.md) — recurrence note + P2→P1 bump on the existing `pre-push develop-guard hook` entry (2026-06-05).

## 2026-06-10 · PR #1124 · `tests-out-of-band` — override legitimate, but a self-declared "elevate to GitHub Issue at ship" never elevated

### What escaped
PR #1124 (shorten long UI texts behind `(?)` help-marker tooltips) merged to
`develop` carrying `tests-out-of-band`, which dismissed a **genuinely RED**
`Test-delta gate`. Two things to separate:

- **The override itself was legitimate.** #1124 added real UI logic (the
  `SmatchetHelpMarker` widget + a tab-aware Preferences footer that records
  `preferencesActiveTab`), so a RED Test-delta gate was *correct* — there is new
  untested surface. But the only honest coverage home is bucket-E **hover/tooltip**
  testing (ImGui Test Engine has no hover-surface tests today). The PR did the
  right thing: it filed a detailed `test.md` P2 entry (2026-06-10) with a concrete
  `test-author` next action (`ItemHover` on a marker inside the Assistant disabled
  block, both `Render` paths, FA-atlas-absent fallback). Disposition trail present.
- **The actual escape:** that same `test.md` entry also recorded a **UX Pillar 4
  (Accessibility) regression** — the ~38 long-form explanations moved from
  always-visible inline text to **mouse-hover-only**, so keyboard-only users lost
  access entirely — and stated it "should be elevated to a GitHub Issue at ship
  (user-observable, per `issue-triage.md`)". **That Issue was never filed.** A
  user-observable regression that `issue-triage.md` makes a mandatory GitHub Issue
  shipped with the intent-to-file recorded only in a backlog-row prose sentence no
  gate reads. (Filed retroactively during this postmortem as **#1128**.)

### Root cause
Blameless, two layers:
- **Test-delta has no honest "coverage-is-bucket-E-only" path.** A change whose
  only test home is hover/tooltip surface (which the harness can't yet drive) can
  only ship via `tests-out-of-band` + a backlog entry. That half worked as
  designed — the backlog entry is the disposition trail.
- **The self-elevation marker is unenforced (the real hole).** `issue-triage.md`
  mandates a GitHub Issue for a user-observable regression, but nothing connects a
  backlog/plan row that *declares its own intent to elevate* ("should be a GitHub
  Issue at ship") to an actual Issue. The signal lived entirely in prose; the
  ship-loop closeout never checked it, so the mandatory Issue was silently skipped.

### Preventing gate
- **New (process P2): `ship-time-issue-elevation-check`.** Extend the closeout
  sweep (`issue-sweep.sh`) to grep a merged PR's added
  `docs/self-improvement/categories/*` + `docs/plans/active/*` lines for an
  "elevate to … GitHub Issue" / "should be a GitHub Issue" marker; if found and the
  PR body carries no `Issue: #N` / `Fixes #N` to an open Issue, emit a closeout WARN
  + `[issue-propose]` line. Plus a one-line convention in `issue-triage.md`: a
  self-elevation marker owes either an Issue link in the PR body or a rewrite to
  "deferred — no Issue (reason)". (The Test-delta-bucket-E-only half is **not** a
  new gate — the existing backlog disposition is the correct mechanism.)
- Remediation already applied: missing Issue filed as **#1128** (a11y, P2, area:ui).

### Filed as
- [`categories/process.md`](categories/process.md) — `ship-time-issue-elevation-check` (P2, 2026-06-10, #1124).

## 2026-06-10 · PR #1110, #1095, #1096 · `cr-out-of-band` ×2 + `tests-out-of-band` — three overrides surfaced together by the postmortem-owed nudge

### What escaped
Three PRs merged to `develop` carrying an override label that dismissed a
non-required block. None shipped a defect; the SessionStart `postmortem-owed.sh`
nudge surfaced all three together. They are **three distinct classes** —
disposition differs per PR:

- **#1110** (mobile WS6 close-out, pure-docs) — `cr-out-of-band` dismissed a
  `CR finding gate` block raised by **two CodeRabbit false positives**: (F1)
  flagged the tier-less plan-ref `docs/plans/mobile-mvp-completion.md` as
  "missing `shipped/`", and (F2) claimed `docs/plans/INDEX.md` was out of sync
  ("gate RED"). Both refuted by deterministic ground truth on the head
  (`3cde8676`): `test-plan-ref-integrity.sh` exit 0 ("all 126 referenced plan
  paths resolve" — the tier-less `docs/plans/<slug>.md` form resolves against any
  tier, the intentional **move-proof** convention), and `test-plan-index.sh
  --check` exit 0 ("index up to date") with live CI "Doc anchors + agent
  contract" = success. CR re-flags this convention on every plan-archive PR
  because nothing teaches it the rule.
- **#1095** (ADR-0019 + a shipped-plan archive, pure-docs) — `cr-out-of-band`
  dismissed a CR **review-skipped** block whose cause was CodeRabbit's own
  **"Review limit reached" rate limit**, *not* a finding (CR never reviewed the
  diff). The override was correct, but it was a **manual** step on a pure-docs PR
  that CR could not have meaningfully reviewed anyway. (The `Test-delta gate`
  showing `cancelled` on this head is a CI concurrency-group artifact of a
  superseded run on a pure-docs diff — **not** a real escape; do not chase it.)
- **#1096** (off-thread the toolbar per-tracker append disk read) —
  `tests-out-of-band` dismissed a **genuinely RED** `Test-delta gate` on a
  **behaviour-preserving** off-thread perf refactor (logic moved to a worker, no
  semantic change, existing suite stayed green). This is the **same class
  already postmortem'd** twice: `2026-06-08 · #1021/#1016` and `2026-06-09 ·
  #1083`. No new gate — covered below by reference.

### Root cause
Blameless, per class:
- **#1110 (CR convention-blindness).** `.coderabbit.yaml` has no `path_instructions`
  entry for `docs/plans/**`, so CodeRabbit has no way to learn the repo's
  tier-less move-proof plan-ref convention or that `INDEX.md` is gate-synced by
  `doc-validation.yml`. It therefore re-derives both as defects every time a plan
  archives `active/` → `shipped/`, forcing a manual `cr-out-of-band` each time.
- **#1095 (no auto-downgrade for CR-can't-review on low-risk diffs).** A CR
  `review-skipped` caused by an upstream **rate limit** is an infra condition, not
  a signal about the diff. On a **pure-docs** PR (`is-pure-docs-diff.sh` true)
  there is nothing for CR to find, yet the gate still hard-blocks until a human
  hand-applies `cr-out-of-band` — a recurring manual override for a deterministically
  safe case.
- **#1096 (Test-delta has no behaviour-preserving-refactor exemption).** Same
  root cause as the two prior entries: `coverage-delta-gate.sh` `_classify_diff`
  has no exemption for `Source/Core/src/*.cpp` changes that **cannot** carry a
  desktop test delta by construction (behaviour-preserving refactor / cross-compile-only
  arm). Third recurrence — raises the priority signal on that already-filed residue.

### Preventing gate
- **#1110 → new (tooling P2): `.coderabbit.yaml` `path_instructions` for `docs/plans/**`.**
  Teach CodeRabbit the tier-less move-proof plan-ref convention (a `docs/plans/<slug>.md`
  reference with no `active/`|`shipped/`|`deferred/` segment is valid by design and
  must not be flagged as "missing a tier") and that `docs/plans/INDEX.md` is
  auto-synced by CI's "Auto-sync plan INDEX" job (don't assert it RED from a stale
  pipeline view). Stops the recurring false positives at the source so no future
  plan-archive PR needs `cr-out-of-band`.
- **#1095 → new (tooling P2): auto-downgrade CR `review-skipped`→WARN when cause is
  a rate limit AND the diff is pure-docs.** In the CR gate (`merge-gates.sh` CR
  condition), when CodeRabbit's review state is `review-skipped` with a
  rate-limit cause **and** `agents/scripts/core/is-pure-docs-diff.sh` returns true,
  treat it as WARN (the existing `cr-out-of-band` semantics) automatically — no
  manual label. Scoped to **pure-docs only** on purpose: CR's review stays a hard
  signal on any code diff.
- **#1096 → none new — already covered.** The behaviour-preserving / cross-compile-only
  Test-delta exemption is filed as `coverage-gate-platform-else-arm-exemption`
  (the `2026-06-08 · #1021/#1016` entry) with a behaviour-preserving-refactor P3
  residue; the `2026-06-09 · #1083` entry is a second recurrence. This is the
  **third** — the residue should graduate from P3 to P2 (signal raised in the
  filed entry).

### Filed as
- [`tooling.md`](categories/tooling.md) — `coderabbit-plan-ref-convention-path-instruction` (P2, #1110)
- [`tooling.md`](categories/tooling.md) — `cr-review-skipped-pure-docs-auto-downgrade` (P2, #1095)
- #1096 → no new file; priority-raise note appended to the existing
  `coverage-gate-platform-else-arm-exemption` residue line.

## 2026-06-09 · PR #1083 · `tests-out-of-band` — Test-delta gate RED on a dual-target compile-guard
### What escaped
The **Test-delta gate** (`scripts/dev/coverage-delta-gate.sh`) tripped RED on
#1083 and was waved through with `tests-out-of-band`. The product-code change
that triggered it was a single `#ifndef SMATCHET_EMBEDDED_IN_UNREAL` guard
wrapped around the **existing** `PendingShotStamp()` definition in
`SmatchetBugReportUi.cpp` — the function is byte-identical on the desktop test
target; the guard only stops it compiling on the DX12 / Unreal target (where
every call site is `#ifdef`'d out), silencing `-Wunused-function -Werror`. No
new runtime surface exists for the desktop test binary to assert, so the
coverage-keyed gate can never be satisfied and always trips RED.
### Root cause
**Not a new gate hole — a recurrence of an already-diagnosed, already-filed,
still-unapplied one.** The `test-delta-test-light-exemption` class was diagnosed
in the 2026-06-06 postmortem and filed to `infra.md`, but the carve-out classifier
was never implemented, so every legitimately-untestable correctness diff keeps
paying the `tests-out-of-band` + postmortem tax. #1083 is the ≥10th instance
across ≥4 unrelated work-streams. Additionally, the existing classifier spec
enumerated `static_assert`-only / logging-only / comment-only / CMake-only but
did **not** name the **preprocessor-guard-only** sub-case (a diff that only
adds/moves `#if`/`#ifdef`/`#ifndef`/`#else`/`#endif` around otherwise-unchanged
code) — exactly the #1083 shape — so even once built, the planned classifier
would have missed it.
### Preventing gate
**Escalate + extend the already-filed gate, not a new one.** The recurrence is an
*application* gap, not a *diagnosis* gap — a second duplicate entry would add
noise, not coverage. So: (1) escalated `test-delta-test-light-exemption` P2→P1
in `infra.md` (recurrence ≥10 PRs, still unapplied → the override+postmortem tax
now dominates); (2) extended its no-new-runtime-surface classifier spec to add
the **preprocessor-guard-only** sub-case, with a matching bats fixture called
out in the Concrete-next-action; (3) referenced #1016/#1021/#1083 as recurrence
evidence so `postmortem-owed.sh` dedupes them against the one open entry.
### Filed as
`docs/self-improvement/categories/infra.md` — `test-delta-test-light-exemption`
(escalated P2→P1, preprocessor-guard-only sub-case added, recurrence PRs logged).

## 2026-06-09 · PR #1074, PR #1075 · `cr-out-of-band` ×2 (batch tail) + a configure-time gate that FATAL'd all CI
### What escaped
Two things, neither of which shipped *broken* to develop, but both owe the ledger:
1. **`cr-out-of-band` ×2** — #1074/#1075 are the tail of the same close-gate-gaps burst as the 13-PR batch directly below; CodeRabbit's org-credit/rate-limit was still exhausted, so both merged with the label and no `cr-disposition:` trail. Same class + root cause as that batch.
2. **#1074's new MSVC toolset guard FATAL'd every Windows CI required check** (Coverage / Windows+MSVC / Windows-light / Perf-fast / Packaging). The guard read `build.msvc_toolset_pin` (`14.38`, a **local-dev** convention) and `FATAL_ERROR`'d when the compiler minor differed — but CI runners use their own consistent (non-14.38) toolset. This did **not** escape: the merge-gate correctly **blocked** #1074 until it was fixed (`NOT DEFINED ENV{CI}` → guard is local-only). The "gate I added needed a gate" irony.
### Root cause
(1) The `pr-burst-guard` (infra P1, filed for the batch below) is still open — a >10-PR burst by one author always blows CR's hourly quota; nothing throttles it. (2) A **gate that encodes a local-dev assumption (the pinned toolset) was applied unconditionally**, so it fired in the one environment (CI) where the assumption is false. The local-only intent lived in the comment, not the condition.
### Preventing gate
- For the `cr-out-of-band` tail: no NEW gate — same as the batch below (`pr-burst-guard` throttle + `cr-disposition` trail, both already filed). Override legitimate (CR billing-unavailable; content reviewed by the orchestrator + specialist agents per-PR).
- For the toolset-guard-broke-CI class: **rule — a configure-time / build gate that encodes a *local-dev* convention (a pinned toolset, a machine path, a `$HOME` assumption) MUST be scoped to local (`NOT DEFINED ENV{CI}`), or it breaks every CI runner.** Codified in the new infra self-improvement entry + the guard now carries the env-gate. Cheap future check: a reviewer/lint nudge on a new `message(FATAL_ERROR` in `CMakeLists.txt` that references a `project.config.json` *local* knob without an env-scope guard.
### Filed as
`docs/self-improvement/categories/infra.md` (local-dev gates must be CI-scoped; subagent build-dir reconfigure hazard).

## 2026-06-09 · PR #1046, PR #1049, PR #1052, PR #1053, PR #1056, PR #1057, PR #1058, PR #1059, PR #1060, PR #1061, PR #1062, PR #1064, PR #1072, PR #1075 · `postmortem-owed` batch (14 PRs) — `cr-out-of-band` ×14 + phantom red-checks

> **#1075 folded in (2026-06-09, post-merge of this entry).** A later
> same-class straggler from the same close-gate-gaps sprint, flagged by the next
> SessionStart nudge after this batch shipped. Identical disposition — routine
> `cr-out-of-band` + a phantom `CANCELLED`-beside-`SUCCESS` Test-delta twin
> (CANCELLED 14:27:17 + SUCCESS 14:27:27). No new RCA or gate; referenced here
> so `postmortem-owed.sh` dedupes it. Counts below updated 13→14 / 11→12.
> (#1075 is also covered by the `PR #1074, PR #1075` entry above, filed
> concurrently by PR #1078; the double reference is intentional — either entry
> dedupes it.)

### What escaped
A single SessionStart `postmortem-owed` nudge for **14 develop merges** (13 in
the original batch + #1075 folded in). Triaged against the live
`statusCheckRollup` (the snapshot ledger is uncommitted on develop — see Root
cause), **12 of 14 are detector false-positives, not real escapes**; 2 facets
are real-but-healed:

- **`cr-out-of-band` ×14 (all of them)** — every PR in a ~14-PR "close-gate-gaps"
  burst (#1046–#1075) carried `cr-out-of-band`. CodeRabbit's hourly per-author
  review quota was exhausted by the burst (CR posted its rate-limit auto-comment
  on #1046 and #1052), so CR could not review most of them in time → the label
  downgraded the CR block to WARN ×14. **Exact recurrence of the 2026-06-06
  #905–#908 PR-burst cascade.**
- **"red `Test-delta gate`" on #1072/#1062/#1064/#1060/#1058/#1059/#1049/#1075 —
  phantom.** Each is a `CANCELLED` concurrency-superseded twin sitting ~10 s
  beside a `SUCCESS` run for the same context (verified on #1072: CANCELLED
  13:21:47 + SUCCESS 13:21:57; and on #1075: CANCELLED 14:27:17 + SUCCESS
  14:27:27). The gate passed; GitHub merged on the SUCCESS run; the detector read
  the CANCELLED twin.
- **#1064 `Mobile — Android NDK arm64-v8a` (advisory) red — phantom.** A transient
  `sdkmanager` "Error on ZipFile" at the NDK install step on a `continue-on-error`
  lane outside the merge-gates meant-to-block allow-list — it can never block a
  merge, so it owes no postmortem.
- **#1062/#1049/#1046 `tests-out-of-band` — moot.** Test-delta gate was `SUCCESS`
  on the merge head; the override was non-load-bearing. **Recurrence of the
  2026-06-08 #991 moot-override class.**
- **#1072 `Sanitizer (UBSan via Clang)` — real-but-healed.** Genuinely
  `IN_PROGRESS` at the maintainer's manual merge (started 13:33:41, merge
  13:36:14) and only reached `SUCCESS` at 13:44:14 — a manual `PUT …/merge` fired
  past a non-terminal meant-to-block allow-list check, which then passed. No
  product harm; the automated poller would have blocked (the manual merge bypassed
  it).
- **#1046 `Bucket-E UI tests` red — real-but-healed.** The `Run bucket-E …`
  scenario step PASSED; the red was an `actions/cache` "Post Cache FetchContent
  _deps" teardown SAVE failure after the tests, manually merged past by the
  maintainer (Bucket- is on the meant-to-block allow-list, so the teardown-red
  twin tripped it).

### Root cause
Two independent gate holes plus a shared meta-cause:

1. **Detector over-reports (the dominant hole).** `postmortem-owed.sh`'s JQ_ROWS
   filter flags any `statusCheckRollup` row whose conclusion ∉
   {SUCCESS, SKIPPED, NEUTRAL}, with **no** reconciliation to (a) the latest run
   per context, (b) the merge-gates blocking scope (required ∪
   `Coverage|Sanitizer|Bucket-`), or (c) a genuine terminal FAILURE. So a
   CANCELLED concurrency twin, an advisory-lane flake, and an IN_PROGRESS-then-
   SUCCESS check all read identically to a hard required-check failure — 10 of the
   14 phantom rows come from this alone.
2. **PR-batching is prose-only.** `AGENTS.md` § Autonomous ship-loop "one PR per
   logical feature" is unenforced; nothing measures the burst or the CR
   rate-limit comment before opening PR N+1, so the quota blew exactly as on
   #905–#908.
3. **Meta-cause — the snapshot ledger is dark.** `merge-snapshots.jsonl` is
   uncommitted/0-bytes on develop, so `postmortem-owed.sh` runs **entirely on the
   degraded live-`statusCheckRollup` fallback**, where post-merge re-runs +
   CANCELLED twins + non-terminal rows are all visible. A lossless ADR-0017
   snapshot would record the single terminal conclusion per context at merge and
   starve both hole (1) and the #991 moot-override class at the source.

Blameless: no operator did anything wrong — the burst followed a legitimate
gate-hardening sprint, the manual merges were past genuinely-green test steps,
and the overrides were correct. Every false "owed" is a **gate-don't-trust
inversion**: the detector cries wolf 11/13, training the operator to wave the
nudge through — which is precisely how a real escape would slip past.

### Preventing gate
- **NEW (P1):** `postmortem-owed.sh` must reconcile the rollup the way
  `merge-gates.sh` does before flagging a `red-check` — dedupe to the latest run
  per context (drop CANCELLED-beside-SUCCESS twins / exclude CANCELLED), restrict
  to the merge-gates blocking scope (advisory lanes dropped), and require a
  terminal FAILURE.
- **ESCALATED P2→P1:** `pr-burst-guard` (infra) — a pre-ship check that counts
  open-PR / `gh pr create` rate (and/or detects the CR rate-limit comment) and
  pauses before blowing the quota. Second confirmed occurrence (#905–#908, now
  #1046–#1072) → escalated.
- **Reinforced (already filed, recurred):** `postmortem-owed-moot-override-false-positive`
  (tooling P2, #991 — #1062/#1049/#1046 again); `cr-out-of-band-disposition-trail`
  (process P3 — all 13 lacked a disposition trail);
  `mandatory-merge-snapshot-on-override-merge` (tooling P1, #966 — the dark-ledger
  meta-cause). No new entry for the #1072/#1046 manual-merge-past-running-check
  facet — the detector terminal-state fix covers the false nudge, and the
  bypass-the-poller theme is already tracked by `postmortem-owed-direct-push-blindspot`
  (tooling P2).

### Filed as
- `docs/self-improvement/categories/tooling.md` — NEW P1
  `postmortem-owed-overreports-nonblocking-and-cancelled-twins`; recurrence note
  on `postmortem-owed-moot-override-false-positive`.
- `docs/self-improvement/categories/infra.md` — `pr-burst-guard` escalated
  P2→P1 + recurrence note.
- `docs/self-improvement/categories/process.md` — recurrence note on
  `cr-out-of-band-disposition-trail`.

## 2026-06-08 · PR #1021, #1016 · `tests-out-of-band` override (load-bearing, both legitimate)

### What escaped
Nothing defective. Two PRs merged carrying a **load-bearing** `tests-out-of-band`
label that dismissed a RED Test-delta gate — both correct to ship without a new
desktop test:
- **#1021** (Phase-0 mobile triple-target build infra, Slices 0–2) — touched
  `Source/Core/src/Tracker/TrackerHttpUtils.cpp` + `Source/Core/src/SubprocessCapture.cpp`
  alongside build-only CMake/CI/preset files. The two Core edits are
  **cross-compile-only**: a `static_cast<std::int32_t>` that is an **identity no-op
  on the desktop test target** (LLP64 `long` is already 32-bit — it only narrows on
  LP64 Android/Linux), and a `std::string(ptr,length)` ctor swap living in a
  **Bionic-only `#else` arm the desktop unit-test binary never compiles**. No
  meaningful `*.test.cpp` delta is possible on the desktop test target; the edits
  were validated by the advisory `android-ndk-arm64` + `posix-core-check` jobs.
- **#1016** (AppController pImpl / sol2 header-lift) — a **behaviour-preserving
  refactor**; logic moved between TUs with no semantic change, so no new
  `*.test.cpp` is warranted (the unchanged existing suite already covers it and
  stayed green).

Both overrides were load-bearing (the Test-delta gate was genuinely RED and the
label dismissed it), but neither change is desktop-unit-testable and each was
validated by other means.

### Root cause
Blameless. The Test-delta line-classifier (`coverage-delta-gate.sh` `_classify_diff`)
has no exemption for two legitimate classes of `Source/Core/src/*.cpp` change that
**cannot** carry a desktop test delta by construction:
1. **Cross-compile-only edits.** A hunk confined to a platform `#else`/`#elif` arm of
   a `#if defined(<platform-macro>)` block is unreachable on the desktop test target
   (it compiles only under the other platform's toolchain); an identity cast that is
   a no-op on the test platform's type model is untestable there.
2. **Behaviour-preserving refactors.** Logic moved between files with no semantic
   change adds production-line churn the classifier counts as testable surface, even
   though the existing suite already covers it.
The classifier sees production `.cpp` churn + zero `*.test.cpp` delta and trips,
forcing a manual `tests-out-of-band` override for changes correct to ship without a
new test. (The existing auto-exemptions cover comment/log/`static_assert`/include/
catch-scaffold/build-only — not a real statement inside a platform `#else` arm.)

### Preventing gate
Extend `coverage-delta-gate.sh` `_classify_diff` with a **platform-guard exemption**:
while walking the diff, track `#if defined(<macro>)` / `#ifdef` / `#else` / `#elif` /
`#endif` nesting, and auto-exempt added/removed lines that sit inside an
`#else`/`#elif` arm of a guard keyed on a known cross-target macro set (`__ANDROID__`,
`__APPLE__`, `__linux__`, and the non-`_WIN32` else of a `_WIN32`/`WIN32` guard) —
unreachable on the desktop test target by construction, exactly like the existing
comment/log/include auto-exemptions. Filed to tooling (P2,
`coverage-gate-platform-else-arm-exemption`). **P3 residue** (same entry, harder to
classify mechanically — likely stays a manual override): (a) behaviour-preserving
cross-TU refactors like #1016; (b) an identity cast on the desktop-**reachable**
side — #1021's LP64 cast is compiled by both targets and is *not* `#else`-confined,
so the platform-arm exemption won't cover it (would need a separate
"no-op-on-test-platform" heuristic, probably not worth the complexity).

### Filed as
`docs/self-improvement/categories/tooling.md` 2026-06-08 —
`coverage-gate-platform-else-arm-exemption` (P2) + the refactor / identity-cast P3 residue note.

## 2026-06-08 · PR #991 · `tests-out-of-band` override (moot at merge — non-load-bearing)

### What escaped
Nothing defective. #991 (log Ollama streaming transport + HTTP errors) merged
carrying a `tests-out-of-band` label that was **moot by merge time**. The label
was applied on commit `3ef73d64` while the diff was logging-only; a later commit
`644b32be` then extracted the message assembly into pure helpers
(`OllamaStreamError.{h,cpp}` — `FormatOllamaTransportError`,
`FormatOllamaHttpError`) and added `tests/Core/OllamaStreamError.test.cpp`
(4 `TEST_CASE`s / redaction-aware), so on the merged head the **Test-delta gate
passes on its own "production + test files both changed" branch** —
`coverage-delta-gate.sh:447`. The override was no longer load-bearing, but the
label was never removed, so `postmortem-owed.sh` flagged a *resolved* override as
an escape.

### Root cause
Blameless — two gate holes, neither an agent/person:
1. **Stale-override hygiene.** An override applied mid-PR (when the diff genuinely
   tripped the gate) was not removed after a follow-up commit resolved the gate
   *in kind* (added the test). No step in the ship-loop prompts dropping a
   now-moot `*-out-of-band` label before merge.
2. **`postmortem-owed.sh` keys on label _presence_, not load-bearing-ness.** A
   resolved/moot override reads identically to one that actually dismissed a RED
   required check. So the detector raises a phantom "owed" nudge — the same
   false-positive class as the 2026-05-23 revert-prose detector bug (which matched
   commit *bodies* and flagged feature PRs that merely mentioned "revert").

### Preventing gate
Teach `postmortem-owed.sh` to **suppress an `*-out-of-band` flag when the override
was not load-bearing** — i.e. the named check is terminal-`SUCCESS` on the merge
head AND (for `tests-out-of-band`) the PR's diff carries a test delta
(`tests/**/*.test.cpp` add/modify), meaning the gate would have passed without the
label. Only a load-bearing override (the named check would be RED without the
label) owes a postmortem. Mirrors the 2026-05-23 subject-match tightening that
stopped phantom revert nudges. Filed to tooling. **Defensive sibling** (also
filed, the latent hole that would have forced a *real* override had `644b32be`
not added the test): teach `coverage-delta-gate.sh` to join multi-line `LOG_*`
continuations before the per-line classifier — a logging-only `LOG_` call wrapped
across lines (forced by `.clang-format` ColumnLimit 120, leaving an
identifier-bearing tail like `r.error.message.c_str());`) is currently classified
as real runtime surface and trips the gate, even though the whole statement is a
single no-new-runtime-surface log call.

### Filed as
`docs/self-improvement/categories/tooling.md` 2026-06-08 — two entries:
`postmortem-owed-moot-override-false-positive` (P2) +
`coverage-gate-multiline-log-join` (P3).

## 2026-06-08 · PR #995 (fix); escaped via an earlier merge · `test-shell-lint.sh` SIGPIPE-aborted (exit 141), blocking the required Shell-lint check on ALL open PRs

### What escaped
The required **Shell lint (shellcheck)** check went red on `develop` (~01:31 UTC 2026-06-08) and stayed red on every open PR built afterward (#993, #994, and any other). The failure was **exit 141 (SIGPIPE)**, not a real finding: `test-shell-lint.sh`'s deps rule extracted the first hit's line number with `lno=$(printf '%s\n' "$real_use" | head -1 | cut -d: -f1)`. Once a scanned script used an allow-listed tool on enough lines that `$real_use` exceeded the **64 KB pipe buffer**, `head` closed the pipe after one line → `printf` got SIGPIPE → under `set -euo pipefail` the **plain assignment** returned 141 → `set -e` aborted the whole gate. The script that crossed the threshold merged in the 00:51–01:31 window.

### Root cause
Two compounding holes:
1. **The gate's own CI run never reproduced the failure mode it ships under.** `test-shell-lint.sh` runs on the **msys2** dev shell locally (which sets `SIGPIPE` to `SIG_IGN`, inherited by children — so `printf` gets a benign `EPIPE` write error, not a signal) and passed **137/137**. CI runs on **ubuntu** with the **default** SIGPIPE disposition, where the same pipeline kills `printf` and trips `set -e`. The 137/137 local pass was a false green for the CI environment.
2. **Pipe-fragile idiom under `pipefail`.** `producer | head -N` in a plain assignment is inherently SIGPIPE-prone with `set -euo pipefail`; the gate had no rule against its own shape, and the failure was **data-dependent** (only trips past 64 KB), so it lay dormant until a large-enough script entered the scan set.

### Preventing gate
A bats regression in the **required** Shell-lint set that runs `test-shell-lint.sh` with the **default SIGPIPE disposition** (`trap - PIPE`) against a **>64 KB** unguarded-tool fixture and asserts a clean finding (exit 1), **never 141** — shipped in PR #995 (`tests/bats/shell_lint.bats` "many-line unguarded use does not SIGPIPE the gate"; fixture `tests/fixtures/shell_lint/known-bad-1-deps-manylines.sh`). Mutation-verified: the case fails (141) against the old pipeline. This makes the CI-only environment difference reproducible in the gate's own test suite, closing hole 1 for this class. Follow-up (filed below) generalizes hole 2: a lint forbidding `… | head` in a plain assignment under `pipefail`.

### Filed as
[`docs/self-improvement/categories/tooling.md`](categories/tooling.md) — 2026-06-08 P2: a shell self-lint rule flagging `<producer> | head` in a bare (`set -e`-exposed) assignment under `pipefail` as SIGPIPE-fragile, repo-wide.
## 2026-06-07 · PR #966 · vsync CR-953 follow-ups merged past RED Tests + Perf via tests-out-of-band + perf-out-of-band

### What escaped
PR #966 (`fix(vsync): honour --vsync/--no-vsync on hidden-window boot + config.set
string forms`) merged to develop carrying BOTH `tests-out-of-band` and
`perf-out-of-band` — i.e. it shipped past a red required Tests check AND a red
required Perf PR-fast check, each downgraded to WARN by its named override. No
`merge-snapshots.jsonl` line was written for #966, so the exact red checks can no
longer be reconstructed (post-merge re-runs overwrote the live rollup); only the
two override labels survive. postmortem-owed flagged it via trigger 2 (override
label), not trigger 1 (no lossless snapshot to read).

### Root cause
Blameless — three gate holes, no person:
1. **Perf gate is warmup-dominated.** A vsync change shifts frame pacing → the
   perf-pr-fast p99 ceiling fires; per the #963 postmortem this ceiling currently
   fires on *every* perf-relevant PR (cold-start frames dominate p99), so
   `perf-out-of-band` is the routine escape, not a rare exception. #966 is another
   instance of that still-open class.
2. **Override labels can't downgrade a re-run** — `perf-pr-fast.yml` reads the
   frozen `github.event.pull_request.labels` payload, so applying the label then
   re-running replays the old payload; the author had to mint an empty commit to
   apply it (already filed tooling P2).
3. **No lossless audit of what an override bypassed.** ADR-0017's merge-snapshot
   ledger was NOT written for #966, so the override-label merge left no record of
   which checks were red. The `tests-out-of-band` half is now unrecoverable — we
   cannot say which test was red.

### Preventing gate
NET-NEW (the auditability hole): **make the merge-snapshot ledger write mandatory
+ verified for any override-label merge.** The merge actors (orchestrator
`handle_pass` / git-janitor / merge-watcher) must append the ADR-0017
`{pr, mergeCommit, redChecks, overrideLabels}` line BEFORE an override-downgraded
merge (fail the merge if the append fails), and postmortem-owed (or a post-merge
job) must WARN when a develop merge commit's PR carried an override label but has
no matching snapshot line. That guarantees every override is auditable after the
fact, closing the "tests-out-of-band masked an unknown test" hole. The other two
holes already have owners — cross-ref `p99-gate-warmup-frame-exclusion` (tooling
P1, from #963) for #1 and the frozen-payload label read (tooling P2, PR #966) for
#2. Do not duplicate those.

### Filed as
`docs/self-improvement/categories/tooling.md` 2026-06-07 `mandatory-merge-snapshot-on-override-merge` (P1).

## 2026-06-07 · PR #945, #953 · cr-out-of-band overrides — legitimate (findings triaged out-of-band, none dropped)

### What escaped
PR #945 (multi-grid Slice 1a, GridLiveContext extraction) and PR #953 (full
vsync toggle) each merged with the `cr-out-of-band` label, downgrading
CodeRabbit's `COMMENTED + actionable` block to WARN. Override-label use owes a
postmortem per AGENTS.md regardless of legitimacy.

### Root cause
Blameless — designed use of the label, not a hole. Both PRs were reviewed
out-of-band of the CodeRabbit merge gate during the 2026-06-07 sprint:
- **#945**: the in-repo `code-review` agent (opus/high) reviewed pre-merge and
  caught 2 HIGH raw-pointer-across-async (backend-latch) bugs, fixed in-branch
  before merge. CR's COMMENTED findings were triaged against that pass.
- **#953**: CR's actionable findings were triaged into follow-up PR #966
  (`fix(vsync): honour --vsync/--no-vsync on hidden-window boot + config.set
  string forms (CR-953 follow-ups)`) — every actionable carried forward, none
  dropped.

### Preventing gate
none — override legitimate (label used as designed: review happened
out-of-band with a verifiable disposition for every actionable — in-branch
fixes for #945, follow-up PR #966 for #953). One conformance residue found
during this RCA: `merge-gates.md` says the label "MUST NOT stay on the PR
post-merge", yet it was still on both PRs (and `perf-out-of-band` on #963) —
all three stripped 2026-06-07. Residual class risk: the label *could*
silently drop findings if applied without a disposition trail; a cheap
tightening (label application must cite where each actionable was triaged,
plus a janitor sweep for stale post-merge labels) is filed in the § Filed as
entry rather than mandated here.

### Filed as
`docs/self-improvement/categories/process.md` 2026-06-07
`cr-out-of-band-disposition-trail` (P3).

## 2026-06-07 · PR #963 · perf-out-of-band override merged the 100 Hz floor past a red required Perf PR-fast check

### What escaped
PR #963 (raise the Pillar-1 floor 60 Hz → 100 Hz; p99 ceiling 16.67 → 10.0 ms)
was merged with the `perf-out-of-band` label, which downgraded a RED required
check (`Perf PR-fast (windows-2022)`) to WARN. The check failed on
`SmatchetUI::Draw` p99 **43.1 ms** and `drawEnsureCatalogAndInitialSync`
**41.4 ms** — both far over the new 10.0 ms ceiling AND over the prior 16.67 ms
floor. Merging past a required check via a documented override label is a
gate-escape class per AGENTS.md (override-label use owes a postmortem).

### Root cause
Blameless — two gate holes, not a person:
1. **The p99 ceiling is warmup-dominated.** Each PR-fast scenario's p99 is taken
   over a short frame window that includes cold-start frames (font-atlas build,
   first-frame layout, initial catalog sync). Those one-time spikes (40+ ms on a
   software-GL CI runner) dominate the 99th percentile, so the umbrella
   per-frame scope `SmatchetUI::Draw` reports a p99 that reflects warmup, not
   steady state. The ceiling therefore fires on every perf-relevant PR
   regardless of the cap value — it would have fired at 16.67 ms too; it was
   silent only because the ceiling was structurally inert until CR-949-1 made it
   live.
2. **No steady-state isolation in the gate.** `perf-compare.py` compares fresh
   p99 against the absolute ceiling with no warmup-frame exclusion, so a gate
   meant to protect steady-state framerate is gated on cold-start outliers.

### Preventing gate
Add **warmup-frame exclusion** to the p99 path: drop the first N frames (or
first M ms) of each scenario before the ring feeds `ComputeP99`, so the absolute
p99 ceiling measures steady-state work, not cold-start. The 10.0 ms (100 Hz)
ceiling then becomes enforceable for real and perf-relevant PRs stop needing
`perf-out-of-band`. This is the substance of the parked perf-gate-revival work
(`docs/plans/active/build-quality-velocity-hardening.md` #8/#13); the #963
override is the forcing signal to unpark it. Until it lands, perf-relevant PRs
that trip the umbrella warmup spike legitimately use `perf-out-of-band` (WARN),
and a RUN failure (build/exe/plumbing) still hard-blocks regardless of the label.

### Filed as
`docs/self-improvement/categories/tooling.md` 2026-06-07 `p99-gate-warmup-frame-exclusion` (P1).

## 2026-06-07 · PR-less direct push 93c63d0f · code-review model change shipped to develop bypassing the PR flow + 6 required checks

### What escaped
Commit `93c63d0f` (code-review agent `sonnet/high` → `opus/high`) landed on
`origin/develop` via a **direct push**, bypassing branch protection ("Changes
must be made through a pull request" + "6 of 6 required status checks") through
the repo-admin bypass — no PR, no CI, no `merge-snapshots.jsonl` line. The change
itself was trivial + explicitly user-directed; the escape is that **the escape
detector never saw it**. `postmortem-owed.sh`'s three triggers all key on a
merged PR (non-SUCCESS check on a merged head, override label on a merged PR,
a `Revert` commit) or the pr+mergeCommit-keyed snapshot ledger. A PR-less direct
push produces none of those, so the class is structurally invisible to the
SessionStart nudge — it owed a postmortem only because a human noticed in-session.

### Root cause
Two stacked holes, neither an agent/person:
1. **`postmortem-owed.sh` is PR-centric.** Every trigger derives from a merged PR
   or the pr-keyed snapshot ledger. A commit pushed straight to develop (admin
   bypass) never appears in `gh pr list` and writes no ledger line, so the
   detector is blind to direct-push escapes — the *highest-trust* escape (no
   review, no CI at all) is the one it cannot see.
2. **Local guard hooks are env-overridable with no audit trail.**
   `guard-head-drift.sh` (no direct commit to develop) and `guard-shared-tree.sh`
   (no HEAD mutation under a concurrent session) are defeated by
   `SMATCHET_ACK_BRANCH_DRIFT=1` / `SMATCHET_ALLOW_SHARED_SWITCH=1`. Legitimate
   escape hatches, but they leave no record that an override fired — so even the
   hook side offers the detector nothing to key on.

### Preventing gate
Add a **fourth trigger to `postmortem-owed.sh`**: in the develop scan window,
flag any non-merge commit on develop with **no backing PR** — its subject lacks
the `(#N)` squash-merge suffix AND `gh pr list --search <sha> --state merged`
returns nothing → "PR-less direct push, owes a postmortem", deduped by commit
SHA (as the PR triggers dedupe by `#N`). The subject-suffix half works offline
via `git log`, so the detector degrades gracefully when `gh` is down (it was
unauthenticated during this very incident). Secondary (optional): have the
override hooks append a one-line `{sha, override-name, branch}` record to a
committed audit log when an override fires, giving the detector a second,
hook-side source. Bats coverage: a direct-push fixture commit must produce a
`postmortem owed` line.

### Filed as
`docs/self-improvement/categories/tooling.md` 2026-06-07 `postmortem-owed-direct-push-blindspot` (P2).

## 2026-06-07 · PR #441 (escape origin), fixed by PR #937 · bucket-C/E green-but-broken for 2 weeks (continue-on-error masked total harness death)

### What escaped
The bucket-C screenshot-diff and bucket-E ImGui-Test-Engine jobs (the
"headless GL" validation lanes added by #441, 2026-05-24) showed **green on
every run while zero tests ever executed successfully on CI**. Observed
directly on #937's bucket-C run: `Passed: 0  Failed: 3`, step exit 1, job
green. Every visual/UI regression those lanes exist to catch was unguarded
for the whole period.

### Root cause
Two stacked holes, neither an agent/person:
1. **Incomplete Mesa provisioning**: the #441 install step copied only
   `opengl32.dll` from mesa-dist-win. Mesa ≥ 22 splits the driver —
   `opengl32.dll` is a 137 KB thin loader hard-requiring `libgallium_wgl.dll`
   (53 MB) + `libglapi.dll` beside it — so every exe in the build dir died at
   process start (`STATUS_DLL_NOT_FOUND`), before any test logic. Silent: the
   Windows loader writes nothing to the console.
2. **`continue-on-error: true` masks *total* harness death the same as
   *partial* flakiness**: the advisory lane was designed to tolerate flaky
   individual tests, but it equally swallows "the harness never ran at all" —
   `Passed: 0` and `Passed: 47, Failed: 1` look identical from outside
   (both green). No signal distinguishes an advisory lane that is degraded
   from one that is dead.

Fixed (the Mesa half) by PR #937: 3-DLL copy, strict final `cp` (fail loudly),
cache key bumped `mesa-dlls-24.2.5-v2`, verified by local two-phase repro +
a live CI run (`run_failure_count=0`).

### Preventing gate
A **launch-smoke hard step** in front of every advisory exe-running lane:
a NON-`continue-on-error` step that runs the freshly-provisioned exe once
(`Smatchet.exe cmd app.version --spawn --yes`) after Mesa install and before
the advisory bucket step. "The exe cannot even start" then fails the job hard
regardless of how flaky the tests behind it are — separating *dead harness*
(hard fail) from *flaky tests* (advisory). NOTE (corrected in PR #1180): the
outer `timeout` MUST sit ABOVE the app's own `--spawn` ready window
(`SMATCHET_SPAWN_READY_MS`, default 30 s — `--spawn` boots the full GUI app
+ MCP server, not a bare CLI). The first cut used `timeout 10`, which
undercut the 30 s ready budget and red-walled bucket-C/E on a slow-but-healthy
Mesa boot; the gate now pins `SMATCHET_SPAWN_READY_MS=30000` with a 45 s
outer hang-guard.
Plus, inside the advisory steps: treat `Passed: 0` with `Failed: > 0` as a
hard exit (a lane that passes nothing is not flaky, it is broken).

### Filed as
`docs/self-improvement/categories/infra.md` 2026-06-07 `bucket-lane-launch-smoke` (P1).

## 2026-06-07 · coverage.yml (since #834 graduation), fixed by PR #941 · documented `coverage-out-of-band` escape was implemented nowhere (prose-promise gate)

### What escaped
`coverage.yml`'s header has documented "Below-threshold PRs escape with the
`coverage-out-of-band` label" since the gate graduated to blocking
(#834, 2026-06-04) — but no code read the label: not the workflow, not
`merge-gates.sh`. The first PR that legitimately needed the hatch (#939,
structural measured-set expansion 70% → 64% while absolute coverage rose)
found it didn't exist; the merge path was blocked until the escape was
implemented mid-flight (#941).

### Root cause
The escape was specified in prose at graduation time and never wired —
exactly the "prose promise, no gate" class AGENTS.md § Merge gates warns
about, inverted: here the *escape* (not the gate) was prose-only. Nothing
validates that a documented override label has a reader.

### Preventing gate
A self-test-style lint: every `*-out-of-band` label string mentioned in
`.github/workflows/*.yml` comments or `AGENTS.md` § Merge gates must be
grep-matched by an implementation site (a `labels`-reading step in a
workflow, or a `merge-gates.sh` downgrade branch). Cheapest form: a case in
`agents/scripts/project/test-lint-rules.sh` (or `test-docs.sh`) that extracts
documented label names and asserts each appears in at least one non-comment
code line. Catches the class for all future labels.

### Filed as
`docs/self-improvement/categories/tooling.md` 2026-06-07 `oob-label-implementation-lint` (P2).

## 2026-06-07 · PR #939 · `coverage-out-of-band` override used (legitimate)

### What escaped
Nothing defective: #939 (multi-grid Slice 0 WS2) linked 5 real `JiraClient`
impl TUs into `SmatchetTests` for the catalog-build fixture, structurally
expanding the coverage denominator (70% → 64%) while **absolute covered lines
rose** (33 new test cases / 157 assertions; full rig 13,683 assertions green).

### Root cause
The line-rate threshold measures a ratio; adding production code to the
measured binary for legitimate fixture reasons dilutes the ratio without any
testing regression. This is the documented use case for the label.

### Preventing gate
none — override legitimate (structural measured-set expansion, absolute
coverage increased; the real follow-up — raising backend-impl coverage so the
class shrinks — is filed as a debt entry, `categories/debt.md` 2026-06-07).

### Filed as
`docs/self-improvement/categories/debt.md` 2026-06-07 `backend-impl-coverage-recovery` (P2).

## 2026-06-06 · PR #923 · auto-merged past a RED non-required "Coverage" check (spec inconsistency)

> Self-reported. The `smatchet-merge-watcher` auto-merged #923 (via
> `gh pr merge --squash --auto`) while the **non-required** `Coverage
> (windows-2022 + OpenCppCoverage)` check was RED — it was a `0x80000003`
> debugger-break crash from #923's own intentional `WARN`-on-false flaky-quarantine
> self-test under OpenCppCoverage (all 1314 tests PASSED; the process broke at
> teardown). `postmortem-owed.sh` did NOT flag it — by the gate's own design it was
> a clean merge (all 5 *required* checks were green).

### What escaped
A check that is **intended to block** (its CI step is literally named
`Capture coverage (blocking; --threshold 65)`) but is **configured non-required**
and **gate-ignored**. Two layers let it through:
1. The merge-watcher's gate (`merge-gates.sh`) only blocks on `isRequired==true`
   contexts (`GATE_FILTER` line 345: `$failing` is computed over `$req`, not all
   `$ctx`). This is **deliberate + tested** — `tests/fixtures/merge_gates_pass.json`
   contains a `non-required-fail` check and the "all gates pass → return 0" test
   asserts the gate PASSES past it.
2. GitHub auto-merge (`--auto`) likewise only gates on required checks, so even
   without the poller a non-required red never blocks `--auto`.

So #923 merged at its pre-fix head past the red coverage check, briefly leaving
develop's coverage job broken for any coverage-triggering PR. (Remediated same day:
PR #927 added `coverage.sh --no-breaks` so OpenCppCoverage's attached-debugger no
longer turns a failing/WARN assertion into a `STATUS_BREAKPOINT` crash.)

### Root cause
A **spec inconsistency**, not a code bug: `AGENTS.md` § Merge gates prose says
*"Never merge past ANY red check — required or not,"* but the merge-gates
implementation + its bats contract deliberately **ignore non-required checks**, and
`Coverage` (intended "blocking") is configured non-required with no skip-companion.
The prose policy and the tested implementation directly contradict; the watcher
followed the implementation. Coverage rarely runs (path-filtered to Source/tests),
so the contradiction lay dormant until a coverage-triggering PR with a red coverage
job hit the watcher.

### Preventing gate
**Resolve the inconsistency — a DESIGN DECISION surfaced to the maintainer (this
session), one of:**
- **(A)** Make `Coverage (windows-2022 + OpenCppCoverage)` a **required** check
  (`project.config.json` `branch_protection.required_contexts` + `setup-branch-protection.sh`)
  **and** add a `coverage-skip.yml` companion (Pattern B) so docs-only PRs don't
  deadlock. The watcher's existing required-only gate then blocks it. (Branch-protection
  change → maintainer approval.)
- **(B)** Add a curated **non-required-but-blocking allow-list** to `merge-gates.sh`
  (broaden `$failing` to include non-required contexts whose name is NOT advisory
  and IS in the allow-list — e.g. Coverage, Sanitizer, Bucket-E), update
  `merge_gates_pass.json` + add a bats case. Keeps truly-advisory checks
  (`Duplication scanner (advisory)`) non-blocking. (Code change; contradicts the
  current tested "non-required → pass" contract, hence needs the explicit decision.)
- **(C)** Accept Coverage as advisory — rename the step to drop "blocking" and
  document that only required checks gate merges. (Cheapest; weakens Pillar-1/3 coverage
  enforcement.)
Filed to `infra.md` (P1). **RESOLVED — maintainer chose (B), shipped in PR #933:**
`merge-gates.sh` `$failing` now computes over all `$ctx` and blocks a failing check
when it is required OR (non-required AND name matches `Coverage|Sanitizer|Bucket-`
AND not "advisory"); non-allow-listed non-required reds still pass (contract
preserved — verified by a direct jq test + a new `merge_gates.bats` case). The
allow-list regex is the single extension point to gate more checks later.

### Filed as
`docs/self-improvement/categories/infra.md` — *merge-gates non-required-red policy
(AGENTS.md "never merge past any red" ⇄ tested "non-required → pass" contradiction)* (P1, decision-pending).

## 2026-06-06 · PR #905, #906, #907, #908 (recurring class: #892, #894, #896, #897, #898) · 6-PR burst exhausted CodeRabbit quota → override cascade

> Surfaced while merging the build-quality-velocity-hardening Sprint-1 PRs: a
> 6-PR-per-feature split blew CodeRabbit's hourly review quota, and the
> `Test-delta gate` fired on test-light correctness fixes — both waved through
> with override labels.
>
> Covers (clears the owed nudges): PR #905, PR #906, PR #907, PR #908, and the
> recurring funcsize instances PR #892, PR #894, PR #896, PR #897, PR #898.
> Also PR #915 (Sprint-1 hygiene finish) and PR #917 (#22 SHA-pin + Lua-mirror
> smoke) — same root cause (test-light correctness diffs tripping `Test-delta
> gate` → `tests-out-of-band`). The preventing gate has since SHIPPED: PR #918
> taught `coverage-delta-gate.sh` to auto-exempt no-new-runtime-surface diffs, so
> this class should stop requiring the override going forward.

### What escaped
Two gate classes, both via sanctioned-but-cascading overrides:
1. **CodeRabbit never reviewed #905–#908** — `cr-out-of-band` downgraded the CR gate to WARN on all four — because a 6-PR burst exhausted CR's hourly per-developer quota. The green `CodeRabbit` / `CR findings (0 actionable)` checks were **status-only default-passes**, not real reviews (the only CR comment on the unreviewed PRs was the rate-limit notice). Nothing prevented opening PRs faster than CR's quota; CR demonstrably adds value (it caught a real `STL1001` terminology/accuracy issue on #909, which *was* reviewed before the limit).
2. **`Test-delta gate` fired RED on legitimately test-light correctness changes** — #906 (a `LOG_WARN` replacing an empty catch) and #907 (a compile-time `static_assert`) this session, and recurrently #892/#894/#896/#897/#898 (funcsize decompositions) — each merged via `tests-out-of-band`. The gate has no exemption for diffs that are inherently compile-time-tested / logging-only / no-new-runtime-surface.

(Also 2 admin force-merges of strict-`BEHIND` #908/#911 — deliberate, conflict-free, user-authorized to beat concurrent plan-doc churn; no gate owed per [ADR-0013](../adr/0013-solo-no-required-review.md), same as the 2026-05-23 entry.)

### Root cause
1. **CR quota**: the PR-batching rule (`AGENTS.md` § Autonomous ship-loop default — "one PR per logical *feature*… related slices accumulate on one branch") was violated. Sprint-1 was split into 6 subsystem PRs opened back-to-back, exceeding CodeRabbit's hourly per-developer limit. The rule is prose-only — nothing measures the burst or warns before it blows the quota.
2. **Test-delta shape**: `coverage-delta-gate.sh` keys purely on coverage / test-file delta. A correctness change with no new runtime surface (a `static_assert`, a `LOG_*` line, a marker/comment, a CMake edit, a pure relocation) *cannot* add coverage, so it always trips → recurring override. ≥7 PRs across two unrelated work-streams hitting the same override shows the gate's shape — not the PRs — is wrong.

### Preventing gate
1. **`pr-burst-guard`** (NEW) — a pre-ship check (wired into `scripts/dev/pre-ship.sh` / the autonomous ship-loop) that counts the author's open PRs + recent create-rate and WARNs (or pauses the loop) before opening a PR that would exceed CodeRabbit's hourly quota — enforcing the PR-batching rule mechanically instead of by prose. Filed to infra.
2. **Test-delta test-light exemption** (NEW) — extend `coverage-delta-gate.sh` to auto-PASS (no override needed) a diff whose product-code change is provably compile-time-tested / no-new-runtime-surface: `static_assert`-only, logging-only (`LOG_*` additions), comment/marker-only, or CMake-only. Removes the standing incentive to reach for `tests-out-of-band`. Filed to infra.

### Filed as
[`docs/self-improvement/categories/infra.md`](categories/infra.md) — two entries: `pr-burst-guard` (CR-quota-aware PR spacing) + `test-delta-test-light-exemption`.

## 2026-06-05 · PR #880, #881, #882 · required-but-path-filtered check deadlocked product-only PRs

> Discovered while a merge-gate poller exhausted its window against three PRs that
> were green on every other required check yet stuck `BLOCKED`.

### What escaped
The **branch-protection required-checks configuration** itself. `Doc anchors +
agent contract` is a **required** context (`project.config.json` §
`branch_protection.required_contexts`) but its workflow `doc-validation.yml` was
**path-filtered** to docs/agents paths. On a PR touching none of those paths the
workflow never runs → the required context is never reported → GitHub holds the
PR `BLOCKED` forever. No gate flagged that making a path-filtered workflow a
*required* context creates a permanent deadlock for any diff outside the filter.

### Root cause
The required context was added to live branch protection without a companion
always-runs emitter. The deadlock only manifests on a PR touching **none** of the
filtered paths — rare, because almost every PR also touches a `.md` (a plan / ADR
/ backlog update) which trips the filter. So it lay dormant: the last 8 pure-product
PRs (#766–#844) all merged 2026-06-03/04 *before* the context became live-required,
and #880/#881/#882 (pure product/test diffs) are the first to hit it. The merge-gates
poller's own `req-missing` detector (#877) correctly *flagged* the block — detection
worked; what was missing was a gate preventing the deadlock-prone config.

### Preventing gate
PR #884 — `doc-validation.yml` drops its `pull_request.paths` filter and the
`Doc anchors + agent contract` job **self-gates** (a `Detect doc-relevant changes`
step runs the real validation or no-ops green), so the required context is reported
on **every** PR. Durable class-fix (filed below): a selftest asserting every
`branch_protection.required_contexts` entry maps to a workflow that runs
unconditionally on `pull_request` (no `paths:` filter, or an internal self-gate) —
so a path-filtered required context can never be re-introduced.

### Filed as
`docs/self-improvement/categories/infra.md` — *required-context ⇄ unconditional-workflow parity selftest* (P1).

## 2026-06-05 · develop direct-push (`a678741f`) · direct push to `develop` (no PR/CI/CR)

> Self-reported. A one-line docs link fix was committed + pushed straight to
> `develop`, bypassing the PR/CI/CodeRabbit gates, because the orchestrator was on
> the `develop` branch (from a prior `git checkout develop` to start merge-gate
> pollers) and never switched back to the intended feature branch before
> `git commit` + `git pull && git push`.

### What escaped
The **PR-only / branch-protection gate** for `develop`. The fix (`a678741f` —
`docs/agent-rules/delegation.md`, adding the `../../` prefix to the
`scratchpad-recall` skill link) reached `develop` with no PR, no CI run, and no
CodeRabbit review. Branch protection (strict + required checks) did not block it
because the push used the repo owner's credentials (admin bypass).

### Root cause
Branch-state drift in a long multi-PR session. The orchestrator ran
`git checkout develop` to start the merge-gate pollers (which don't need a feature
branch), then — several steps later, fixing a CI failure on PR #858 — edited
`delegation.md` and `git commit`ed WITHOUT re-checking `git branch --show-current`.
The commit landed on local `develop`; `git pull --no-edit` (clearing a
non-fast-forward) then `git push` sent it to `origin/develop`. The content was
correct + locally-verified (`test-markdown-links` + `test-portable-purity` passed),
so no breakage shipped — develop's latent broken link was actually fixed — but the
gate was bypassed. The intended target was `claude/slice1-pre-first-push-gate`
(#858), which now inherits the fix via update-branch.

### Preventing gate
A git **`pre-push` hook that rejects any direct push to `develop` / `main`** from a
local same-named branch (a `develop -> develop` push) unless an explicit
`SMATCHET_ALLOW_DEVELOP_PUSH=1` escape is set — turning the branch-protection
contract (which admin credentials bypass) into a **local hard stop**. Pairs with an
orchestrator discipline: **verify `git branch --show-current` immediately before
every `git commit` in a multi-branch session** — the pollers' `git checkout develop`
is the recurring trigger for branch-state drift.

### Filed as
[`docs/self-improvement/categories/tooling.md`](categories/tooling.md) (2026-06-05, P2 — pre-push develop-guard hook + branch-verify discipline).

## 2026-06-04 · PR #844 · override label (`tests-out-of-band`)

> Flagged by `postmortem-owed.sh` because #844 merged carrying a `tests-out-of-band`
> override. Recorded for completeness; the override was **legitimate** — no new gate
> owed. Blameless: the gate worked, the escape hatch was used as designed.

### What escaped
The **Test-delta gate** (`FAIL: Source/Core/ changes without test deltas`) was
dismissed on #844 by the `tests-out-of-band` label. #844 shipped `Source/Core`
changes with no accompanying test delta.

### Root cause
#844's `Source/Core` diff was entirely a **build-define relocation** — moving
`NOMINMAX` / `WIN32_LEAN_AND_MEAN` out of two `.cpp` files into
`target_compile_definitions` (for the PCH-off publish path) plus dead-include /
dead-variable cleanup — all confined to `Source/Core/src/Ui/`, the **light** lint
zone. Ui is **excluded from the coverage surface** (`project.config.json`
`coverage.excluded`) and is exercised by **bucket-C/E screenshot tests**, not ctest
unit tests. A compile-define move + dead-code removal has **no executable-logic
surface a unit test could assert**, so the Test-delta gate's demand cannot be
satisfied in kind — the override label is the gate's intended escape hatch for
exactly this case, and the merge carried a justification comment naming the zone.
No breakage shipped (develop's post-merge build-and-test was green).

### Preventing gate
**None — override legitimate** (Ui-zone compile-define + dead-code change; the
`light` zone is coverage-excluded and bucket-C/E-tested, so no unit-test delta is
meaningful). A *possible* future tightening — auto-exempt diffs confined to the
`light`/`Ui/` zone that add or remove no executable statements, so the label isn't
needed — is **deferred**: a reliable "no logic changed" classifier is non-trivial
(real Ui logic changes must still demand tests), and the manual label + zone-citing
justification is the correct lightweight control today. Recorded, not gated.

### Filed as
No new category entry (no gate owed). This ledger entry is the record.

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
`docs/plans/shipped/ci-build-time-reduction.md:116` (`§ Scope-reduction edits +
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

> **SUPERSEDED 2026-06-07 (#920 decision, #950 config, CR-sweep CR-950-1/-2).**
> The preventing gate above (`strict: true`) was deliberately turned OFF by the
> merge-throughput decision (AGENTS.md § Merge gates: merge on own-head green;
> GitHub merge queue unavailable on a user-owned repo) — #950 aligned the config
> after #946's protection re-apply made the stale `strict:true` value live and
> immediately produced the BEHIND/update-branch dance the decision retired.
> **Accepted residual risk**: the concurrent-PR class this entry documents
> (gate-in-PR-A + violation-in-PR-B, both green alone) is REOPENED and has no
> structural mitigation — post-merge CI goes red on develop but blocks nothing
> (proven by this very incident). Accepted because: solo-dev cadence makes the
> A/B window small, the class has recurred once in ~3 weeks, and the structural
> fixes (strict rebasing every PR, or a merge queue) cost more than the class
> burns today. Revisit trigger: a second occurrence of this class, or the repo
> moving under an org (merge queue becomes available). Watch entry:
> `categories/infra.md` 2026-06-07 "strict-off residual".

## 2026-05-23 · commit 831d0342 (revert of c78ad386) · direct-push to develop, self-reverted

> Backfilled 2026-06-04 while sweeping stale `postmortem-owed.sh` hits. Recorded for
> completeness; the escape was a **deliberate, self-corrected** admin direct-push — no
> new gate owed for the revert itself. The sweep *did* surface a real tooling bug (the
> detector's false positives) — that fix is this entry's preventing gate.

### What escaped
`c78ad386` ("feat(p4-gated-ship-loop): split `p4-task-stream-to-pr.sh` into 3 modes +
AGENTS/ADR/AGENT_FLOWS rules") reached `develop` at 10:09 with **no PR, no `(#N)`, no
CI / CodeRabbit / merge-gate run** — a direct push — and was reverted 15 minutes later
by `831d0342` at 10:24. Both commits bypassed the entire PR ship-line.

### Root cause
A direct admin push to `develop` bypasses PR review, CI, CodeRabbit, and the
merge-gate poller entirely. This is *possible* because `enforce_admins=false`
(`project.config.json` `branch_protection`), a deliberate solo-repo tradeoff per
[ADR-0013](../adr/0013-solo-no-required-review.md) so the maintainer can break a
stale-`BLOCKED` state. Here it was used for a quick in-progress commit during the
p4-gated-ship-loop work that was immediately judged wrong and backed out; the script
split later re-landed properly through the normal ship-line (PR #609, scripts reorg).
No defect persisted on `develop` beyond the 15-minute window.

### Preventing gate
**For the revert: none — direct-push deliberate and self-corrected** (the revert *is*
the correction; `enforce_admins=false` is an intentional ADR-0013 tradeoff, not a hole
to close). **The actionable gate from this sweep is a `postmortem-owed.sh` fix**: the
detector used `git log --grep='^Revert'`, whose multiline `^` matched commit *bodies*,
so feature/docs PRs with revert *prose* ("Reverts the index row …" #512; "Reverted
the read-only widget …" #199) were flagged as phantom reverts owing postmortems. Fixed
to gate on the **subject** (`Revert "…"`) so only genuine revert commits trigger —
shipped in this PR. That stops the false-escape nudges that obscured this real one.

### Filed as
This entry + the `postmortem-owed.sh` subject-match fix in this PR (no category entry —
the revert owes no gate; the detector fix is the change).

## 2026-06-09 · PR #1092, #1095 · override: cr-out-of-band (+ phantom red Test-delta on #1095)

### What escaped
Both merged to `develop` with the `cr-out-of-band` label, waiving the CodeRabbit review
block: CodeRabbit was org-credit/rate-limited for the **entire session**, so the label was
applied to keep shipping. #1092 is a 12-file Core refactor (`refactor(tracker): dedup
query-suggest helpers`); #1095 is a docs-only ADR (`docs(adr): ADR-0019`) whose snapshot
*additionally* shows a red `Test-delta gate`.

### Root cause
Two distinct things the sweep conflated:
1. **#1092 (real)** — a Core-cpp refactor merged without an automated CodeRabbit pass. The
   override was *operationally* correct (CR genuinely unavailable), and the diff still
   cleared CI + delta-lint + the coverage gate + orchestrator review, so no defect shipped —
   but the automated review surface was skipped, a true (if low-residual-risk) gap.
2. **#1095 (false)** — `cr-out-of-band` on a docs-only diff waives nothing material (CR is
   advisory and adds little to prose), and the red `Test-delta gate` is a phantom:
   `coverage-delta-gate.sh` PASSES any diff with zero `Source/Core/src/*.cpp` files
   (`PROD_CHANGES==0 → exit 0`), so it cannot legitimately fail a docs-only ADR — the
   snapshot captured a transient non-terminal check state. Both triggers are Core-cpp-scoped
   yet fired on a no-Core-cpp PR.

### Preventing gate
`postmortem-owed.sh` now **drops a flagged PR whose SOLE trigger(s) are `cr-out-of-band`
and/or a red `Test-delta gate` when the PR touched no `Source/Core/src/*.cpp`** (new
`core_scoped_only_trigger` + `pr_touches_core_cpp` guards). Both gates are Core-cpp-scoped,
so their trigger on a non-Core-cpp diff is a false positive — same spirit as the
revert-subject fix above. This de-noises #1095 (and every future docs/non-Core PR) so the
ledger stays focused on real escapes like #1092. For #1092 the residual action is a
post-recovery CodeRabbit pass (CR-on-`develop`) once org credit is restored — tracked here,
not auto-enforced (advisory-CR, solo-repo human-on-the-loop).

### Filed as
This entry (resolves the owe for both PRs) + the `postmortem-owed.sh` Core-cpp-scope
de-noise gate in this PR.

## 2026-06-09 · PR #1096 · override: tests-out-of-band (+ red Test-delta gate)

### What escaped
`perf(ui): off-thread the toolbar per-tracker append disk read (#611 site #7)` merged with
the `tests-out-of-band` label dismissing the red `Test-delta gate`. The diff is UI-only
(`Source/Core/src/Ui/SmatchetToolbarUi.cpp` + its header) — moving a per-frame
`LoadPersistentViewsFromDisk` onto a `LaunchBackgroundTask` worker — with no paired test.

### Root cause
The coverage delta gate requires a paired `tests/Core/*.test.cpp` delta for any
`Source/Core/src/*.cpp` change, and that glob includes `Source/Core/src/Ui/` even though
ImGui render code has no unit-test surface (it is covered by bucket-C/E visual + scenario
harnesses, not doctest). `tests-out-of-band` is the documented, intended override for exactly
that case; it was applied correctly. No defect — the change only *removes* UI-thread work.
(Process note: it was armed for auto-merge and landed before the held visual sign-off; the
toolbar append behaviour should still be eyeballed post-merge, revert if wrong.)

### Preventing gate
`tests-out-of-band` on a diff whose only `Source/Core/src/*.cpp` files are under `Ui/` is an
**intended** override, not an escape — the same false-positive shape as the cr-out-of-band
Core-cpp-scope de-noise added in this PR. Deliberately NOT folded into that de-noise yet:
suppressing `tests-out-of-band` UI merges wholesale risks hiding a UI `.cpp` that *does* carry
testable non-render logic, so it stays a visible (cheap) ledger line pending a tighter
"render-only" classifier. Tracked here as the named follow-up gate.

### Filed as
This entry. Follow-up gate (render-only `tests-out-of-band` de-noise) noted, not yet shipped.

## 2026-06-14 · PR #1237 (+ #1232 #1227 #1220 #1198) · red-check: Sanitizer / Coverage merged while IN_PROGRESS

### What escaped
`fix(security): bound value.dump() fallbacks on deep server json (ASAN DoS)` (#1237) merged
to `develop` at 22:01:59Z while its `Sanitizer (ASAN via MSVC)` check was still
`status=in_progress` (started 22:01:12Z, completed `success` only at 22:13Z — 11 min AFTER
the merge). The merge-gate poller had recorded `GATES_PASSED` at 22:01:56Z with `0 fail,
0 pending`. Same shape as the four other owed escapes this session (#1232 ASAN, #1227
Coverage, #1220 ASAN, #1198 ASAN): each merged before a non-required allow-listed build
(Sanitizer/Coverage/Bucket) reached a terminal state. #1237 was benign only by luck — the
fix genuinely passed ASAN — but the gate provided no protection.

### Root cause
The #923 fix added the meant-to-block allow-list (`Coverage|Sanitizer|Bucket-|Perf
PR-fast|Android security gate`, non-advisory) to the poller's **`$failing`** set, so a
*terminally-failed* non-required allow-listed check blocks. But it never extended the
**pending** count, which was computed over `$req` (required contexts) ONLY:

    ([$req[] | select((.__typename=="CheckRun" and .status!="COMPLETED") or ...)] | length)

So a non-required allow-listed check still `IN_PROGRESS` (not yet terminal) fell into a blind
spot: not in `$failing` (not terminal-failed) and not in the pending count (not required) →
`fail==0 && pending==0` → `GATES_PASSED`. The merge fired the instant the 5 *required*
checks went green, racing — and beating — the ASAN/Coverage/Bucket build to the finish line.
GitHub's native auto-merge has the same blind spot (those checks are non-required), so this
also explains escapes that merged via `--auto`.

### Preventing gate
`merge-gates.sh` now binds a `$blocking` set = `$req` ∪ (non-required allow-listed
non-advisory contexts) — the SAME predicate `$failing` already unions — and computes the
**pending** count over `$blocking` instead of `$req`. An in-flight Sanitizer/Coverage/Bucket
now counts as pending, so the poller WAITS for it to finish (then `$failing` blocks if it
fails) instead of waving the merge through mid-build. Two `tests/bats/merge_gates.bats`
cases lock it: a non-required `Sanitizer (ASAN via MSVC)` `IN_PROGRESS` → `1 pending` → block;
an advisory non-allow-listed `IN_PROGRESS` → `0 pending` → still passes (prior contract
preserved). Closes the in-flight half of the #923 allow-list; the terminal-failure half was
already gated.

### Filed as
This entry (resolves the owe for #1237; the #1232/#1227/#1220/#1198 owes share this single
root cause and gate) + the `$blocking`-pending fix in this PR.

## 2026-07-10 · PR #1698 · red-check: test-orphan-bats (required "Doc anchors") ran POST-merge

### What escaped
`feat(test): mutation-smoke gate (Slice F)` (#1698) added `tests/bats/mutation_smoke.bats`
with **no `test-*.sh` wrapper**. `scripts/dev/test-all.sh` discovers suites by the
`test-*.sh` glob and never runs a bare `.bats`, so the suite was both dead weight CI never
executed AND flagged by `test-orphan-bats.sh` — the gate that exists precisely to catch an
un-wrapped `.bats`. That gate lives inside the **required** `Doc anchors + agent contract`
check. #1698 merged at `08:48:56Z`; its `Doc anchors` check-runs did not *start* until
`08:49:56Z` (failure) / `08:50:41Z` (success) — the required check ran entirely **after** the
merge. So the orphan landed on `develop` un-caught, `Doc anchors` has been RED on the develop
tip since, and under **block-on-any-red** that red was inherited onto every open PR's own
head (it blocked #1704 / the #1666 fix, which is how it surfaced).

### Root cause
Two independent safety nets both failed to fire *before* the merge:
1. **Local pre-ship** — `scripts/dev/pre-ship.sh` runs `test-docs.sh`, which DOES invoke
   `test-orphan-bats.sh` (line ~60). Had the author run pre-ship after adding the `.bats`, it
   would have flagged locally. Evidently not run (or run before the file was added).
2. **Required CI** — same class as the #1237 family (merged-before-terminal): the merge fired
   ~60 s *before* the required `Doc anchors` check even started, so CI could not block it. The
   squash was committed by GitHub (committer `GitHub`, author the repo owner) with no override
   label. The precise merge-path cause (native auto-merge racing an un-started required
   context, a required-contexts reconfiguration around the merge instant, or a manual merge)
   is **not fully determinable** from the available APIs (branch-protection has no history
   endpoint) — flagged honestly rather than guessed. What IS certain from the timestamps: the
   required check had not run at merge time.

The detecting gate worked; the miss was purely *timing* — a required check that runs post-merge
provides zero pre-merge protection, and a single un-wrapped `.bats` then converts into a
develop-wide, every-PR block.

### Preventing gate
Instance fixed in **#1705** (`agents/scripts/core/test-mutation-smoke.sh` — the missing
wrapper; `test-orphan-bats` now PASSES, 63/63 suites wrapped, suite runs 9/9 green).
Class prevention proposed (not yet landed): a lightweight **develop-tip health assertion** —
a SessionStart / periodic check (e.g. extend `agents/scripts/core/postmortem-owed.sh`'s sweep,
or a new `develop-tip-required-green.sh`) that queries the develop tip's *required* check
conclusions and raises a loud, attributable nudge the moment any required context (here
`Doc anchors + agent contract`) is RED — converting "silent red develop silently blocks every
PR" into an immediate signal tied to the PR that introduced it, instead of the next author
discovering it by inheritance. This is the durable complement to #1705: the wrapper stops
*this* orphan; the tip-health assert stops the *class* of "required check goes red on develop
and nobody notices until it blocks the next PR" (of which the #1237-family merge-before-terminal
race is one upstream cause).

### Filed as
This entry + the #1705 wrapper (instance) + a follow-up backlog item for the develop-tip
health assertion (class).

## 2026-08-05 · PR #1957 · red-check: Windows x64 / ARM64 installer smoke red on develop for 3 merges

### What escaped
Slice E of the kill-PowerShell plan (#1957, `1ec9fb0c`) ported `scripts/publish/release_github.ps1`
to `scripts/publish/release-github.sh`. The port builds Inno Setup's argv as
`/DMyAppVersion=$PROJECT_VERSION`, `/DMySourceDir=…` etc. Git Bash rewrites a *whole* argument
that looks like an absolute POSIX path, so `/DMyAppVersion=0.1.0` reached ISCC.exe as
`C:/Program Files/Git/DMyAppVersion=0.1.0` — no longer a switch. ISCC counted it as a second
script filename and aborted:

```
You may not specify more than one script filename.
```

Both `Windows x64 installer smoke` and `Windows-on-ARM ARM64 installer smoke (runner-gated)`
have been RED on develop since #1957. #1959 and #1960 then merged on top of a red develop.

### Root cause
Three independent failures compounded:

1. **The bug itself is a knowledge gap, not a slip.** The author *half*-anticipated it — the
   surviving comment reads "MSYS argument translation is not dependable for /D-style switches,
   so convert explicitly" — but converted only the *values* via `winpath()`. The leading `/D`
   is what triggers the rewrite; converting the value cannot help. (An argument whose value
   already looks like a Windows path, e.g. `/DMySourceDir=D:\a\x`, survives — which is exactly
   why the failure looked arbitrary.) The same exposure applies to `signtool`'s
   `/fd /td /tr /f /p /sm /sha1 /n /pa` and to a bare `cmd.exe /c` (`/c` → `C:/`).
2. **No pre-merge gate could see it.** The installer-smoke jobs are deliberate POST-MERGE
   BACKSTOPS — `if: github.event_name == 'push' || 'workflow_dispatch'`, never on
   `pull_request` or `merge_group`, because the LTO publish build costs ~20–30 min. So a break
   in `release-github.sh` is structurally invisible to PR checks.
3. **The post-merge red was never surfaced.** #1957's own develop run was *cancelled* by a
   superseding push, so the failure did not even announce itself on the PR. Nothing asserts
   develop-tip health at the next merge, so two more PRs merged on top of a red develop
   without any signal.

Failure 3 is the **same class** the 2026-07-10 / #1698 entry already proposed a gate for
(develop-tip health assertion) and which was filed as backlog rather than landed. This
incident is that backlog item's second occurrence — evidence to promote it.

### Preventing gate
Instance + class fixed in this PR:

- `scripts/publish/release-github.sh` gains `native_exec()` (`MSYS_NO_PATHCONV=1 "$@"`), applied
  at the ISCC invocation and both signtool call sites. The signtool sites were latent
  release-blockers: CI has no signing cert, so they would only have bitten on a real signed
  release.
- Same guard applied to the three sibling `cmd.exe /c` call sites the sweep surfaced —
  `scripts/dev/local/build-deploy-and-open-unreal.sh` (×2, Unreal `Build.bat` never ran),
  `scripts/dev/local/rebuild-testproject-plugin.sh`, and `scripts/dev/coverage.sh` (the merge
  carrier was silently a `cmd.exe` with no command switch).
- **New gate**: `tests/bats/msys_argv_switches.bats` +
  `scripts/dev/test-msys-argv-switches-bats.sh` (auto-enrolled by `test-all.sh`, so it runs on
  every PR — closing failure 2 for this bug class without paying the publish-build cost). It
  asserts statically that every native-Windows-exe `/switch` invocation across all tracked
  `*.sh` carries `MSYS_NO_PATHCONV=1` / `native_exec`, and behaviourally (Windows-only) that
  MSYS really does mangle a bare `/switch` and that the guard restores it. All five cases are
  mutation-proven: removing each guard turns the matching case red.

Still open (class, failure 3): the develop-tip health assertion proposed in the 2026-07-10
entry. Second occurrence now recorded; backlogged rather than landed here to keep this PR
scoped to the break.

### Filed as
This entry + the `native_exec` / `MSYS_NO_PATHCONV` fixes + the new bats gate in this PR.
## 2026-08-06 · PR #1937 · masked gate: bucket-C golden diff swallowed a stale-golden verdict

### What escaped
`feat(about): About Smatchet dialog under Help` (#1937) moved `drawMenuBarHelpMenu(ctx)`
outside the `trackerLocked` `BeginDisabled()` block in `SmatchetUI_MainMenu.cpp:142-174`,
which permanently changed the rendered "Help" menu-bar label from dim `(154,154,154)` to
bright `(232,232,232)`. Four `user-info-*` bucket-C goldens were not regenerated. The
bucket-C golden-diff step then failed on every subsequent run — `linf=81`, confined to
`y=[8,19] x=[273,296]` — and reported green anyway, because that step is one of the three
sanctioned step-level masks (`AGENTS.md` § Merge gates). Found ~3 weeks later, by hand,
while debugging an unrelated screenshot flake. Three further goldens
(`code-syntax-coloring`, `command-palette-fuzzy`, `dock-gap-sentinel`, `linf≈240`) turn out
to be stale the same way from an older theme-palette change (bg `(15,15,15)` → `(31,31,36)`);
those files date to 2026-05-26/31. Seven stale goldens, zero signals emitted.

### Root cause
The mask is **total, not graduated**. It was added for a real reason — scenario
nondeterminism must not block merge — but it discards the step's verdict rather than
downgrading it, so a golden stale for a perfectly *deterministic* reason (a deliberate,
permanent UI change nobody regenerated for) is indistinguishable from a clean run. The
detecting gate ran, computed the correct answer, and threw it away.

Nothing else covers the gap: `postmortem-owed.sh` keys on merge-instant signals
(non-SUCCESS checks, override labels, `Revert`, overdue deviations) and a masked step emits
none, so no owe was raised for #1937. Goldens have no age nag, no staleness inventory, and
no expiry — the only detection path is a human diffing them by hand, which is exactly what
happened, three weeks late.

Same shape as the 2026-07-10 #1698 entry (a working gate whose *result* never reached a
blocking position), but the failure is in signal handling rather than timing: there, a
required check ran after the merge; here, it ran before and its answer was suppressed.

### Preventing gate
**Split reporting from blocking — a masked step must still publish its verdict.** Have the
bucket-C step always write per-scenario results (`name`, `linf`, band `y=[a,b]`, golden
mtime) to the job summary / an artifact regardless of the mask, and surface that on the PR.
A stale golden then lands attributably on the PR that changed the pixels without gaining the
power to block a flaky lane. This generalises to the other two sanctioned masks (fuzz-smoke's
stochastic run, bucket-E's Mesa per-test run), which discard their verdicts for the same
reason.

Instance ratchet, available now: PR #1962 removes the three nondeterminism sources behind
the `user-info-*` flake (pre-`Draw` dispatcher-drain clobber of `g_ui.cfg`, docked-tab focus
via `TabBar->NextSelectedTabId`, and ephemeral-session suppression of the update modal),
measured 0/20 deviations twice. The flakiness that justified masking no longer applies to
that subset, so `user-info-*` can carry an **unmasked** diff while the rest stay masked
pending their own determinism work (`ScenarioRunner::Tick` runs twice per rendered frame,
duplicating content for any scenario that draws in `OnFrame`).

Prerequisite for both: regenerating the seven stale goldens, which is approval-gated by
`docs/agent-rules/golden-image-approval.md`.

### Filed as
This entry + [`tooling/2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md`](categories/tooling/2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md)
(the report-don't-discard gate) + PR #1962 (the determinism work the instance ratchet rests on).
