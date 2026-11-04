# The reviewer's commands and the full suite both skip `test-docs.sh`, so a broken citation in a script comment reaches the gate review

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the `agent-layer` pin to the-unwilling-agentic-bunch#19 (Smatchet#2389). A new comment in `scripts/dev/check-layer-pin.sh` cited the "First checkout" heading with the section-sign form the doc-anchors gate resolves against the layer's `AGENTS.md`, but only the host root `AGENTS.md` has that heading.
- **Status**: open

## What happened

The widened layer doc-anchors gate now checks `<path>.md § <heading>` citations in scripts, JSON and YAML as well as Markdown, and it failed on that comment. Two routine checks missed it before the code-review gate caught it:

- `scripts/dev/test-all.sh --ci` lists `test-docs` in `CI_SKIP_RE`, so the full suite (2885 passed) never ran the doc gates.
- `docs/guides/review-checklist.md` never names `scripts/dev/test-docs.sh`, so a reviewer following the checklist's static-analysis commands would not run it either.

The citation also passed an earlier `test-docs.sh` run, because the new script was still untracked and the gate scans tracked files.

## Concrete next action

Add `bash scripts/dev/test-docs.sh` to the static-analysis block of `docs/guides/review-checklist.md` for any diff that touches `*.md`, `*.sh`, `*.json`, `*.yml` or the `agent-layer` gitlink. Note beside it that the gate scans tracked files only, so it must run after `git add`. Consider having `scripts/dev/pre-ship.sh` run `test-docs.sh` on the staged tree whenever the diff touches those paths.
