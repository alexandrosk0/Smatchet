# Plan — P4-gated ship-loop

> **Slug**: `p4-gated-ship-loop`

## Context

When `SMATCHET_AGENT_VCS=p4`, agents currently follow the same autonomous ship-loop as git: diagnose → fix → build → commit → push → PR. A PR is opened before the user has reviewed the change and before full tests are confirmed clean.

Goal: when Perforce is enabled, iterate exclusively in p4, pause for user review of the shelved change, run full tests, fix failures, then open a PR — git/GitHub touched once, at the end, when the change is known-good. After this lands, `SMATCHET_AGENT_VCS=p4` sessions always present a shelf for human review before any git push or PR creation.

## Approach

Add a **P4-gated ship-loop variant** documented in two places:

1. **`AGENTS.md` § Autonomous ship-loop default** — one-paragraph branch that fires when `SMATCHET_AGENT_VCS=p4`, naming the 4-phase loop and cross-linking to `docs/perforce/AGENT_FLOWS.md § P4-gated ship-loop`.
2. **`docs/perforce/AGENT_FLOWS.md`** — new `## P4-gated ship-loop` section with full phase sequence, invariants, and exception rules.

The existing git ship-loop is unchanged. Pure-docs change.

### Small vs multi-slice: stream selection

| Situation | p4 stream | Promote-to-PR |
|---|---|---|
| **Small change** — single slice, ≤ ~5 files, no parallel agents | `//smatchet/main` client directly | `git add -A && git commit && git push && gh pr create` |
| **Multi-slice / complex** — multiple slices, many files, or parallel subagents | task stream via `scripts/dev/p4-task-stream.sh <id>` | `scripts/dev/p4-task-stream-to-pr.sh <id> "<title>"` |

"Small" threshold: orchestrator judgment. When in doubt (scope unclear, >1 subsystem, parallel agents planned) → use task stream.

### Loop sequence — small change (single slice, main stream)

```
[p4 iterate — on //smatchet/main]
  edit → p4 submit to //smatchet/main
  repeat until complete

[p4 shelf for review]
  p4 shelve -c <pending-CL>
  AskUserQuestion: "Shelf <CL> ready — review in P4V and confirm."
  → rejected: iterate back
  → approved: continue

[full tests]
  bash scripts/dev/doctor.sh                                              # toolchain pre-flight
  cmake --build --preset ninja-test-msys2                                 # doctest rig build
  ctest --output-on-failure --test-dir build/ninja-test-msys2            # doctest rig run
  cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone    # main build
  cmake --build --preset ninja-iter-msys2 -DSMATCHET_WITH_WHISPER=OFF    # sentinel: no-whisper
  cmake --build --preset ninja-iter-msys2 -DSMATCHET_WITH_AGENTIC=OFF    # sentinel: no-agentic
  bash scripts/dev/lint-flush.sh                                          # clang-format + cppcheck + clang-tidy
  bash scripts/dev/coverage-delta-gate.sh   # if Source_Core/src/*.cpp touched
  bash scripts/dev/test-doc-anchors.sh      # if AGENTS.md or agents/** touched
  bash scripts/dev/test-agent-contract.sh   # if AGENTS.md or agents/** touched
  bash scripts/dev/test-all.sh              # scenario/integration/bash-driver tests
  # perf gate — conditional on diff hitting the scenario map (agents/perf-gatekeeper.md)
  # uses $SMATCHET_PERF_HOST env var for per-machine baseline selection
  bash scripts/dev/perf-run.sh <scenario>
  python scripts/dev/perf-compare.py docs/perf/baselines/<scenario>.$SMATCHET_PERF_HOST.json \
      build/perf-runs/<scenario>-<ts>.json
  # if SMATCHET_PERF_HOST unset or no baseline for this host → MISSING_BASELINE, skip
  (bucket-E if visual paths touched)
  → failure: fix in p4 → re-test (NO re-review)
  → pass: continue

[promote to PR]
  git add -A && git commit && git push -u origin <branch>
  gh pr create --draft
  post-ship AskUserQuestion (existing 4-option protocol)
```

### Loop sequence — multi-slice (task stream)

