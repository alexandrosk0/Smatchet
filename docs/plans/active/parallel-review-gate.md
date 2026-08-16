# Plan — Parallel code-review gate (review runs *with* the lint gate, not after it)

> **Slug**: `parallel-review-gate` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
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
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
