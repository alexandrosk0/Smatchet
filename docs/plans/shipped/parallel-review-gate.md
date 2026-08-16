# Plan — Parallel code-review gate (review runs *with* the lint gate, not after it)
<!-- plan-date: 2026-08-15 -->

> **Slug**: `parallel-review-gate` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

`scripts/dev/pre-ship.sh` runs the code-review gate **last**, after clang-format,
`test-lint-rules.sh --diff`, md_lint, test-list, orphan-bats and the doc suite. By the
time it fires — minutes in, at the end of a wall of green — the author is one
`--ack-review` away from "Safe to push", and the cheapest way to clear it is to record
the ack rather than dispatch a review. Two failure modes follow from the ordering:

1. **Serial latency.** The review can only start after the gate has already told you it
   is needed, so review time is *added* to gate time instead of overlapping it.
2. **Ack-without-review.** `--ack-review` records a fingerprint and nothing else. A bare
   ack and a real review are byte-identical to the gate.

Prompted by the user question *"how can we make sure code review will happen at the same
time as `test-lint-rules.sh`"*. After this lands: the "review REQUIRED" notice prints at
t=0 (before any lint stage), the ship-loop dispatches `code-review` in the **same
tool-call batch** as the gate run, and `--ack-review` refuses to record without a
review-findings artifact whose fingerprint matches the diff being acked.

## Approach

**Protocol-parallel, not shell-parallel.** No LLM call inside the shell gate, no CI job,
no push-time hook (all three ruled out). The gate publishes *when* a review is needed as
early as it can compute it, and `docs/agent-rules/ship-loops.md` makes the orchestrator
launch the reviewer concurrently with the gate.

The blocker to naive parallelism is that `pre-ship.sh` **mutates the diff before it
lints it** — `clang-format -i` and `comment_audit.py --fix`. A reviewer dispatched
alongside a bare `pre-ship.sh` would read pre-format code and compute a fingerprint that
is stale the moment the format lands. So the mutating half is split into a new blocking
`--format-only` step; everything after it is read-only w.r.t. the C++ diff and safe to
overlap:

```
1a. bash scripts/dev/pre-ship.sh --format-only   (blocking; the only diff-mutating step)
1b. SAME tool-call batch, concurrent:
      bash scripts/dev/pre-ship.sh   ∥   code-review agent
1c. bash scripts/dev/pre-ship.sh --ack-review    (validates the artifact, then stamps)
```

**Enforcement = artifact, not honour.** The reviewer's last action is to write
`.review-findings.json` stamped with `bash scripts/dev/pre-ship.sh --review-fingerprint`.
`--ack-review` requires that file to exist with a `fingerprint` field equal to the
current branch fingerprint, and the final gate re-checks it. A bare `--ack-review` with
no review can no longer pass. This closes the *forgot / rubber-stamped* hole; it does
not close the adversarial one (`SMATCHET_SKIP_REVIEW_GATE=1` is still the logged escape,
and a hand-written JSON still passes) — that is accepted, stated, and unchanged from the
existing ack's threat model.

Fingerprint parsing uses `grep -oE` on the 64-hex field, **not** python or jq: the gate
already has a documented #1116 fail-closed path for "no working python", and adding a
second interpreter dependency to the artifact check would either widen that failure
surface or invite a WARN-degrade that quietly re-opens the hole.

## Files to modify

1. [`scripts/dev/pre-ship.sh`](../../scripts/dev/pre-ship.sh) — the whole change lands here:
   `--format-only` and `--review-fingerprint` flags; the diff-mutating stages split out of
   the lint block; a t=0 "review REQUIRED" notice above the lint stages; the
   `.review-findings.json` requirement on `--ack-review` and on the final gate; two new
   `run_selftest()` cases.
2. [`agents/core/code-review.md`](../../agents/core/code-review.md) — mandate writing the
   stamped artifact as the reviewer's final action; `version: 5` → `6` + both banner lines.
