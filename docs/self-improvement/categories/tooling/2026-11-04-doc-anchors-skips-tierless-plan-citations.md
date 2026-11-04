# `test_doc_anchors.py` skips tier-less `docs/plans/<slug>.md § X` citations, so the section name in a plan cross-reference is never checked

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-04
- **Observed on**: the DX12 readback plan's `docs/plans/dx12-standalone-win-arm64.md § …` citations (Smatchet#2389), where a wrong section name sat unchecked through several review rounds.
- **Status**: open

## What happened

`agent-layer/docs/agent-rules/process-rules.md` § Archive on revision requires plan cross-references in the tier-less form `docs/plans/<slug>.md`, so a plan can move between active, shipped and deferred without a reference sweep. `test-markdown-links.sh` resolves that form to whichever tier holds the file (`plan_tierless_target`). `test_doc_anchors.py` does not: a tier-less path names no existing file, so the citation is counted under "Skipped (path names no existing file)" (48 in Smatchet today), and its section name is never checked. A wrong section therefore passes, and so does a dangling plan path.

## Concrete next action

In the layer, teach `cited_files` in `agents/scripts/core/test_doc_anchors.py` the same tier resolution as `test-markdown-links.sh` (`docs/plans/<slug>.md` → the first of `active/`, `shipped/`, `deferred/` that holds `<slug>.md`), with bats cases for a good and a broken tier-less citation. Then add a WARN, delta-scoped to changed lines, for a citation whose path resolves to no file at all, so a typo in the path is reported instead of skipped.
