# `pre-ship.sh` still registers untracked files from a line listing, which C-quotes non-ASCII names

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-10-06
- **Observed on**: the code-review pass over agent-layer PR alexandrosk0/the-unwilling-agentic-bunch#5.
- **Status**: open. Blocked on the agent-layer bump that brings in the layer's NUL-safe `ra_ita_untracked`.

## What happened

The layer's `ra_ita_untracked` (`agent-layer/agents/scripts/core/lib/review-ack.sh`) now:

- lists untracked files NUL-delimited;
- returns an error when the listing or `git add --intent-to-add` fails, so nothing fingerprints a tree view with new files missing.

The host `scripts/dev/pre-ship.sh` keeps its own copy, `preship_ita_untracked`, which still reads `git ls-files --others --exclude-standard` line by line. With the default `core.quotePath`, a non-ASCII untracked name reaches `git add` C-quoted. pre-ship then stops under `set -e`. That is a loud failure, not a wrong fingerprint, but it blocks a push for a legitimate file name.

The two copies must also list the same files, or pre-ship and `record-review-verdict.sh` fingerprint different tree views.

## Concrete next action

Once the agent-layer pointer includes the NUL-safe `ra_ita_untracked`:

- in `scripts/dev/pre-ship.sh`, replace `preship_ita_untracked` with a call to `ra_ita_untracked` from the sourced library, and stop if it returns an error;
- delete the host copy.

Bats coverage: an untracked file with a non-ASCII name is registered and fingerprinted, and the run stays green.