3. [`docs/agent-rules/ship-loops.md:19`](../agent-rules/ship-loops.md) — replace pre-first-push
   gate item 1 with the 1a/1b/1c parallel protocol.
4. [`.gitignore`](../../.gitignore) — `.review-findings.json` next to the existing `.review-ack`
   entry (local, per-worktree evidence, never committed).

No new TU (`rg` check not applicable — no C++ in this diff). No `SMATCHET_WITH_*` gating touched.

## Existing utilities reused

- `ra_fingerprint branch <base>` — `agents/scripts/core/lib/review-ack.sh:110` — the exact
  hash `--ack-review` already pins; `--review-fingerprint` prints it verbatim so the
  artifact and the ack can never disagree on how the diff is hashed.
- `ra_is_substantive` / `RA_SUBSTANTIVE_REASON` — `agents/scripts/core/lib/review-ack.sh:189` —
  reused unchanged for the t=0 notice, so the early message and the late gate share one
  definition of "substantive".
- `ra_read_marker` / `ra_write_marker` / `ra_read_verdict` — `review-ack.sh:210,243,277` —
  the ack record format and the verifier-verdict path are untouched.
- `SMATCHET_PRESHIP_GATE_ONLY=1` — `pre-ship.sh:291` — the existing selftest hook that
  skips the lint stages; the two new selftest cases ride it.
- `agents/scripts/core/test-gate-selftests.sh` — the gate-that-gates-the-gates; `pre-ship.sh`
  already carries the `# selftest: asserts-failure` marker, so extending `run_selftest()`
  needs no enrolment change.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — shell tooling only, no
  product code in the diff.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no UI code.
- **Pillar 3 (never crash)**: no impact on the shipped binary. Gate-side, the new checks
  fail **closed** (missing/unparseable artifact ⇒ block), never fail-open.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — the diff touches only shell tooling, one agent prompt, one rule-doc and `.gitignore`;
no `Source/Core/` file is modified, so none of the five gates (PR-fast scenario, Pillar-2
static scanner, dispatcher drain, visible-cue bucket-E harness, marker inventory) fires.

## Risks / non-goals

- **Risk: `--format-only` drift.** The mutating stages now live in one branch and the lint
  stages in another; a future edit could add a mutator to the wrong half and re-open the
  stale-fingerprint hole. *Mitigation*: the split is a single `if`/`fi` pair with a comment
  naming the invariant ("everything below is read-only w.r.t. the C++ diff").
- **Risk: the artifact becomes a rubber stamp of its own.** A reviewer that writes findings
  it did not derive passes. *Accepted* — same threat model as the existing ack; the gate
  verifies review *presence*, never review *quality* (that is what the verifier-verdict
  work in `docs/plans/verifier-scored-code-review-gate.md` is for).
- **Risk: an extra required file annoys the small-diff path.** *Mitigation*: the artifact is
  required **only** when the diff is already substantive (strict-zone touch or
  `>= REVIEW_LINE_THRESHOLD` lines) — a small non-strict diff still N/A-passes untouched.
- **Non-goal**: a CI job or a push-time hook enforcing review presence server-side (both
  explicitly ruled out by the user).
- **Non-goal**: changing the COMMIT-side gate (`scripts/git-hooks/pre-commit` via
  `agents/scripts/core/review-ack.sh`, mode `staged`). It keeps its current ack-only
  contract; only the PUSH-side gate gains the artifact requirement.
- **Non-goal**: scoring review quality — presence-only, as today.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: `N/A — no C++ logic changed.`
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: `N/A — no UI surface.`
- **Bash-driver scenario / screenshot / sanitizer**: `bash scripts/dev/pre-ship.sh --selftest`
  is the automated proof — it asserts the gate BLOCKS an unacked substantive diff, blocks an
  `--ack-review` with **no** artifact, blocks one with a **stale-fingerprint** artifact, passes
  with a matching artifact, re-arms on a post-ack edit, honours the documented bypass, and
  keeps the #1116 no-python fail-closed path. Plus `bash agents/scripts/core/test-gate-selftests.sh`
  (the gate-that-gates-the-gates) and a real `bash scripts/dev/pre-ship.sh` run over this branch.
