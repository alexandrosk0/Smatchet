- 2026-08-16 · orchestrator · [tooling] · P2 — the `(head=<12-hex>)` the Intent gate checks is transcribed BY HAND into the PR body while `record-review-verdict.sh` already prints the exact line; one mistyped hex red-flagged the gate on PR #2023, and every re-push needs the same manual re-transcription
  Details: `record-review-verdict.sh` writes the local marker AND echoes the
    canonical line `adversarial-code-review: <verdict> (head=<12-hex>)`. The PR
    body must carry that same line for the Intent-section check
    (`check-pr-intent.sh` matches `(head=<12-hex>)` against the PR head). Nothing
    connects the two: the orchestrator reads the script's output and retypes it
    into the body. On PR #2023 I predicted the hex before the commit existed and
    wrote `498f7ad2b3f4` when the real head was `498f7ad2c1f3` — the gate caught
    it correctly, but it cost a red check, a diagnosis detour, and a body edit.
    The same PR then needed the line re-transcribed on SIX subsequent pushes
    (rounds 1-6), each an opportunity for the same typo. Related cost: every one
    of those body edits re-sent the ENTIRE body through the PR-update tool
    (~8 KB), when a `curl -X PATCH` with `$GITHUB_TOKEN` (present in the remote
    environment) does it far more cheaply — worth codifying alongside.
  Concrete next action: teach `record-review-verdict.sh` an opt-in
    `--sync-pr <n>` (or a sibling `sync-verdict-to-pr.sh`) that, after writing
    the local marker, PATCHes the PR body: replace the existing
    `^adversarial-code-review: .*$` line with the freshly generated one, or
    append it when absent. The script already owns the canonical string and the
    head SHA, so the transcription step — and its typo class — disappears. Guard
    it: no-op with a clear message when `$GITHUB_TOKEN` is unset or `--sync-pr`
    is omitted (local-only runs must keep working), and never invent a PR
    number. Bats-testable against a stub API endpoint. Est ~0.5d.
  Status: open
  Last-reviewed: 2026-08-16
