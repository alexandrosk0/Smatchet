# The review-proof "substantive diff" test is first-party-C++-only, so the gate scripts that enforce it can edit themselves without ever tripping it

- **Category**: process
- **Priority**: P2
- **Date**: 2026-09-14
- **Observed on**: adversarial-code-review of the fix for `record-review-verdict-n-a-with-no-review` (PR #2219 follow-up, not yet a PR number at filing time)
- **Status**: open

## What happened

Fixing the PR #2219 gap (`record-review-verdict.sh` accepted a hand-written
`n/a — <reason>` verdict with zero review tool ever invoked) required routing that script
through `ra_is_substantive` (`agents/scripts/core/lib/review-ack.sh`) — the same
strict-zone-or-line-threshold test `scripts/dev/pre-ship.sh --ack-review` already uses to
decide whether a diff needs a fingerprint-matching `.review-ack` + `.review-findings.json`
pair before a verdict can be recorded.

The independent adversarial review of that fix caught that `ra_is_substantive` only inspects
`RA_CPP_GLOBS` — `Source/Core/*.{cpp,h}`, `Source/Plugins/*.{cpp,h}`,
`Source/Standalone/*.{cpp,h}`, `tests/*.{cpp,h}`. Confirmed empirically by sourcing the lib
and running `ra_is_substantive branch origin/develop` against the fix's own diff (4 shell +
1 markdown file, 0 C++ lines): `NOT substantive: 0 C++ lines, no strict-zone touch`.

## Why it matters

A diff that touches ONLY `agents/scripts/core/**`, `scripts/dev/**`, or
`scripts/git-hooks/**` — i.e. the review-enforcement machinery itself — is therefore never
substantive under this rule, no matter how much gate logic it rewrites. That diff can always
carry a hand-written `n/a — <reason>` with zero proof artifact: the exact PR #2219 shape,
one layer up, attacking the gate instead of product code. This is not a regression the
PR #2219 fix introduced — item 1's `preship_require_findings` had the identical blind spot
before that fix existed, gated by the same `ra_is_substantive` — but it is the highest-leverage
place in the repo for this attack class to land unnoticed, and the fix that was supposed to
close "unreviewed verdict on a real diff" is itself an instance of the diff class it doesn't
cover.

## Concrete next action

Extend `ra_is_substantive`'s file scope (or add a second, OR'd trigger) to also trip on
changes under `agents/scripts/core/**`, `scripts/dev/**`, and `scripts/git-hooks/**` — the
repo's own enforcement surface. This is NOT a drop-in one-liner: `RA_CPP_GLOBS` also drives
`ra_fingerprint`'s hash and is shared by the PRE-COMMIT gate (`scripts/git-hooks/pre-commit`
staged mode via `agents/scripts/core/review-ack.sh`), so widening it needs the same WARN-first
calibration this repo already applies to `duplication` / `unused-symbol-under-config-guard`
(ADR-0015 precedent) before it becomes a hard block — a bare extension risks tripping on every
markdown-only doc PR that happens to touch a gate script's comment, or ratcheting the existing
`.review-ack` mechanism's blast radius without warning the sessions that rely on its current
C++-only scope.

1. Add a WARN-only advisory (mirrors `unused-symbol-under-config-guard`'s calibration phase)
   when a diff touches `agents/scripts/core/**` / `scripts/dev/**` / `scripts/git-hooks/**`
   with no matching `.review-ack` for `branch` mode, before making it block.
2. After a calibration window with no false-positive complaints, promote to a real
   `ra_is_substantive` trigger (new glob set, OR'd with the existing C++ test, both feeding
   the same `.review-findings.json` artifact contract) — same shape as the C++ path, not a
   parallel implementation.
3. Update `docs/agent-rules/ship-loops.md` § [pre-first-push gate] item 5 and this script's
   own header comment (both currently document the C++-only scope as a known limit) once
   closed.

**Enumerator**: `bash -c '. agents/scripts/core/lib/review-ack.sh && ra_is_substantive branch origin/develop'`
run against a diff touching only `agents/scripts/core/**` — today this always reports
"not substantive" regardless of how large or behavior-changing the script diff is.
