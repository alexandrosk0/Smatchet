- 2026-08-11 · claude-code · [tooling] · P1 — `test-gate-selftests --check` proves a `# selftest: asserts-failure` marker EXISTS, not that the negative under it can ever fail; `test-plan-index.sh`'s negative had been satisfied by a `Permission denied` for its whole life, and four more vacuous negatives were written in the two sessions that touched this area

  Details: [`test-gate-selftests.sh`](../../../../agents/scripts/core/test-gate-selftests.sh)
  enforces that every `--selftest`-exposing gate script carries a negative
  assertion, marked `# selftest: asserts-failure`. That is the right gate to
  have — it closed a real gap. But what it can check is the presence of a marker
  and some negative-looking code near it. It cannot check the property that
  matters: **that the negative actually fails when the behaviour it names is
  removed.**

  **The live instance.** [`test-plan-index.sh`](../../../../agents/scripts/core/test-plan-index.sh)
  case (3) fed a non-existent archive dir and required a non-zero exit. Two
  independent reasons it could not fail:

  1. It invoked `"$_SCRIPT_PATH" --check` — executing the script directly. The
     file is mode **100644** in git (`git ls-files -s` confirms), so that exec
     fails with **126 Permission denied** on every machine, including CI. The
     assertion was satisfied by the permission error, never reaching the
     archive-dir guard at all.
  2. Even via `bash`, it accepted ANY non-zero status. Deleting the guard leaves
     the script exiting non-zero on an unhandled `os.listdir` traceback — so the
     assertion stayed green with the behaviour it names entirely removed.

  Verified both ways: with the archive-dir guard deleted, the selftest reported
  PASS before the fix and FAIL after it.

  **This is a class, not an instance, and the evidence is uncomfortable.** In the
  same two sessions, *four more* vacuous negatives were written — by the session
  fixing this very family of bugs:

  - `archive-backlog-entry.sh`'s first negative assertions (three of them) each
    required only a non-zero exit; all three still passed with their own guard
    deleted, because the script dies downstream for an unrelated reason.
  - The `plan-date` marker selftest wrapped its assertions in `( set -e … ) || {…}`.
    `set -e` is **suppressed inside a subshell that is the left operand of `||`**
    — the shell disables it for any command in a `&&`/`||` list — so every step
    ran regardless of failure and only the last command's status was reported. It
    passed with the fix disabled.

  Five vacuous negatives, none of which a reviewer or a marker-checking gate
  caught. Every one was found the same way: **delete the code under the assertion
  and re-run.** That is the only check that distinguishes a test from a comment.

  Two recurring mechanical causes worth naming, because both are invisible on
  inspection:

  - **Accepting a bare non-zero status.** Any sufficiently broken program exits
    non-zero. A negative must assert the *reason* — the refusal message, the
    specific exit code — or it is satisfied by crashes, missing interpreters,
    permission errors and typos in the test itself.
  - **`set -e` inside a `&&`/`||` operand.** Already documented in
    [`script-freshness.sh`](../../../../agents/scripts/core/lib/script-freshness.sh)
    for the callee side; the *test* side has the same trap and no note anywhere.

  Concrete next action, cheapest first:

  1. **Make `test-gate-selftests --check` reject self-exec.** A `--selftest` that
     re-invokes its own script must do it via `bash "$path"`, never `"$path"`,
     since every gate script in this repo is mode 100644. This is a grep, it is
     exact, and it would have caught the live instance. Check the other 77
     scripts for the same shape while adding it.
  2. **Require negatives to assert a reason.** Flag an `asserts-failure` block
     whose only assertion is a bare status test (`if ! cmd; then`/`|| fail=1`)
     with no message/exit-code comparison anywhere in the block. Necessarily
     heuristic, so WARN rather than block, and cite this entry in the message.
  3. **Do NOT try to prove reachability mechanically.** Confirming a negative can
     fail means mutating the subject and re-running — that is mutation testing,
     and building it here would cost far more than it returns. The durable fix is
     the authoring rule (delete the code, re-run, watch it go red), which belongs
     in the review checklist rather than in a gate. A gate that pretended to
     verify reachability would itself be the false green this entry is about.

  Status: open
  Last-reviewed: 2026-08-11
