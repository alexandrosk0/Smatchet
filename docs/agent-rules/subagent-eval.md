# Subagent eval (advisory)

Eval-driven development for the **development agents** (`agents/*.md` — the orchestrator + the delegated subagents), NOT the product under development or any in-app AI-assistant surface. Mirrors the perf pipeline one level up the stack: instead of frame latency, it scores agent **decision quality** base-vs-head so a prompt edit ships on data, not judgment.

Plan: [`../plans/shipped/subagent-eval-harness.md`](../plans/shipped/subagent-eval-harness.md). Phase-1 MVP — `code-review` only, **advisory**. The forward roadmap — judge calibration (WARN→BLOCK graduation), the trace flywheel (auto-grown golden set), and the three Snorkel eval-gap dimensions (output-complexity rubrics, autonomy-horizon trajectories, multi-agent / tool-fault environments) — is the unified [`../plans/subagent-eval-agentic-coverage.md`](../plans/subagent-eval-agentic-coverage.md) plan (which absorbs the former standalone trace-flywheel plan as its Phase 2).

## Pieces

| Piece | Path |
|---|---|
| Case schema (golden case shape) | [`../agent-eval/case-schema.json`](../agent-eval/case-schema.json) |
| Result schema (one runner output) | [`../agent-eval/result-schema.json`](../agent-eval/result-schema.json) |
| Scoring policy (thresholds) | [`../agent-eval/scoring-policy.json`](../agent-eval/scoring-policy.json) |
| Scorer (pure stdlib, base-vs-head) | `scripts/dev/agent-eval-score.py` |
| Runner (drives one case, N trials) | `scripts/dev/agent-eval-run.sh` |
| Curated cases (`code-review`, 3-5) | `tests/agent-eval/code-review/*.json` |
| Schema validator (stdlib, test-only) | `tests/agent-eval/validate_schema.py` |
| Tests | `tests/bats/agent_eval_score.bats`, `tests/bats/agent_eval_run.bats` |

## The advisory gate

CI / pre-push treats an eval delta as **advisory**:

- **Malformed eval artifact → FAIL.** A result JSON that doesn't conform to `result-schema.json`, a caseId mismatch between base and head, a missing dimension, or a judge that won't run is a hard error (scorer exit `2`). A broken harness must never read as "no regression".
- **Quality regression → WARN, not BLOCK.** A dimension dropping below base by more than the policy's `max_score_drop` (scorer exit `1`) surfaces a WARN with the delta table. It does **not** block the merge — yet. The auto-judge isn't calibrated against humans, so a single regression signal isn't trustworthy enough to gate on.
- **Clean → pass** (scorer exit `0`).

### Why advisory (the WARN→BLOCK graduation is deferred)

Blocking on a score requires trusting the judge. That trust is earned with **calibration data** — a corpus of judge-vs-human agreement on the same runs. Until that exists, a blocking gate would either rubber-stamp (thresholds loose) or cry wolf (thresholds tight). Graduation to BLOCK is gated on calibration and tracked as a deferred-automation item in [`../self-improvement/categories/tooling.md`](../self-improvement/categories/tooling.md). Do not wire a merge-gate BLOCK before then.

## Scoring two dimension kinds

Each case declares scored `dimensions` (see `case-schema.json`):

- **objective** — a deterministic inline code check on `trial.findings`, no LLM:
  - `cited_file_line` — fraction of findings that cite a concrete `file:line`.
  - `severity_enum` — fraction of findings whose severity is in the allowed enum.
  - `finding_count` — closeness of the finding count to `referenceOutcome.expectedFindingCount`.
- **judge** — graded by an **external** judge command, named by `--judge-cmd` (or its environment-variable fallback — the project env-prefix + `_AGENT_EVAL_JUDGE_CMD`; see `agent-eval-score.py --help` and the `AGENTS.md` pointer for the exact name). The scorer pipes `{case, output, dimension, referenceOutcome}` JSON to the command's stdin and reads `{"score": 0..1}` from its stdout. Keeping the judge external is what keeps `agent-eval-score.py` **pure stdlib + deterministic + unit-testable** — the bats suite injects a fake judge with no model in the loop.

Scores normalise to `[0,1]` (higher is better). The scorer averages each dimension across trials.

> **Windows note:** the judge is launched as a subprocess. Invoke it as `python <script>` (not `bash <script>`) so the native Python interpreter runs it directly — a bare `bash` on a Windows PATH resolves to WSL. Pass the script as a native `C:/…` path (`cygpath -m`), not an MSYS `/c/…` path.

## Before/after recipe (the two-worktree run)

A single checkout can't produce a delta — the runner's `--prompt-root=<path>` is the seam. Score a prompt PR by running the case set once against each side, then diffing:

```bash
# base = the develop-side agent prompts; head = the PR-side agent prompts
git worktree add ../eval-base origin/develop
git worktree add ../eval-head <pr-branch>

CASE=tests/agent-eval/code-review/cr-dpapi-secret-loss.json
BASE=$(bash scripts/dev/agent-eval-run.sh "$CASE" --prompt-root=../eval-base --trials=3)
HEAD=$(bash scripts/dev/agent-eval-run.sh "$CASE" --prompt-root=../eval-head --trials=3)

python scripts/dev/agent-eval-score.py "$BASE" "$HEAD" \
  --policy docs/agent-eval/scoring-policy.json \
  --judge-cmd "python scripts/dev/my-judge.py"
# exit 0 clean · 1 regression (WARN) · 2 malformed (FAIL)
```

`--fake-runner` swaps the live harness for a canned result (no tokens) — that's how CI exercises the contract deterministically.

## Adding a case

Drop a `tests/agent-eval/code-review/<caseId>.json` conforming to `case-schema.json` (the bats suite validates every case). Record enough to reconstruct the run: `repoRef`, `baseBranch`, `files`/`diff`, `toolPosture`, the exact `delegationPacket`, the scored `dimensions`, and the `referenceOutcome`. The current cases are **real and reproducible** — each is anchored to a historical commit (`repoRef` = the buggy parent of a real `fix(` commit), with `referenceOutcome` = the ground truth the fix established. Mine new ones the same way (`git log --grep='^fix(' --oneline`, diff the fix, take the parent as the buggy state); the flywheel will later auto-harvest them from live traces.

Coverage beyond `code-review` is added only after the MVP proves the gate catches a real prompt regression.
