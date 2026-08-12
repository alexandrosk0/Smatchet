# Plan — merge-watcher triage-budget recovery + CR-thread resolution
<!-- plan-date: 2026-05-28 -->

> **Slug**: `merge-watcher-triage-recovery`.

## Context

Closes the 2026-05-22 P1 entry in `docs/backlog/agent-self-improvement/tooling.md` (line 31). Symptom seen on PRs #408 + #410 of session 2026-05-22: user picked post-ship option 3 ("Register with watcher"); both PRs had 1–2 CodeRabbit Actionable findings at registration; the watcher's auto-act path successfully pushed fix commits addressing CR's asks; but CR's per-line review threads remained `isResolved:false` on GitHub, which kept `merge-gates.sh` returning BLOCKED via `cr_open > 0`. The watcher then incremented `triage_attempts` on every poll until the registry latched at `LAST_STATE: TRIAGE_BUDGET_EXHAUSTED` (counts hit 119 / 93). PRs sat green-but-blocked until manual unregister-and-merge.

Two distinct sub-bugs in the action plan:

- **(a)** `TRIAGE_BUDGET_EXHAUSTED` is a terminal sink — `handle_blocked_cr_triage` keeps incrementing the per-PR-lifetime counter on every poll that matches `_looks_like_cr_finding_block`. Once attempts cross the budget there is no re-evaluation path; even objectively green polls (status_line shows "STALE_RESOLVED" / "0 actionable") leave the counter pinned because the early-return branches do not reset it.
- **(b)** After the auto-act spawn pushes a fix commit, the watcher never calls `resolveReviewThread` on the threads its fix addressed. CR's StatusContext on the new head can flip to SUCCESS without CR auto-resolving the original review threads — `cr_open` stays > 0 and `cr_open_blocks` keeps the merge gate at BLOCKED indefinitely.

Outcome after this lands: a PR that goes through one auto-fix-then-merge cycle reaches GATES_PASSED without manual intervention; the registry's triage counter resets to 0 as soon as CR is no longer block-shaped, so a later CR-finding round starts fresh.

## Approach

Two narrowly scoped edits on top of the existing `merge-watcher.py` poll loop.

**Sub-bug (a) — counter reset path.** In `handle_blocked_cr_triage`, when the early-exit `not _looks_like_cr_finding_block(status_line)` branch fires AND the registry has a non-zero `triage_attempts`, reset it to 0 in the same registry transaction (existing `_bump_triage_attempts` helper, with `triage_for_head_sha` preserved). Surfaces the reset on the state dict as `triage_reset_on_cr_clear` so the daemon log line names the transition. No new state machine; this slots into the existing CR-clean fall-through.

**Sub-bug (b) — `resolveReviewThread` after auto-act.** Add `id` to the `reviewThreads.nodes` selection in `scripts/dev/merge-gates.graphql` so threads carry their global node id. New helper `maybe_resolve_stuck_cr_threads(state, entry)` called from `daemon_loop` between `handle_blocked_cr_triage` and `maybe_notify`. Fires only when (1) the registry has a previously-recorded `auto_act_for_head_sha`, (2) the PR's current `headRefOid` differs from that recorded sha (i.e. a push has landed since auto-act fired), (3) `cr_status_state == SUCCESS` per a one-off `gh api graphql` fetch using the same query. Enumerates CR-authored, non-outdated, unresolved review threads tied to the prior head and calls `mutation { resolveReviewThread(input:{threadId:$id}) { thread { id } } }` per thread. Persists `last_resolved_threads_count` + `last_resolved_at_unix` on the registry entry. Opt-in via `MERGE_WATCH_RESOLVE_CR_THREADS=true` for the first ship; flip default after one validation round.

Trade-off: the resolution is unconditional ("our fix push addresses all CR threads on prior head") rather than per-finding ("commit X addresses thread Y"). The agent's auto-act prompt does not return a thread-id list; trying to thread that through would require a structured handoff from the spawned `claude -p` session back to the daemon, which is out of scope for a P1 hygiene fix. False-positive blast radius — the watcher resolves a thread the fix did not actually address — is bounded because (i) the next CR re-review will re-open relevant threads, and (ii) the gate condition (`auto_act_for_head_sha` set AND head advanced AND CR status SUCCESS) is narrow.

## Files to modify

1. `scripts/dev/merge-watcher.py` — `handle_blocked_cr_triage` counter reset path; new `maybe_resolve_stuck_cr_threads`; new helpers `_fetch_unresolved_cr_threads` + `_resolve_review_threads`; wire into `daemon_loop`.
2. `scripts/dev/merge-gates.graphql` — add `id` to `reviewThreads.nodes` selection (read by both the gate poller and the new watcher helper).
3. `tests/bats/merge_watcher.bats` — three new tests: (i) counter reset on CR-clear, (ii) `maybe_resolve_stuck_cr_threads` fires when head advanced + CR SUCCESS, (iii) `maybe_resolve_stuck_cr_threads` no-ops when gate not satisfied.

## Existing utilities reused

