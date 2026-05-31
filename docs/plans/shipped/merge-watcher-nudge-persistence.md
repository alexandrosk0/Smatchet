# Plan — Persist cross-poll gate state (CR-nudge guard + STALE streak) across watcher poll cycles

> **Slug**: `merge-watcher-nudge-persistence` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.
>
> **Grill outcome (2026-05-30)**: approach B (env round-trip, nudge logic stays in `merge-gates.sh`); scope = persist the nudge guard **and** the STALE streak; `gh_fails` + timeout `start` audited harmless-by-design. **Double-check correction**: the counters live in the **registry** (mirror `_bump_cr_none_grace`), NOT `state/<pr>.json` — see Approach.

## Context

`smatchet-merge-watcher` spam-posts `@coderabbitai review` on a registered PR — observed **14+ comments at a steady ~84 s cadence** on [#567](https://github.com/alexandrosk0/Smatchet/pull/567) (17:21:10 → 17:39:19 UTC, 2026-05-30) before the PR was manually unregistered + merged.

Root cause class: `poll_merge_gates()` (`scripts/dev/merge-gates.sh`) carries **cross-poll state in bash locals** that only persist *within one invocation's* poll loop. The watcher invokes the poller with `MERGE_GATES_MAX_POLLS=1` **once per daemon cycle** (`scripts/dev/merge-watcher.py:379`) — a fresh process each time — so every such local re-initialises every cycle. Audit of the four cross-poll locals:

| Local (`merge-gates.sh`) | Behaviour under watcher `MAX_POLLS=1` | Disposition |
|---|---|---|
| `rereview_posted_head` (:204) — once-per-HEAD nudge guard | resets each cycle → `@coderabbitai review` re-fires every cycle | **FIX — persist** |
| `stale_streak` / `stale_head` (:199-200) — consecutive-STALE counter gating the STALE re-review nudge (:519-528) | never reaches `STALE_REREVIEW_POLLS` (default 5) → STALE re-review nudge **never fires** via the watcher | **FIX — persist** |
| `gh_fails` — transient-`gh`-failure retry counter | maxes at 1, never reaches 3 → `GH_API_DOWN` never raised; a `gh`-down cycle returns `BLOCKED` (safe self-correcting fallback) + the watcher has its own `try/except` at `merge-watcher.py:403` | leave (audited-harmless) |
| `start` — wall-clock base for `GATES_TIMEOUT` | single fast poll → never times out; by design the watcher owns total PR lifetime, the timeout guards only the in-session `MAX_POLLS=60` path | leave (audited-harmless) |

The nudge fires because CodeRabbit comments rather than approving, so GitHub `reviewDecision` stays `null` (NONE) and the CR-NONE early-nudge (`merge-gates.sh:594-612`) triggers on the first (only) blocking poll every cycle. The CR-NONE **grace** counter is already persisted watcher-side, so the PR still eventually merges — these are spam/cost + dead-feature defects, **not** merge-correctness defects.

**Proven precedent — this exact bug class is already solved once.** `maybe_pass_cr_none_grace` (`merge-watcher.py:1140`) fixed the identical `MAX_POLLS=1`-resets-an-in-process-counter problem for the CR-NONE *grace* counter; its docstring (`:1146-1157`) states it verbatim: *"poll_one runs merge-gates with MERGE_GATES_MAX_POLLS=1, so `p` is always 0 — the grace window can never elapse within a single invocation and resets every cycle. Fix: count consecutive grace-wait cycles per HEAD in the registry."* `_bump_cr_none_grace` (`:1122-1137`) persists `cr_none_grace_polls` + `cr_none_grace_head` to the **registry** (pinned to HEAD so a fresh push restarts the window). This plan extends that same registry-counter pattern to the nudge guard + STALE streak.

**Intended outcome (one sentence):** after this lands, across a PR's entire watcher lifetime CodeRabbit is nudged **at most once per head SHA** (re-arming only when the head advances), and the STALE re-review nudge fires correctly after `STALE_REREVIEW_POLLS` watcher cycles on a stuck STALE head.

## Approach

Round-trip the two surviving counters through the **registry** `entry` dict (the same store + read-before-poll / write-after pattern the CR-NONE grace fix already uses), keeping all nudge/streak *logic* in `merge-gates.sh` (single source of truth, shared with the in-session orchestrator's `MAX_POLLS=60` path):

1. **`merge-gates.sh`** — seed the three locals from new input env vars at declaration (`:199-204`):
   `rereview_posted_head="${MERGE_GATES_PRIOR_NUDGE_HEAD:-}"`, `stale_head="${MERGE_GATES_PRIOR_STALE_HEAD:-}"`, `stale_streak="${MERGE_GATES_PRIOR_STALE_STREAK:-0}"` (`set -u`-safe via `:-`). Emit one parseable stdout line carrying the final values immediately before the blocked `return 1` (`:692`): `GATE_CARRY nudge_head=<sha> stale_head=<sha> stale_streak=<n>`. Single emit point suffices: under `MAX_POLLS=1` a fired nudge always falls through to `return 1` (it only fires when `cr_pass=false`, which can't reach the `return 0` pass at `:677`), and the per-iteration `Poll …` line (`:628`) always precedes it so the watcher's `last_status_line` parse (`merge-watcher.py:414`) is unaffected. Seeding makes head-advance re-arm automatic (existing `:522-525` / `:623-626` reset logic).
2. **`merge-watcher.py`** — in `poll_one` (`:290`): read the prior `nudged_head` / `stale_head` / `stale_streak` from the registry `entry` via `entry.get(...)` (exactly like `entry.get("cr_none_grace_polls", 0)` at `:1192`), pass them as the three `MERGE_GATES_PRIOR_*` env vars through the existing `extra_gates_env` channel (`:399`), and parse the `GATE_CARRY` line out of `gates.stdout` into the returned state dict. In the daemon loop after `poll_one` (`:1754`, alongside the `maybe_pass_cr_none_grace` call at `:1760`): persist the parsed values to the registry via a new `_bump_nudge_state(pr, clone_path, nudged_head, stale_head, stale_streak)` — a near-clone of `_bump_cr_none_grace` (`:1122`) under `registry_lock()`.

Trade-off named (grill-confirmed): keeping the nudge POST in `merge-gates.sh` (vs moving it into the watcher) avoids forking nudge logic away from the in-session caller; the env round-trip is the cost.

## Files to modify

1. `scripts/dev/merge-gates.sh:199-204` — seed `stale_streak` / `stale_head` / `rereview_posted_head` from `MERGE_GATES_PRIOR_STALE_STREAK` / `_STALE_HEAD` / `_NUDGE_HEAD` (defaults `0`/`""`/`""`); document the three new input envs in the knob block (`:36-51`).
2. `scripts/dev/merge-gates.sh:692` — emit `printf 'GATE_CARRY nudge_head=%s stale_head=%s stale_streak=%s\n' …` immediately before `return 1`.
3. `scripts/dev/merge-watcher.py:378-400` (`poll_one` env build) — inject the three `MERGE_GATES_PRIOR_*` from `entry.get(...)` (alongside the existing `extra_gates_env` merge at `:399-400`).
4. `scripts/dev/merge-watcher.py:411-442` (`poll_one` output parse + returned state) — parse the `GATE_CARRY` line from `gates.stdout`; stash the three values into the returned dict (e.g. `nudge_carry`) for the daemon to persist (carry prior forward when the line is absent, e.g. on a non-poll early-return).
5. `scripts/dev/merge-watcher.py` — new `_bump_nudge_state(...)` mirroring `_bump_cr_none_grace` (`:1122-1137`); call it from the daemon loop after `poll_one` (`:1754-1760`) when `nudge_carry` is present.
6. `tests/bats/merge_gates.bats` (after the `:786-811` CR-NONE nudge block) — (a) two sequential `MAX_POLLS=1` invocations threading `GATE_CARRY`→`MERGE_GATES_PRIOR_NUDGE_HEAD` post **exactly one** `@coderabbitai review`; (b) a third with a changed head re-arms; (c) STALE streak seeded to `STALE_REREVIEW_POLLS-1` fires the STALE nudge on the next single poll; (d) the 20-field gate-filter stream still parses with `GATE_CARRY` present.
7. `tests/bats/merge_watcher.bats` (near the head-change-resets pattern at `:458`) — persisted registry `nudged_head` / `stale_streak` round-trip + suppress the second-cycle nudge; head advance re-arms (mirrors the `_bump_cr_none_grace` registry tests).

## Existing utilities reused

- `_bump_cr_none_grace` + `maybe_pass_cr_none_grace` — `scripts/dev/merge-watcher.py:1122-1159` — the exact precedent: a per-(PR,head) registry counter that solves the same `MAX_POLLS=1` reset bug; `_bump_nudge_state` is a near-clone.
- `registry_lock()` / `read_registry()` / `write_registry()` (`_CLI.*`) — `scripts/dev/merge-watcher.py:1130-1137` — the registry read-modify-write idiom `_bump_nudge_state` reuses.
- `entry.get("cr_none_grace_polls", 0)` read-before-poll pattern — `scripts/dev/merge-watcher.py:1192` — confirms registry `entry` is the right store for a read-before-poll counter (vs the post-poll `state/<pr>.json`).
- `extra_gates_env` per-invocation override channel — `scripts/dev/merge-watcher.py:399` — already passes `MERGE_GATES_CR_GRACE_POLLS`; reused for the three `MERGE_GATES_PRIOR_*`.
- `nudge_coderabbit()` + `rereview_posted_head` guard — `scripts/dev/merge-gates.sh:204,209,213-214` — guard set only on a *successful* `gh pr comment`, so the emitted `nudge_head` is accurate; only seed + emit change.
- `stale_streak` / `stale_head` STALE trigger + reset — `scripts/dev/merge-gates.sh:519-528,623-626` — unchanged logic; only seeded + emitted.
- `MERGE_GATES_STUB_COMMENT_COUNTER` bats harness + `Poll `-prefix stdout contract — `tests/bats/merge_gates.bats:786-811`, `merge-watcher.py:414` — existing nudge-assert harness to extend; confirms a distinct-prefix `GATE_CARRY` line is safe.

## UX Pillar callouts

Dev-process / CI tooling only — no product-runtime code, no `Source/Core/`. All four N/A.

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: N/A — bash/python CI tooling, never on the UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — no product code path.
- **Pillar 3 (never crash)**: N/A for the app; `_bump_nudge_state` reuses the `registry_lock` + `write_registry` envelope `_bump_cr_none_grace` already relies on.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

`N/A — diff touches scripts/dev/ + tests/bats/ only; no Source/Core/.` (Tooling diff, not pure-docs — skips the perf gate but still runs shell-lint + bats.)

## Risks / non-goals

- **RISK — `GATE_CARRY` parse collision**: a new stdout line could misalign the `Poll `-prefix parser. *Mitigation*: distinct `GATE_CARRY` prefix (not `Poll `, not a 20-field gate-filter line — that's a separate `gh --jq` call); the per-iteration `Poll` line always precedes it on the `return 1` path so `last_status_line` is unaffected. Bats asserts both parsers still work + the field stream still counts 20.
- **RISK — registry write contention**: `_bump_nudge_state` + `_bump_cr_none_grace` both mutate the same `entry` in a cycle. *Mitigation*: both take `registry_lock()` + read-modify-write the whole registry; they run sequentially in the daemon loop, not concurrently. Mirror the existing lock discipline exactly.
- **RISK — stale prior vs live head**: head advances between cycles. *Mitigation*: the guard/streak compare against the *live* `$head_sha` from the same poll's GraphQL; a stale prior simply doesn't match → reset → one nudge on the new head (self-correcting, desired).
- **RISK — empty prior on first cycle**: registry has no `nudged_head` yet → `entry.get` default `""`/`0` → first nudge fires once (correct); first STALE poll starts the streak at 1.
- **NON-GOAL — `gh_fails` + timeout `start` persistence**: audited harmless-by-design (table above); left per-invocation. Documented, not fixed.
- **NON-GOAL — change CR-NONE grace / merge semantics**: untouched; nudge-frequency + STALE-streak only.
- **NON-GOAL — fix CodeRabbit `reviewDecision=null`**: external; handled by the existing grace.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — no `Source/Core/` helper.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver / bats**:
  - `tests/bats/merge_gates.bats` — (a) two `MAX_POLLS=1` invocations threading `GATE_CARRY`→`MERGE_GATES_PRIOR_NUDGE_HEAD` post **exactly one** `@coderabbitai review` (spam regression); (b) third invocation with a new head re-arms; (c) STALE streak seeded to `STALE_REREVIEW_POLLS-1` fires the STALE nudge on the next single poll; (d) the 20-field gate-filter stream still parses with `GATE_CARRY` present.
  - `tests/bats/merge_watcher.bats` — persisted registry `nudged_head` / `stale_head` / `stale_streak` round-trip; second cycle passes the priors and suppresses the nudge; head advance re-arms.
  - `scripts/dev/test-shell-lint.sh` (5-rule) on the edited `merge-gates.sh`.
  - Whole suite via `scripts/dev/test-all.sh` (bats + shell-lint at the pre-push gate).
- **Build gate**: `N/A — no compile (scripts/ + tests/bats/ only; SmatchetStandalone / SmatchetCore_DX12 untouched).`
- **Manual residue**: none — the spam reproduction + dead STALE nudge are now bats assertions.

## Out of scope (flagged, not designed)

- **`gh_fails` / timeout `start` cross-cycle persistence** — audited harmless-by-design; no-action.
- **Triage-attempt counter** (`merge-watcher.py:871`) — already registry-persisted; unaffected.
- **CodeRabbit approve-vs-comment behaviour** — external; no-action.

## Implementation log

- `fd395b9a` · plan (registry-counter pattern).
- `a52f92eb` · `merge-gates.sh` seed 3 locals from `MERGE_GATES_PRIOR_*` + emit `GATE_CARRY` before `return 1`; `merge-watcher.py` `_parse_gate_carry` + env-seed in `poll_one` + `_bump_nudge_state` (clone of `_bump_cr_none_grace`) + daemon-loop persist; 6 bats tests.

## Deviations from plan

- **Store corrected (registry, not `state/<pr>.json`)** — the original draft named `state/<pr>.json` + `notify_dispatched_for_state` as the precedent. The double-check found the daemon reads `entry` from `read_registry()` *before* the poll, and the CR-NONE grace counter (the *identical* `MAX_POLLS=1` bug) is persisted in the **registry** via `_bump_cr_none_grace`. `notify_dispatched_for_state` is a post-poll result flag — wrong precedent. Switched to the registry-counter pattern (no behavioural difference, correct store).
- **Single emit point, no `RETURN` trap** — under `MAX_POLLS=1` a fired nudge always exits via `return 1` (it only fires when `cr_pass=false`, which can't reach the `return 0` pass) and the per-iteration `Poll` line always precedes it, so one `printf` before `return 1` suffices; the trap was unnecessary.

## Verification (actual)

- **merge_gates.bats** — 4 new (`nudge guard survives MAX_POLLS=1 … exactly one post`, `re-arms on stale head`, `STALE streak fires at threshold`, `GATE_CARRY doesn't disturb the Poll status line`) + 93 existing = **all PASS, 0 regressions**.
- **merge_watcher.bats** — 2 new (`_parse_gate_carry`, `_bump_nudge_state` registry round-trip) + full suite **72 PASS, 0 not-ok**.
- `bash -n scripts/dev/merge-gates.sh` + `python -m py_compile scripts/dev/merge-watcher.py` clean. `shellcheck` not installed locally (CI runs the shell-lint gate).
- **Build gate**: N/A — no compile (scripts/ + tests/bats/ only).
