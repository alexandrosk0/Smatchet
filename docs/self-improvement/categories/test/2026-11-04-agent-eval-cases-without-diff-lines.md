# Three of the four code-review eval cases carry a diff with no `+`/`-` lines, so "introduced by this diff" is undecidable

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the code-review eval of the pre-#18 prompt against the thinned one (the-unwilling-agentic-bunch#18 → Smatchet#2385).
- **Status**: open

## What happened

The `diff` field of `tests/agent-eval/code-review/cr-dpapi-secret-loss.json`, `cr-env-cred-routing.json` and `cr-recentlru-false-positive.json` holds only context lines: zero added or removed lines. Only `cr-reads-as-empty-user-catalog.json` has a real diff (50 changed lines). The review method tells the agent to report bugs the change introduces and to drop pre-existing ones, so on a context-only diff a correct reviewer may legitimately demote the seeded defect. One trial did exactly that on dpapi: it found the erase-before-encrypt bug and filed it as info, "pre-existing, not introduced by this diff". The grade then depends on how the agent reads an ill-formed case, not on review quality.

## Concrete next action

Regenerate each of the three diffs so the seeded lines appear as `+` lines. The commit each case's `description` names is the fix, not the introducer, so `git show` of it prints the seeded lines as `-` lines. For dpapi and env-cred use the introducing commit `cd66e28c` (#357; `git show cd66e28c -- Source_Core/src/ConfigManager.cpp`) or the reverse of the fix (`git diff c76256de c76256de^ -- Source_Core/src/ConfigManager.cpp`); for recentlru use the reverse of the cleanup (`git diff 42aecd27 42aecd27^ -- <file>`). Then add a check to `tests/agent-eval/validate_schema.py` that a case whose `referenceOutcome` expects findings has at least one `+`/`-` line in its diff. A no-defect case such as recentlru keeps a real diff too, since the false-positive bait should be in changed code.
