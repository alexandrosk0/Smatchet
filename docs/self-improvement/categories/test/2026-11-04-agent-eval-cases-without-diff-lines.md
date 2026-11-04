# Three of the four code-review eval cases carry a diff with no `+`/`-` lines, so "introduced by this diff" is undecidable

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the code-review eval of the pre-#18 prompt against the thinned one (the-unwilling-agentic-bunch#18 → Smatchet#2385).
- **Status**: open

## What happened

The `diff` field of `tests/agent-eval/code-review/cr-dpapi-secret-loss.json`, `cr-env-cred-routing.json` and `cr-recentlru-false-positive.json` holds only context lines: zero added or removed lines. Only `cr-reads-as-empty-user-catalog.json` has a real diff (50 changed lines). The review method tells the agent to report bugs the change introduces and to drop pre-existing ones, so on a context-only diff a correct reviewer may legitimately demote the seeded defect. One trial did exactly that on dpapi: it found the erase-before-encrypt bug and filed it as info, "pre-existing, not introduced by this diff". The grade then depends on how the agent reads an ill-formed case, not on review quality.

## Concrete next action

Regenerate each of the three diffs from the commit that introduced its defect (`git show <commit> -- <file>`, the commit named in the case's `description`), so the seeded lines appear as `+` lines. Then add a check to `tests/agent-eval/validate_schema.py` that a case whose `referenceOutcome` expects findings has at least one `+`/`-` line in its diff. A no-defect case such as recentlru keeps a real diff too, since the false-positive bait should be in changed code.
