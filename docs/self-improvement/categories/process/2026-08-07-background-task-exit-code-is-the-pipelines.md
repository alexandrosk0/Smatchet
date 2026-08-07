- 2026-08-07 · claude-code · [process] · P2 — a background-task completion notification's "exit code 0" is the **pipeline's** exit, not the gate's; always read the in-file `*_EXIT=` value

  Hit twice this session. A gate run in the background as

  ```bash
  bash agents/scripts/project/test-lint-rules.sh --diff origin/develop 2>&1 | tee out.txt
  ```

  completes with a notification reading `exit code 0` **regardless of the gate's verdict**,
  because the reported status is the pipeline's, and the pipeline ends in `tee`. The same
  trap bites the interactive form: `echo "$?"` after a pipe reports the **last** element's
  exit — use `${PIPESTATUS[0]}`.

  The concrete near-miss: two wrong bucket-E invocations
  (`--target SmatchetUiTests` → `ninja: error: unknown target`, and
  `bash scripts/dev/test-ui.sh` → `No such file or directory`) both surfaced as exit 0 and
  were nearly recorded as passing runs. They were only caught by reading the output file,
  which contained the errors in plain text.

  Two fixes, both cheap:

  1. **Convention** — every gate wrapper this repo runs in the background must end with an
     explicit `echo "<NAME>_EXIT=${PIPESTATUS[0]}"` appended to the same output file, and
     the reader must grep for that token rather than trusting the notification. This is an
     ad-hoc habit today, not a repo convention — agents invent a token per run (`LINT_EXIT=`,
     `DOCS_EXIT=`, `FMT=`, `BUILD_*_DONE`) and no gate wrapper in `scripts/` emits a verdict
     token into its own output. (`scripts/dev/local/test-build-wrapper.sh:186` sets
     `SMATCHET_TEST_WITHMSVC_EXIT=` — an env var handed to a subprocess, not a verdict written
     where a reader will look for it.) Naming the convention and writing it down is the whole
     proposal.
  2. **Doc** — add the rule to [`process-rules.md`](../../../agent-rules/process-rules.md)
     § Cadence and verification, next to the existing note that `tail -N` can truncate a
     gate's verdict off the head of its output.

  Generalised: **a notification is a liveness signal, not a verdict.** Any claim that a
  gate passed must cite a line from the gate's own output.