- **Build gate**: `N/A — no C++ in the diff; a dual-target build would compile an unchanged tree.`
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical
  `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors /
  agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script,
  don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the
  domain model + sharpen terms before finalising; record the outcome.
- **Manual residue**: none — every claim above is asserted by `--selftest` or by a suite that runs in CI.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction
edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and
`docs/self-improvement/categories/` for stray references to anything deferred here, and revise
or delete them.

- **Verifier-scored gating** — `docs/plans/verifier-scored-code-review-gate.md` already owns
  promoting the advisory score to a blocking one. This plan only makes the *presence* signal
  honest; no change to the score's advisory status.
- **Commit-side artifact requirement** — the `staged`-mode gate could demand the same artifact.
  Deliberately deferred: the staged fingerprint and the branch fingerprint differ textually for
  identical content, so it needs its own stamping mode. Follow-up only if the push-side artifact
  proves it earns its keep.
- **Structured findings enforcement** (block on a recorded Critical/High) — the artifact carries
  counts, but nothing acts on them yet. Natural next slice; not designed here.

## Implementation log

- `1b27872b` · `wip(plan): parallel-review-gate` — this plan doc, committed before implementation per § Plan-doc safety.
- `f3de34d5` · `feat(gate): run code review concurrently with the pre-ship lint gate` — the whole change, four files:
  - `scripts/dev/pre-ship.sh` — split into **Group A** (the two diff-MUTATING stages: `clang-format -i`, `comment_audit.py --fix`) and **Group B** (the read-only stages: delta lint, md_lint, test-list, orphan-bats, `test-docs.sh`), each its own `if [ "$gate_only" != "1" ]` block closed by a `fi # end of Group X` comment naming the invariant. New flags `--format-only` (runs Group A, prints the concurrency instructions, exits 0) and `--review-fingerprint` (prints `ra_fingerprint branch <base>` and exits, nothing else). The t=0 "CODE REVIEW REQUIRED" notice + fingerprint print between the groups. New `preship_findings_fingerprint` / `preship_require_findings` enforce `.review-findings.json` on both `--ack-review` and the final gate.
  - `agents/core/code-review.md` — new mandatory Process step 7 (write the stamped artifact as the reviewer's LAST action); `version: 6` → `7` + both banner strings.
  - `docs/agent-rules/ship-loops.md:19` — pre-first-push gate item 1 → the 1a/1b/1c protocol.
  - `.gitignore` — `.review-findings.json` beside `.review-ack`.
- `test(gate): cover the review-artifact requirement in CI` — the regression coverage the first CI run showed was missing:
  - `tests/bats/preship_review_artifact.bats` (8 tests) + `agents/scripts/core/test-preship-review-artifact-bats.sh` — the artifact requirement itself, in a suite CI actually runs.
  - `tests/bats/verifier_preship_wiring.bats` — fixture repair: all 9 of its tests drive a strict-zone (substantive) diff, so the new requirement blocked them. `setup()` now stamps a matching artifact via a shared `diff_fingerprint()` helper, which also de-duplicates the inline fingerprint pipeline the last two tests each carried.

## Deviations from plan

- **`version: 5` → `6` was predicted; `6` → `7` shipped.** § Files to modify pinned the version current when the plan was written; `code-review.md` had already been bumped by implementation time.
- **The intent-to-add block was extracted, not merely moved.** The plan said only that the mutating stages split out of the lint block. In practice `--review-fingerprint` needs the ita registration too — a brand-new untracked `.cpp` is invisible to `git diff HEAD`, so the reviewer would stamp a hash that could never match. It became `preship_ita_untracked [quiet]`, called from both the Group A path and the `--review-fingerprint` path, with `quiet` keeping the fingerprint output machine-readable. A correctness fix the plan missed, not a scope change.
- **Three selftest cases, not two.** § Files to modify said "two new `run_selftest()` cases"; the old case 2 was *replaced* by **2a** (ack with no artifact ⇒ MUST FAIL), **2b** (ack with a mismatched artifact ⇒ MUST FAIL) and **2c** (artifact stamped via `--review-fingerprint`, then ack ⇒ MUST PASS, asserted on both the ack and a plain gate run). Cases 1, 3, 4, 5a, 5b unchanged.
- **`--ack-review` honours `SMATCHET_SKIP_REVIEW_GATE=1` for the artifact check too** (WARN, not silent). Unplanned but required: without it the documented bypass would clear the final gate yet still wedge on the ack — worse than having no bypass.
- **`--selftest` alone was not CI coverage.** The plan treated the three new `run_selftest()` cases as the regression net. They are not: no CI job invokes `pre-ship.sh --selftest` (`test-gate-selftests.sh` only asserts that the script *exposes* a failure case, never runs it). The first CI run made this concrete from the other side — the new requirement broke `verifier_preship_wiring.bats`, a suite CI *does* run, which `--selftest` had no way to predict. Fixed by adding `tests/bats/preship_review_artifact.bats` + its wrapper.
- Nothing deferred. Every § Files-to-modify row shipped.

## Verification (actual)

| Check | Result |
|---|---|
| `bash -n scripts/dev/pre-ship.sh` | **PASS** — syntax clean |
| `bash scripts/dev/pre-ship.sh --selftest` | **PASS** — `gate blocks unacked substantive diffs, refuses an ack with a missing/stale review artifact, acks with a stamped one, re-arms on edit, bypass works, #1116 fail-closed on no-python` |
| `bash agents/scripts/core/test-gate-selftests.sh` | **PASS** — `all 80 --selftest-exposing scripts assert a failure case` |
| `bash agents/scripts/core/test-preship-review-artifact-bats.sh` | **PASS** — `Passed: 8  Failed: 0` |
| `bash agents/scripts/core/test-verifier-preship-wiring-bats.sh` | **PASS on CI** — locally `Passed: 4  Failed: 5`; all 5 are `start_endpoint … failed with status 49` (this Windows box has only the `python3` App-Execution-Alias stub). The 4 python-free tests, including both that the artifact requirement had broken, pass |
| `bash agents/scripts/core/test-orphan-bats.sh` | **PASS** — `all 90 bats suite(s) have a test-*.sh wrapper` (the new suite included) |
| `shellcheck -S warning agents/scripts/core/test-preship-review-artifact-bats.sh` | **PASS** — no output |
| `shellcheck -S warning scripts/dev/pre-ship.sh` | **PASS** — no output |
| `bash scripts/dev/pre-ship.sh` (full run, this branch) | **PASS** — exit 0; review gate N/A (no first-party C++ in this diff) |
| `bash scripts/dev/pre-ship.sh --review-fingerprint` | **PASS** — printed the sha256 of the empty C++ diff, correct for a docs+shell branch |
| `bash scripts/dev/pre-ship.sh --format-only` | **PASS** — printed the diff-stable PASS + the concurrency instructions, exited 0 |
| `bash agents/scripts/core/test-plan-index.sh` | **PASS** — `index up to date (189 plans)` while this plan sat under `active/` (the index covers `shipped/` only); re-run with `--fix` after the archive `git mv` → `rewrote index (190 plans)` |
| Bucket A (ctest `test-rig`) | **N/A** — no C++ logic changed |
| Bucket E (ImGui Test Engine) | **N/A** — no UI surface |
| Build gate | **N/A** — no C++ in the diff |

Manual residue: **none** — every row above is a command, and `--selftest` is enrolled in `test-gate-selftests.sh`, which CI runs.
