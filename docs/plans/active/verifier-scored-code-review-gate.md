# Plan — Verifier-scored code-review gate

> **Slug**: `verifier-scored-code-review-gate`
>
> **Status**: `active`

## Context

PR #1964 made the `code-review` pass mechanically enforced at commit time: `scripts/git-hooks/pre-commit` check (B) refuses a substantive staged C++ diff whose fingerprint is not recorded in `.review-ack`. That closed the "nobody ran a review at all" hole — an audit caught a live session about to commit 24 modified + 6 new files with no `code-review` dispatch at any point.

It deliberately did **not** close the next one. The PR states it plainly: *"The hook gates the presence of a review, never its verdict — a recorded ack with unfixed Critical/High findings is a process violation it cannot see."* A fingerprint proves the diff has not changed since **something** was acknowledged. It cannot distinguish a rigorous review from a rubber stamp, and an agent that would skip the review can record a clean ack just as cheaply.

The project already has the machinery to score verdict quality. [`docs/plans/llm-verifier-sidecar.md`](llm-verifier-sidecar.md) shipped a re-derivation of the *LLM-as-a-Verifier* framework — `scripts/dev/verifier-sidecar.py` (deterministic aggregator), `verifier-produce.py` (model-calling half, constrained single-token A–T logprob scoring), `verifier-calibrate.py` (advisory→blocking promotion gate), `verifier-endpoint.py` (offline stub). It is wired into exactly one review path: `agents/_shared/workflows/pre-merge-review.js`, at PR/merge time.

**Intended outcome**: after this lands, the commit-time review ack carries a verifier score from an *independent* model, `hard_veto` blocks the commit immediately, and the continuous score is recorded as advisory evidence until `verifier-calibrate.py` says it is trustworthy enough to block on.

## Approach

Extend the `.review-ack` record from `<mode>\t<sha256>` to additionally carry the verdict — `overall_score`, `recommendation`, `hard_veto` — produced by running the existing verifier pipeline over the `code-review` output, then teach `pre-commit` check (B) a third state. Today it has two: no/stale ack → block, current ack → pass. It gains: current ack **with** `hard_veto` (or, post-calibration, a sub-threshold score) → block, naming the veto reason.

Two decisions shape this, and both are inherited from `docs/agent-rules/verifier-sidecar.md` § Operating rules rather than invented here:

**Score is advisory; veto blocks on day one.** Rule 5 ("calibrate before blocking") forbids gating on a continuous score before `verifier-calibrate.py` clears `min_samples`/`max_brier`/`max_ece`/`min_auc`. But rule 2 ("veto beats average") describes a categorical finding — a real security issue or invariant breach — not a probabilistic forecast, so blocking on `hard_veto` trusts no threshold and needs no calibration. This mirrors the WARN-first-then-graduate path `dup_audit` took (ADR-0015).

**The verifier must not be the reviewer.** If the agent that writes the ack also scores its own review, this is self-certification with extra steps and is strictly worse than the current honest binary — it manufactures a quality signal where none exists. `verifier-produce.py` is what makes the design sound: it calls a separate OpenAI-compatible backend, so the score comes from a different model than the one under evaluation. If no independent backend is configured, the gate records *no score* and stays in today's presence-only behaviour — it must never fall back to self-scoring.

Scoring runs in `branch` mode at pre-push (`scripts/dev/pre-ship.sh`), not on every commit: cost scales as candidates × criteria × repeats, which is wrong in front of `git commit`. The commit hook consumes a recorded verdict when one exists; it does not produce one.

## Files to modify

