- 2026-07-05 · claude-code · [tooling] · P3 — pre-push clang-format check is whole-file, not delta; pre-existing drift in a touched file blocks an unrelated change

  Details: `scripts/git-hooks/pre-push` step 3 runs `clang-format --dry-run --Werror
  "$ci_f"` over each **changed first-party C++ file as a whole**. The CI lint gate
  (`Windows + MSVC` clang-format step) is **delta-based** (flags only NEW violations
  vs origin/develop, grandfathering pre-existing drift), so the local hook is
  STRICTER than the gate it claims to mirror. Observed this session on the
  `perf-win-hunt` one-line change to `SmatchetAiAssistantUi.cpp`: my edit was
  clang-format-clean, but a PRE-EXISTING drift at line 1036 (an over-long
  `EnqueueAppendAndTrim` call from an earlier commit) tripped the whole-file
  `--Werror` and refused the push. The remedy (`clang-format -i` the file) then
  reformats a line I never touched, adding unrelated churn to the diff — or forces
  the `SMATCHET_SKIP_PRESHIP_GATE=1` override for a legitimately-clean change.

  Impact: low-frequency friction, but it (a) makes the hook disagree with CI (the
  parity the hook exists to provide — `docs/agent-rules/ci-local-parity.md`), and
  (b) nudges toward either scope-creep (reformatting untouched lines) or the
  sanctioned-but-noisy skip override.

  Concrete next action: make the pre-push clang-format check delta-aware to match
  the CI gate — e.g. `git clang-format --diff <merge-base>` (formats/checks only the
  changed hunks) instead of `clang-format --dry-run --Werror <whole-file>`. If a
  whole-file check is intentional (catch latent drift early), then it should
  *offer* to reformat only the changed hunks, and its message should say "whole-file
  (stricter than CI delta)" so the operator isn't surprised the hook rejects a
  CI-green change. Home: `scripts/git-hooks/pre-push` step 3.
