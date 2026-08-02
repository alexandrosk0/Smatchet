# Plan — PC-side verification & completion of the agentic-audit-review follow-ups

> **Slug**: `pc-verify-agentic-audit-followups` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all three tasks complete. Tasks A and C were satisfied by CI when PR #1812 merged (2026-07-13). Task B (the `merge-gates.sh` split) landed on a `windows-dev` machine with `bats`: `merge-gates.sh` (1506 → 1249 lines) split into a thin entry point + two sourced modules under `agents/scripts/core/merge-gates.d/` (`00-common.sh` = the block-on-any-red allow-list constant + prompt-shim + `gh_pr_ready_idempotent`; `10-gate-filter.sh` = the 31-field GATE_FILTER jq projection), with `tests/bats/merge_gates.bats` held 164/164 green at each extraction step and byte-identical behaviour proven. See § Verification (actual).

## Context

PR #1812 (branch `claude/agentic-infra-audit-review-lv70nj`) finished the safely-completable follow-ups from [`AGENTIC_INFRA_AUDIT.md`](../../audits/AGENTIC_INFRA_AUDIT.md): it fixed the `agent-too-long` gate's prose self-suppression (A1), trimmed `AGENTS.md` under cap, and shipped the MCP live-HTTP `Authorize` integration test (C6). Two things could **not** be done in a fresh Linux container and are the reason this plan exists — the container has no `bats`, no `gh`, no `shellcheck`, and cannot build the bucket-E (ImGui Test Engine) rig or the full Windows toolchain (finding **C3**, environment parity). After this lands, the C6 test is proven to compile+pass, the C2 monolith-split remainder is either done-and-bats-green or explicitly re-deferred with evidence, and the whole PR is confirmed against the real gate suite.

**One sentence**: run the validations a `windows-dev`-tier machine can run that the audit session could not, and finish the one refactor whose safety net (164 bats tests) is unrunnable in-container.

## Approach

Three ordered tasks, smallest-risk first. **Task A** (verify the already-written C6 test) and **Task C** (full local gate sweep of the PR) are pure verification — no new code, they either pass or surface a real defect to fix. **Task B** (the C2 `merge-gates.sh` split) is the only one that writes code, and it is a behaviour-preserving refactor gated entirely on `tests/bats/merge_gates.bats` staying green at each extraction step — which is exactly why it was left for a machine that can run bats. Do A and C first so the PR is provably green before adding B's risk; land B as its own follow-up PR (one extraction per PR, per the C2 backlog entry).

## Files to modify

This plan is **verification + one deferred refactor**; it modifies no files itself. The work it drives:

1. **Task A** — no file changes; runs `tests/ui/mcp_live_http_auth.test.cpp` (already in the branch) via `scripts/dev/test-ui-mcp-live-http-auth.sh`.
2. **Task B** — `agents/scripts/core/merge-gates.sh` (1506 lines → thin entry point + gate-condition modules under a new `agents/scripts/core/merge-gates.d/`, mirroring the shipped `lint-rules.d/` split), verified by `tests/bats/merge_gates.bats`.
3. **Task C** — no file changes; runs the real gate suite.

## Existing utilities reused

