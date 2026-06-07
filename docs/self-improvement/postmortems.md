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
(`Smatchet.exe cmd app.version --spawn --yes` or equivalent ≤ 10 s probe)
after Mesa install and before the advisory bucket step. "The exe cannot even
start" then fails the job hard regardless of how flaky the tests behind it
are — separating *dead harness* (hard fail) from *flaky tests* (advisory).
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
