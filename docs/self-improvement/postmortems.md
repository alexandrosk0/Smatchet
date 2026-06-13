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