- `scripts/dev/test-ui-mcp-live-http-auth.sh` — the C6 bucket-E driver added in PR #1812 (zero-match fail-closed; auto-discovered by `test-all.sh`).
- `agents/scripts/project/lint-rules.d/` — the **proven** entry-point + sourced-module pattern (`test-lint-rules.sh`'s 2026-07-08 split) that Task B copies for `merge-gates.sh`.
- `tests/bats/merge_gates.bats` — the 164-case suite that is Task B's entire correctness net.

## Extraction sizing (Task B only)

`merge-gates.sh` is 1506 lines with no `--selftest`. Target after the split: a ~400–500-line entry point (CLI, the one `gh api graphql` call, condition dispatch, the unchanged block-on-any-red contract) + one sourced module per gate condition (CI / CodeRabbit / user-comments / Bugbot) under `merge-gates.d/`, each fail-closed on the explicit load list. The win is measured at the **source** (the poller drops under a reviewable size); the modules are the sink. Confirm each extraction leaves `merge_gates.bats` byte-for-byte green **before** the next — an extraction that reds a single case is the wrong seam.

## UX Pillar callouts

- **Pillar 1 (perf)**: N/A — harness scripts + a test-only TU; no product runtime path.
- **Pillar 2 (UI-thread)**: N/A — same.
- **Pillar 3 (never crash)**: N/A — the C6 test *asserts* the MCP auth gate's fail-closed behaviour but adds no product code.
- **Pillar 4 (accessibility)**: N/A.

## Perf-review-system gates

**N/A — the diff touches no `Source/Core/` runtime code** (a test-only bucket-E TU + its registration, plus harness scripts). No scenario mapping, no Pillar-2 sync-I/O, no dispatcher/​cue changes.

## Risks / non-goals

- **Risk — the C6 test reveals a real Authorize divergence** (e.g. a route registered before the auth gate, or a header the pure helper handles differently over a live socket). *Mitigation*: that is the test doing its job (finding C6's whole premise) — fix the plumbing, not the test.
- **Risk — Task B's split changes poller behaviour subtly** (a sourced module resolving a path relative to the wrong dir, as nearly happened in the lint-rules split). *Mitigation*: bats green at each step is mandatory; modules resolve relative to the script, matching `lint-rules.d/`.
- **Non-goal — the `build-and-test.yml` reviewability gap** (the third leg of finding C2). Out of scope here; stays in the C2 backlog entry.
- **Non-goal — re-running the audit.** This plan only closes the PC-blocked residue of PR #1812.

## Verification

Per `AGENTS.md` § Verification automation. Run from a Windows dev machine (or any host with the full toolchain + `bats`).

- **Task A — C6 bucket-E (finding C6, the reason this plan exists)**:
  ```
  cmake --preset ninja-ui-test-msvc
  cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone
  SMATCHET_EXE=build/ninja-ui-test-msvc/Smatchet.exe \
    bash scripts/dev/test-ui-mcp-live-http-auth.sh
  ```
  Expect: `Passed: 1  Failed: 0` for `McpLiveHttp/Authorize_RealSocket` (200 valid token · 401 no/wrong token · 403 DNS-rebind Host · 403 cross-origin Origin · 503 past the SSE cap). A `passed=0 failed=0` result means the filter matched nothing — the driver fails closed on that; investigate the registration under `SMATCHET_WITH_MCP`.
- **Bucket A (pure-logic ctest)**: `cmake --build --preset ninja-test-msvc && ctest --test-dir build/ninja-test-msvc --output-on-failure` — confirms the existing `tests/Plugins/Mcp/*` doctests still pass alongside the new live test.
- **`agent_size.bats` (the A1 gate fix)**: `bats tests/bats/agent_size.bats` — expect green, including the new "STILL FAILS when the deviation token appears only in backtick prose" case. This is the one the container could only replay by hand.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — the C6 TU compiles under `SMATCHET_WITH_MCP`).
- **Doc validation**: `bash scripts/dev/test-docs.sh` green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — including *this* plan file). Already green in-container for the runnable subset; re-confirm the full set.
- **Full sidecar**: `bash scripts/dev/test-all.sh` — picks up the new C6 driver automatically.
- **Task B**: after each extraction, `bats tests/bats/merge_gates.bats` (164 cases) byte-for-byte green; diff `merge-gates.sh --help`/all modes' output before vs after to prove behaviour preservation.
- **Plan stress-test — `grill-with-docs`**: this plan is a verification checklist, not a design; stress-tested against the audit's C3/C6/C2 findings — terms and task boundaries hold.
- **Manual residue**: none — every step above is a script/gate invocation. Task B, if re-deferred, updates the C2 backlog entry with the bats evidence (no silent residue).

## Out of scope (flagged, not designed)

**Deferral residue-sweep** — grepped `docs/self-improvement/categories/`, `AGENTIC_INFRA_AUDIT.md`, and the C2/C6 backlog entries: the only open cross-references are the C6 entry (marked applied-code-complete, points here for the run) and the C2 entry (merge-gates half open, points here for Task B). Both are current.

- **`build-and-test.yml` monolith split** — the third C2 leg; tracked in the C2 backlog entry, not attempted here.
- **A `--selftest` mode for `merge-gates.sh`** — worth adding *with* Task B's split (the lint scanner got one), but not required by this plan; note it in the C2 entry if Task B lands.

