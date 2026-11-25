# The plan CI-proof gate turns a failed commit-status read into a final "never ran" block

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-25
- **Observed on**: independent review of the agent-layer bump 75e1728 -> 4334c2b (the-unwilling-agentic-bunch#23, the plan CI-proof gate). Latent: nothing has fired yet.
- **Status**: open

## What happened

`_mg_plan_proof_verdict` in `agent-layer/agents/scripts/core/merge-gates.sh` looks up each proof check on the PR head in two steps:

1. It reads the check runs for that name. A failed read prints `UNDECIDED` and sets `undecided=1`, so the poll holds and retries.
2. When no check run of that name exists (`absent`), it falls back to the head's commit statuses (around line 607):

   ```bash
   contexts="$(gh api "repos/${owner}/${repo}/commits/${sha}/statuses?per_page=100" --paginate \
       --jq '.[].context' 2>/dev/null || true)"
   ```

   The `|| true` swallows a failure. A transient REST error leaves `contexts` empty and `concl` stays `absent`, which the gate treats as a skip.

So a REST blip in step 2 is reported as `BLOCK: plan CI-proof skipped …`, with the proof check listed as never having run. That is rc 1, which ends the poll at once. Step 1 handles the same kind of failure as a retry.

`merge-watcher.py` re-polls every cycle, so it recovers on the next poll. An orchestrator `poll_merge_gates` call halts immediately and names the wrong cause, which sends the session hunting for a skipped lane that does not exist.

## Concrete next action

Make step 2 match step 1: drop the `|| true`, and when the `statuses` read fails, print `UNDECIDED\t the commit statuses for "<check>" (<path>) could not be read`, set `undecided=1` and `continue`. Add a fixture to `agent-layer/tests/bats/merge_gates.bats` with a stubbed `gh` whose `check-runs` call returns an empty list and whose `statuses` call fails. Assert rc 2 (undecided), not rc 1.
