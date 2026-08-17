# test-gate-selftests raw-self-exec negatives are vacuous on Windows (MSYS `#!` → `-x` true)

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-16
- **Found during**: shipping `github-issue-body-empty-line` (a UI display fix that touches no shell gate)

## Symptom

`bash scripts/dev/test-docs.sh` reports `test-gate-selftests` FAILED on Windows, on a
tree whose copy of `agents/scripts/core/test-gate-selftests.sh` is byte-identical to
`origin/develop`. The gate's own `--selftest` mode prints six paired failures:

```
test-gate-selftests selftest: FAIL — raw self-exec behind env -- was NOT flagged
test-gate-selftests selftest: FAIL — env -- exposer rejected for the wrong reason:
    test-gate-selftests: PASS — all 1 --selftest-exposing scripts assert a failure case.
```

(same shape for `env -i`, tab-separated, spaced-redirection, `&>`-redirection,
braced-expansion and `$( )`-capture fixtures). `--check` mode alone passes, so the
failure is invisible unless the suite runs `--selftest`.

## Cause

The raw-self-exec rule deliberately applies only to **non-executable** scripts (on a
mode-100755 file the raw form execs fine, so the 126-Permission-denied premise does not
hold). It reads the mode from the git index, falling back to the filesystem bit for
**untracked** files — and the `--selftest` synth fixtures are untracked, so they take
that fallback:

```sh
*) if [ -x "$f" ]; then _nonexec=0; else _nonexec=1; fi ;;
```

MSYS / Git-Bash has no real exec bit and reports any file whose first bytes are `#!` as
executable. Every synth fixture is written starting `#!/usr/bin/env bash`, so `[ -x ]` is
true, `_nonexec=0`, and the rule is skipped — the negatives can never fire. Reproduced
directly in this shell: a two-line `#!`-headed file reports `-x` true; the same file
without the shebang reports false. The `chmod -x "$synth"` the fixture sequence performs
does not help, since the heuristic ignores the mode.

Production impact is limited to the gate's own negatives: **tracked** files take their
mode from the git index, so the `--check` path that polices real first-party scripts is
correct on Windows. The damage is that a Windows agent sees `test-docs.sh` red on every
branch, which trains the reflex this repo least wants — treating a red gate as background
noise.

## Proposed fix

Make the untracked fallback independent of the MSYS heuristic. Cheapest correct option:
have the `--selftest` harness classify its own fixtures explicitly (e.g. `git -C "$tmp"
init` + `git add` the fixture so the index-mode branch is exercised, which is also closer
to what the rule actually polices), or gate the fs-bit fallback on
`git config core.fileMode` being true and treat untracked files as non-executable
otherwise. Whichever is chosen, add a negative that fails on a platform where `[ -x ]` is
unreliable, so the vacuity cannot come back.

## Why it matters

This is a `--selftest`-asserts-a-failure-case gate — its entire job is proving its
negatives are reachable — and on one supported platform six of them are unreachable for
the same class of reason the rule itself was written to catch (a negative satisfied by
something other than the behaviour it names).