## Implementation log
- `b105507` (PR #1812, 2026-07-13) · A1 gate fix + C6 test shipped and merged; C6 CI-verified on the bucket-E lane, closing Task A ahead of any dev-machine run.

## Deviations from plan
- **Task A did not need a dev machine after all.** The plan assumed the bucket-E C6 test could only be verified on a `windows-dev` tier, but the repo's CI *has* bucket-E Mesa-GL lanes (Windows runners); the merge exercised `McpLiveHttp/Authorize_RealSocket` there and it passed. The dev-machine steps under § Verification Task A remain valid as a *local* re-run recipe but are no longer the only path — CI is authoritative.
- **The plan itself proved finding C3 the hard way.** Because the authoring Linux container couldn't build the bucket-E rig, CI caught two MSVC `/W4 /WX` warnings it was blind to (`C2446` on `res != nullptr`; `C4456` on the `IM_CHECK`-macro `res` shadow), each a fix + CI round-trip — recorded in the C6 backlog entry as concrete C3 evidence.

## Verification (actual)
- **Task A (C6) — DONE, CI-verified.** `Bucket-E UI tests (Mesa headless GL)` green on PR #1812 (commits `1547763`, `e79190f`); all six assertions pass over a real `httplib` socket. Post-merge re-check on `develop`: the TU carries the `httpRes` / `res.error()` fixes, is registered + enrolled, and the driver `bash -n`-parses.
- **Task C (gate sweep) — DONE.** Runnable-locally subset green on merged `develop` (agent-size `--selftest`, `AGENTS.md` no longer self-suppresses, delta gate exit 0, doc-anchors, agent-contract 27/0, markdown-links); the non-container subset (bucket-E, ctest/Coverage, dual-target MSVC, `agent_size.bats`) green on the merge CI.
- **Task B (merge-gates split) — DONE, bats-verified.** `agents/scripts/core/merge-gates.sh` split from 1506 → 1249 lines into a thin entry point + two fail-closed sourced modules under a new `agents/scripts/core/merge-gates.d/` (explicit load list, script-relative via `BASH_SOURCE`/`SCRIPT_DIR`, mirroring `lint-rules.d/`):
  - `00-common.sh` (85 lines) — the block-on-any-red allow-list constant `MERGE_GATES_BLOCK_ALLOWLIST_RE`, the `merge-gates-prompt.sh` lazy-source, and the standalone `gh_pr_ready_idempotent` (all formerly top-level). Consumers that `source merge-gates.sh` (`safe-admin-merge.sh` 19/19, `safe-merge.sh` 13/13, `postmortem-owed.sh`, `pr-status-watch.sh`) still see both symbols transitively.
  - `10-gate-filter.sh` (234 lines) — the one giant `gh api graphql --jq` GATE_FILTER (31-field projection) relocated VERBATIM into a single-quoted `_MG_GATE_FILTER_TEMPLATE` global the entry point copies byte-for-byte (no command-substitution newline trim); the three placeholders splice in the entry point exactly as before. `diff` of the inner content confirmed byte-identical.
  - **bats: 164/164 green** after each extraction step (baseline 164/164 confirmed first). **Behaviour preservation**: every CLI mode (SKIP bypass, missing-ORCH_USER, freshness typo, bad-poll-int, freshness block/warn with injected blobs) diffed byte-identical stdout+stderr+exit vs the pristine `origin/develop` copy — the only delta is the script's own path/line-number in the `set -u` "owner required" message (an artifact of the two files living at different paths, not a behaviour change). The `#1428` freshness self-check stayed in the entry-point function so `hash-object "${BASH_SOURCE[0]}"` still resolves to `merge-gates.sh` (proven identical).
  - **Coupled gate fixed same-PR**: `test-oob-label-impl.sh` greps `merge-gates.sh` for the `$failing → $downgraded` jq region + `any(. == "<label>")` label bindings, both of which moved into `10-gate-filter.sh`. Extended `_impl_files` + `_mg_ci_downgrade_labels` to fall back to the sibling module (self-contained parity fixtures still read their inline block); its 9-case bats selftest + the real-tree parity case pass.
  - **Seam that resisted (reported, not forced)**: the four gate-condition verdicts (CI / CodeRabbit / Bugbot / user-comments) are ONE indivisible stateful poll loop — they share ~15 per-poll locals (`cr_pass`, `cr_open_blocks`, `cr_overridden`, the `stale_streak`/`none_streak` cross-iteration counters, the `nudge_coderabbit` closure) plus function-local `return` codes; extracting them into eval-functions would thread that state across a boundary (the exact subtle-behaviour risk the contract forbids). Left inline. The C2 `--selftest` bonus was NOT added (the entry point stays `source`-able; no top-level `exit`).

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip the § Status header to `shipped` — DONE,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md` — DONE (this PR),
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix` — DONE.
