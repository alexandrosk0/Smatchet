- 2026-08-06 · orchestrator · [process] · P2 — `postmortem-owed.sh`'s `cr-out-of-band` de-noise drops a **load-bearing** override whenever the diff has no `Source/Core/src/*.cpp`: it assumes the label only ever waives an *advisory CR verdict*, but the label is also the only exit from a **wedged required CR gate**, and that class ships invisible to the nudge
  Details: PR #1948 (the font-asset worktree fallback) merged 2026-08-05 carrying `cr-out-of-band`.
    The label was strictly load-bearing: `CR findings (0 actionable)` — a **required** StatusContext —
    was stuck PENDING because CR's last on-head review was body-less, so
    `.github/actions/cr-finding-gate/action.yml` `decide()` could not terminate (root cause filed as
    [`process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md`](../applied.md)).
    Every other check was green; re-running the workflow re-posted PENDING. The label's early-exit was
    the only way to clear `mergeStateStatus=BLOCKED`.
    `bash agents/scripts/core/postmortem-owed.sh --list` nonetheless reports "no gate escapes owed":
    `core_scoped_only_trigger()` (:156-163) returns 0 — drop — for the trigger string
    `override: cr-out-of-band`, and the guard that would keep it, `pr_touches_core_cpp()` (:167-170),
    is false because #1948 changed only `CMakeLists.txt`, `cmake/SmatchetFontAssets.cmake`,
    `Source/Standalone/CMakeLists.txt`, a bats file, a wrapper script and a README.
    The de-noise rationale (:149-155) is sound for its intended class — "cr-out-of-band only waives the
    (advisory) CodeRabbit review, so on a non-Core diff the escape is a false positive". It does not
    hold for the wedge class: there the label dismisses a **required, non-terminal gate**, and the diff
    scope is irrelevant to whether that mattered. Note the two holes point opposite ways in the same
    CR path — this one hides an override that WAS load-bearing; the sibling tooling entry
    [`tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](../tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md)
    manufactures override use where none is needed.
  Concrete next action: make the drop conditional on the CR gate having actually *ruled*, not on diff
    scope. In `postmortem-owed.sh`, before `core_scoped_only_trigger()` drops a `cr-out-of-band`
    trigger, consult `gate_conclusion "$pr" 'CR findings'` (the helper already exists, :187-202):
      - context SUCCESS on its own → the label dismissed nothing → drop (today's behaviour, now
        justified by evidence rather than by diff scope);
      - context PENDING / non-SUCCESS / absent at merge → the label was load-bearing → **keep**, owes a
        postmortem, regardless of Core-cpp scope.
    This reuses the same "moot vs load-bearing" test the script already applies to
    `tests-out-of-band` / `perf-out-of-band` / `coverage-out-of-band` / `intent-out-of-band` in
    `override_is_moot()` (:212-232) — `cr-out-of-band` is the one override currently exempted from that
    test (:227-230 routes it to the scope heuristic instead). Folding it in removes the special case.
    Caveat to encode: the live rollup is re-run-lossy and override labels are stripped post-merge, so
    prefer the snapshot path where available and fall back to the live query with the existing
    documented lossiness note. Add a `--selftest` case for each of the two arms.
    Est ~0.5d (one helper call + branch + 2 selftest cases + comment rewrite at :149-155).
  Cross-ref: `agents/scripts/core/postmortem-owed.sh` (:149-163 `core_scoped_only_trigger`,
    :165-170 `pr_touches_core_cpp`, :187-202 `gate_conclusion`, :204-232 `override_is_moot`);
    PR #1948 (`2602340e`, the escape this hid); `docs/self-improvement/postmortems.md`
    (2026-08-06 · PR #1948 entry).