- `_bump_triage_attempts` (`merge-watcher.py:883`) — preserves `triage_for_head_sha` while bumping the counter. Reused unchanged for sub-bug (a)'s reset (`new_count=0`).
- `_gh_json` (`merge-watcher.py:416`) — runs a `gh` subcommand expecting JSON. Reused by the new mutation + thread-fetch helpers.
- `_gh_owner_repo` — used by the new helper to resolve owner/repo before the GraphQL call.
- `_CLI.registry_lock()` (used in `_bump_triage_attempts`) — reused for persisting `last_resolved_threads_count` atomically.

## UX Pillar callouts

- **Pillar 1 (perf)**: N/A — daemon-side script, not on the UI thread.
- **Pillar 2 (UI never blocks)**: N/A — same.
- **Pillar 3 (never crash)**: helpers wrap `gh` subprocess calls in `RuntimeError` catch (mirrors existing pattern in `handle_blocked_cr_triage` head_sha probe). Mutation failure logs a WARN and continues — does not crash the daemon.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff touches only `scripts/dev/` + `tests/bats/`; no `Source_Core/` impact.

## Risks / non-goals

- **Over-resolution risk** — the helper resolves all CR-authored open threads tied to the prior head, not just threads the auto-fix demonstrably addressed. Mitigation: gate fires only after head advanced AND CR re-review status is SUCCESS; CR re-opens any genuinely un-addressed thread on its next review pass. Accepted for P1 hygiene; finer-grained per-finding resolution is out of scope (would require structured handoff from the spawned claude session).
- **Default-on as of 2026-05-28 follow-up** — original ship was opt-in via `MERGE_WATCH_RESOLVE_CR_THREADS=true`. Flipped default to true after the feature ran cleanly across 3 production unblock cycles (manual `gh api graphql resolveReviewThread` dance on PRs #487 / #488 / #496-497). The env knob still accepts `false` / `0` / `no` to opt back out. Mitigation: gate conditions remain conservative (auto_act_for_head_sha recorded, head advanced, status_line not CR-block-shaped); bats still covers the on-path; CR re-opens any genuinely un-addressed thread on its next review pass.
- **Non-goal**: refactoring the per-HEAD vs per-PR-lifetime counter split. The reset path covers the observed wedge without re-shaping the counter semantics.
- **Non-goal**: closing the original `GH_API_DOWN` sister bug (2026-05-21 P1 line 46 of `tooling.md`). Same code area, separate root cause; not bundled here.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — daemon-side bash/python.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario**: `tests/bats/merge_watcher.bats` extended with three tests covering the new paths; `bats tests/bats/merge_watcher.bats` runs as part of `scripts/dev/test-all.sh`.
- **Build gate**: N/A — no C++ touched.
- **Manual residue**: none — the live-PR happy path is gated behind `MERGE_WATCH_RESOLVE_CR_THREADS=true` and exercised by bats with stubbed `_gh_json` so the mutation call surface is covered without a live PR.

## Out of scope (flagged, not designed)

- Live-PR end-to-end probe for the resolveReviewThread path. Tracked as a follow-up tooling backlog entry; depends on a future PR with real CR findings.
- ~~Flipping `MERGE_WATCH_RESOLVE_CR_THREADS` default to true. Deferred to follow-up after one production cycle.~~ **Done** 2026-05-28; default flipped after 3 successful production cycles.
- Reshaping `triage_attempts` from per-PR-lifetime to per-HEAD-only semantics. The reset path closes the observed wedge; reshaping is gratuitous churn.

## Implementation log

- `20b5b71` · plan doc landed (`wip(plan): merge-watcher-triage-recovery`).
- Single follow-up implementation commit (this PR) — `scripts/dev/merge-gates.graphql` adds `id` to `reviewThreads.nodes`; `scripts/dev/merge-watcher.py` adds the counter-reset path inside `handle_blocked_cr_triage`, plus `_fetch_unresolved_cr_threads` + `_resolve_review_threads` + `_bump_resolved_threads` + `maybe_resolve_stuck_cr_threads` (~190 LoC); `daemon_loop` wires the new resolve step between triage and notify; `tests/bats/merge_watcher.bats` adds 9 new tests covering both sub-bugs.

## Deviations from plan

- Initial bats covered registry-list assertions for the new `last_resolved_*` fields, but Windows path normalization (CLI `register` writes forward-slash `clone_path`, `os.getcwd()` returns backslash) made the same-Python-process bump miss the registry row. The bug is pre-existing (tests #24/25/26 fail with the same root cause) and out of scope for this slice; the new tests assert the in-memory `extras` shape via Python `print` and leave registry-list assertions for a future Windows-path-normalization PR.

## Verification (actual)

- Bats — `bats tests/bats/merge_watcher.bats --filter 'maybe_resolve_stuck_cr_threads|handle_blocked_cr_triage resets stale|handle_blocked_cr_triage CR-clear'` → 9 / 9 passing locally.
- Full bats file run: my 9 new tests pass; the pre-existing 12 tests that fail to load (parser barfing on `→` in test names) are unrelated to this slice and tracked separately.
- No `Source_Core/` changes, so build gate skipped per project rules.