```
For each slice (repeat until all slices done):
  [p4 iterate — on task stream]
    edit → p4 submit to //smatchet/task-<id>/...
    repeat within slice until complete

  [inter-slice auto-gate — automatic, NO user pause]
    bash scripts/dev/doctor.sh                                              # toolchain pre-flight
    cmake --build --preset ninja-test-msys2                                 # doctest rig build
    ctest --output-on-failure --test-dir build/ninja-test-msys2            # doctest rig run
    cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone    # main build
    cmake --build --preset ninja-iter-msys2 -DSMATCHET_WITH_WHISPER=OFF    # sentinel: no-whisper
    cmake --build --preset ninja-iter-msys2 -DSMATCHET_WITH_AGENTIC=OFF    # sentinel: no-agentic
    bash scripts/dev/lint-flush.sh                                          # clang-format + cppcheck + clang-tidy
    bash scripts/dev/coverage-delta-gate.sh   # if Source_Core/src/*.cpp touched
    bash scripts/dev/test-doc-anchors.sh      # if AGENTS.md or agents/** touched
    bash scripts/dev/test-agent-contract.sh   # if AGENTS.md or agents/** touched
    bash scripts/dev/test-all.sh              # scenario/integration/bash-driver tests
    # perf gate — conditional on diff hitting the scenario map
    bash scripts/dev/perf-run.sh <scenario>
    python scripts/dev/perf-compare.py docs/perf/baselines/<scenario>.$SMATCHET_PERF_HOST.json \
        build/perf-runs/<scenario>-<ts>.json
    # if SMATCHET_PERF_HOST unset or no baseline for this host → MISSING_BASELINE, skip
    code-review agent (cumulative task-stream diff)
    → issues: fix autonomously → re-build → re-lint → re-test → re-review → repeat until clean
    → clean: continue to next slice (or proceed to end-gate if last slice)

[end-gate — after ALL slices pass inter-slice gates]
  [p4 shelf for user validation]
    p4 shelve -c <pending-CL>
    code-review agent (full cumulative diff — final pass)
    AskUserQuestion: "All slices done, tests green. Shelf <CL> ready — review in P4V and confirm."
    → feedback: fix in p4 → re-shelve → re-present
    → approved: continue

  [promote to PR]
    bash scripts/dev/p4-task-stream-to-pr.sh <agent-id> "<title>"
    post-ship AskUserQuestion (existing 4-option protocol)
```

**Per-machine perf baseline setup** (one-time per machine):
- Set `SMATCHET_PERF_HOST=<name>` in the shell profile on each machine (e.g. `desktop`, `laptop`).
- Bootstrap: `bash scripts/dev/perf-baseline.sh init <scenario> --host=$SMATCHET_PERF_HOST` for each affected scenario.
- Baselines land at `docs/perf/baselines/<scenario>.<host>.json` — committed, both machines coexist in the same repo.
- If `SMATCHET_PERF_HOST` unset or no baseline file for this host → gate logs `MISSING_BASELINE` and skips (non-blocking).

Key invariants (both loops):
- `git push` / `gh pr create` happen **once**, after user approval AND full test-pass.
- Review gate fires **exactly once**. Test failures → fix → re-test without re-review. Re-review only on explicit user request.
- Small-change submits land on `//smatchet/main` directly — no task stream overhead.
- Multi-slice task-stream submits never touch `//smatchet/main` until `p4-task-stream-to-pr.sh` integrates them.

## Files to modify

1. `AGENTS.md` — add P4-gated branch paragraph to § Autonomous ship-loop default; cross-link to `docs/perforce/AGENT_FLOWS.md § P4-gated ship-loop`.
2. `docs/perforce/AGENT_FLOWS.md` — add `## P4-gated ship-loop` section; cross-link back to `AGENTS.md § Autonomous ship-loop default`.

## Existing utilities reused

- `scripts/dev/p4-task-stream.sh` — allocates task stream + client (AGENT_FLOWS.md § Task-stream lifecycle).
- `scripts/dev/p4-task-stream-to-pr.sh` — integrates task stream → main → git push → gh pr create; the promote-to-PR step.
- `scripts/dev/test-all.sh` — full test gate at the post-approval step.
- `scripts/dev/lock-claim.sh` / `lock-release.sh` — plan-lock mechanics; referenced in doc, no changes.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — pure-docs change, no C++ touched.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — pure-docs change.
- **Pillar 3 (never crash)**: N/A — pure-docs change.
- **Pillar 4 (accessibility)**: N/A — pure-docs change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff is strictly `AGENTS.md` + `docs/**` (pure-docs allow-list). `bash scripts/dev/is-pure-docs-diff.sh develop` exits 0.

1. **PR-fast CI**: N/A — no `Source_Core/` diff.
2. **Pillar 2 static scanner**: N/A — no new sync-I/O.
3. **Dispatcher drain**: N/A — no `MainThreadDispatcher` touch.
4. **Visible-cue bucket-E harness**: N/A — no new stall path.
5. **Marker inventory**: N/A — no `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- **Risk**: all p4-mode tasks gate on user review before PR — even trivial one-liner fixes. *Mitigation*: doc notes the user can confirm the shelf immediately; the gate is the pause, not the P4V review duration.
- **Non-goal**: git ship-loop unchanged when `SMATCHET_AGENT_VCS` is unset or `git`.
- **Non-goal**: no programmatic enforcement (no script blocking `gh pr create` in p4-mode). Rule is doc-level; agents read the doc.

## Verification

- **Bucket A**: N/A — no pure-logic C++ to unit-test.
- **Bucket E**: N/A — no ImGui widget change.
- **Bash-driver**: N/A — no new script.
- **Build gate**: N/A — pure-docs; `bash scripts/dev/is-pure-docs-diff.sh develop` exits 0, skip build + test-all.
- **Manual residue**: read both edited files, confirm new sections present + internally consistent + cross-linked. No automated equivalent needed for doc edits.

## Out of scope (flagged, not designed)

- **Script-level enforcement** — a pre-`gh pr create` guard that checks `SMATCHET_AGENT_VCS=p4` and blocks without a shelf approval record. Follow-up if doc-level rule proves insufficient.
- **Incremental shelf review** — multiple review checkpoints during iteration, not just at completion. Follow-up if one-gate model proves insufficient.
- **P4V diff integration** — automating a diff view launch from the shelf step. User opens P4V manually.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result)*