1. [`agents/scripts/core/lib/review-ack.sh`](../../../agents/scripts/core/lib/review-ack.sh) — extend the marker schema to `<mode>\t<sha256>\t<score>\t<recommendation>\t<hard_veto>`; add `ra_read_verdict <mode>` / `ra_write_verdict`. Both the 2-field (PR #1964) and legacy bare-sha forms must keep parsing — this file already carries a legacy-migration path to model it on.
2. [`agents/scripts/core/review-ack.sh`](../../../agents/scripts/core/review-ack.sh) — `--record` gains `--verdict <file>` (the `verifier-sidecar.py aggregate` output) and `--check` gains veto enforcement + advisory score reporting. New rc: keep 0/1/2 semantics, veto surfaces as rc 1 with a distinct message.
3. [`scripts/git-hooks/pre-commit`](../../../scripts/git-hooks/pre-commit) — check (B) renders the veto reason and the advisory score. No new blocking condition beyond veto in this slice.
4. [`scripts/dev/pre-ship.sh`](../../../scripts/dev/pre-ship.sh) — after the existing review gate, when an independent verifier backend is configured, drive `verifier-produce.py | verifier-sidecar.py aggregate` over the branch diff and record the verdict with the ack.
5. [`agents/core/code-review.md`](../../../agents/core/code-review.md) — emit the verifier object (same schema `pre-merge-review.js` already requires) alongside the severity punch-list, and pass it to `--record --verdict`.
6. [`agents/_shared/skills/adversarial-code-review/SKILL.md`](../../../agents/_shared/skills/adversarial-code-review/SKILL.md) — parity edit; held in lockstep by `test-skill-vs-agent-parity.sh`.
7. [`docs/agent-rules/verifier-sidecar.md`](../../agent-rules/verifier-sidecar.md) — add the commit/push gate to § Where it runs first.
8. [`docs/agent-rules/process-rules.md`](../../agent-rules/process-rules.md) + [`ship-loops.md`](../../agent-rules/ship-loops.md) — document the veto block and the advisory score.
9. `tests/bats/review_ack_gate.bats` + a new `tests/bats/verifier_review_gate.bats` — cases below.

## Existing utilities reused

- `ra_fingerprint` / `ra_is_substantive` / `ra_read_marker` / `ra_write_marker` — `agents/scripts/core/lib/review-ack.sh` (PR #1964). The whole substantive-diff and staleness mechanism is unchanged; only the payload grows.
- `verifier-sidecar.py aggregate` — `scripts/dev/verifier-sidecar.py`. Deterministic, pure-stdlib, already emits `overall_score`, `uncertainty`, `hard_veto`, `recommendation`. Consume it as data; do not reimplement the reward math.
- `verifier-produce.py` — `scripts/dev/verifier-produce.py`. The model-calling half, incl. `--record` trace capture (which is exactly the calibration evidence this plan needs to accumulate) and `--mode scalar` fallback.
- `verifier-endpoint.py` — `scripts/dev/verifier-endpoint.py`. Deterministic canned OpenAI-compatible endpoint; makes the live path testable with no model or key.
- `verifier-calibrate.py` — `scripts/dev/verifier-calibrate.py`. The promotion gate; `--gate` gives the future CI job that guards flipping the score to blocking.
- The verifier object schema in `agents/_shared/workflows/pre-merge-review.js:60-110` — reuse verbatim so both review paths emit one shape.

## Extraction sizing

N/A — this plan adds behaviour to existing files and extracts no bodies out of an over-cap file.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — agentic tooling only, no code compiled into the app.
- **Pillar 2 (UI-thread never blocks)**: no impact — no `Source/` code path touched.
- **Pillar 3 (never crash)**: no impact on the shipped binary. Developer-facing failure mode is a wedged commit, mitigated by fail-open on infra (rc 2) and `SMATCHET_SKIP_REVIEW_GATE=1`.
- **Pillar 4 (accessibility)**: no impact — no UI surface.

## Perf-review-system gates

N/A — the diff touches no `Source/Core/` file; it is shell, Python-adjacent config, docs, and bats.

## Risks / non-goals

- **Self-scoring collapse** (the one that matters): if the reviewing model also scores itself, the gate manufactures false assurance. *Mitigation*: the verdict is recorded only when an independent `VERIFIER_BASE_URL`/`VERIFIER_MODEL` is configured and distinct from the reviewing agent's model; otherwise no verdict is written and behaviour is unchanged. A bats case asserts the no-backend path records nothing rather than self-scoring.
- **Score inflation / reward hacking**: an agent learns that a high `overall_score` unblocks. *Mitigation*: the score is advisory-only until calibrated, and it is the independent verifier's output, not the reviewer's claim. Accepted residual risk until calibration data exists.
- **Anthropic backend has no logprobs today** — a Claude-backed verifier runs `--mode scalar` and loses the `variance`/`valid_mass` that make `uncertainty` meaningful. *Accepted*: scalar still yields a usable point score; the fine-grained path needs vLLM/SGLang/DeepSeek per `docs/agent-rules/verifier-sidecar.md` § Using DeepSeek.
- **Cost and latency**: criteria × repeats model calls. *Mitigation*: scoring runs at pre-push, not per-commit; the sidecar's SEM-driven repeat schedule resolves low-risk diffs in one pass.
- **A network-dependent gate is a flaky gate**: *Mitigation*: verdict production is fail-open — a backend timeout records no verdict and leaves the presence-only gate intact. It must never block a commit because an API was down.
- **Non-goal**: blocking on the continuous score. That flip is a separate, calibration-justified change.
- **Non-goal**: replacing CodeRabbit, Bugbot, or any deterministic gate. Operating rule 1: gate, do not trust.
- **Non-goal**: the `track` progress-curve and pivot-tournament use cases (see § Out of scope).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ changes.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario**: new `tests/bats/verifier_review_gate.bats` driving the real pipeline against `verifier-endpoint.py` (canned deterministic backend, no key, no network egress) — asserts: a `hard_veto` verdict blocks the commit and names the reason; a clean verdict passes; a sub-threshold score is reported but does **not** block (advisory posture); no configured backend records no verdict and leaves PR #1964 behaviour byte-identical; a backend timeout fails open. Extend `tests/bats/review_ack_gate.bats` with the 3-field marker round-trip plus 2-field and legacy bare-sha back-compat.
- **Selftests**: `bash agents/scripts/core/review-ack.sh --selftest`, `bash scripts/dev/pre-ship.sh --selftest`, `python3 scripts/dev/verifier-sidecar.py --selftest` all green; `agents/scripts/core/test-gate-selftests.sh` still passes (any new `--selftest` exposer carries the `# selftest: asserts-failure` marker).
- **Build gate**: N/A — no compiled artifact. (`cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` not exercised by this diff.)
- **Shell/lint**: `agents/scripts/core/test-shell-lint.sh` green; `shellcheck -S warning -x` clean on every touched script; `test-orphan-bats.sh` (new bats needs its `test-*.sh` wrapper) and `test-bats-ascii-names.sh` green.
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before implementation starts; record the outcome here.
- **Manual residue**: none. Calibration data accumulates from `verifier-produce.py --record` traces during normal use; no human step is required for this slice to be complete.

## Out of scope (flagged, not designed)

- **Flipping the continuous score to blocking** — requires a `verifier-calibrate.py` report clearing all thresholds on real traces + labels. Follow-up plan once enough traces exist; `--gate` is the CI job that would guard it.
- **`track` / progress curves for early-stop** — scoring a session's trajectory (the audit case: many files changed, no review dispatched) is a genuinely good fit but is a session-lifecycle feature, not a commit gate. Separate plan.
- **Pivot-tournament ranking of review findings** — `docs/agent-rules/verifier-sidecar.md` § Candidate selection lists "review findings to investigate first" as a use case; useful when a review returns a dozen findings, orthogonal to gating. Separate plan.
- **Scoring non-C++ diffs** — the substantive test inherits PR #1964's first-party-C++ scope. Widening it is a separate decision with its own cost profile.

## Implementation log
*(populated post-ship)*

## Deviations from plan

Both were surfaced by the pre-implementation stress-test (§ Verification, `grill-with-docs`), which resolved by running the tooling rather than reading its docs — and both changed the design.

1. **Only `hard_veto` blocks; `recommendation` does not.** The plan's § Approach said the gate would block on "`hard_veto` (or, post-calibration, a sub-threshold score)". Running `verifier-sidecar.py aggregate` shows `recommendation` is `block` for a **low score** as well as for a veto, and `escalate` for an ordinary single sample (a 0.82 score at 0.7 confidence already returns `escalate`). Blocking on `recommendation` would therefore either gate on an uncalibrated score — the exact thing rule 5 forbids — or wedge nearly every commit. The gate keys on `hard_veto` alone.

2. **The veto is self-reported, and that is accepted for this one signal.** The plan's § Approach asserted the verifier "must not be the reviewer". `verifier-produce.py` emits **criterion scores only — it never emits `hard_veto`** (confirmed: no occurrence in the file; the sidecar reads the flag from its input sample). So a veto can only originate from the reviewing agent's own verifier object. Rather than drop veto-blocking, the design adopts an **asymmetric trust rule**: a veto is a *confession* — an agent reporting a security hole in its own change argues against its own interest, which a high score never does — so self-reported **bad** news is trusted while self-reported **good** news is not. The continuous score still requires an independent backend to mean anything, which is why it stays advisory. Recorded in `docs/agent-rules/verifier-sidecar.md` § Where it runs first.

Also fixed in passing, a latent bug in the shipped #1964 code rather than a plan deviation: `ra_read_marker` ran `tr -d '[:space:]'` over everything after the mode field, which was correct for a 2-field record but would have folded the new verdict fields into the fingerprint — producing an ack that could never match, i.e. a gate that blocks forever. Now field-exact.

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip the § Status header to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
