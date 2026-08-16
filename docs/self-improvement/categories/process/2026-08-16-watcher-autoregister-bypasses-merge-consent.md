# The auto-register hook silently grants auto-merge authorization to every PR the agent opens

- **Date**: 2026-08-16
- **Author**: orchestrator
- **Category**: process
- **Priority**: P1

## What

`gh pr create` auto-registers the new PR with `smatchet-merge-watcher`, and mere
registration is documented as *authorization to auto-merge*. The two facts compose
into a consent bypass: **every** agent-opened PR is auto-merge-authorized at creation
time, before the user has been asked anything.

The two halves:

1. **`docs/harness/claude-code/hooks/autoregister-pr.sh`** — a `PostToolUse(Bash)`
   hook wired at `docs/harness/claude-code/settings.json.tmpl:145` and `:161`, copied
   to `.claude/hooks/` by `setup-harness.sh claude-code`. On any Bash command
   containing `gh pr create` it greps `pull/<N>` out of the payload and runs
   `python agents/scripts/core/merge-watcher-cli.py register <N>`. Unconditional —
   no authorization check, no user prompt, no opt-in flag. Its header states the
   intent plainly: closing the "shipped a PR but forgot to register it" gap.

2. **`docs/agent-rules/merge-gates.md:84`** — "Auto-`gh pr ready` + merge apply only
   when the user has explicitly authorised this PR for merge (post-ship option 3
   'Register with watcher', in-session 'merge when green', **OR any PR registered
   with `smatchet-merge-watcher`**)."

That third clause is what converts the hook's bookkeeping into consent. Result: the
post-ship 4-option `AskUserQuestion` (AGENTS.md § Autonomous ship-loop) is decorative
with respect to auto-merge — picking option 1, 2, or 4 cannot withhold it, because
the hook already registered the PR minutes earlier.

**Observed live, 2026-08-16, PR #2027.** The user answered the post-ship question with
"Wait and report gates" and stated "No merge without your go-ahead." `merge-watch list`
nonetheless showed `{"pr": 2027, "registered_at": 1786850965, ...}`. Nothing merged
without consent only because the entry was parked at `stuck_reason: "REVIEW_REQUIRED"`,
`stuck_streak: 11` — an incidental branch-protection state, not the consent boundary.
Once every check went terminal-green and `mergeStateStatus` flipped to `CLEAN`, the
next daemon cycle had no remaining reason to hold. The orchestrator ran
`merge-watch unregister 2027` by hand to restore the user's stated intent.

**Reproduced by the PR that files this entry.** Opening the backlog PR fired the hook
again — `merge-watch list` showed `{"pr": 2031, "registered_at": 1786895461, ...}`
seconds after `gh pr create`, with no authorization asked for or given, and the user
had authorized no merge. Unregistered by hand a second time. Two for two: the bypass
is the default path, not an edge case.

## Why it matters

This inverts the autonomy model. [`AI_POLICY.md`](../../../../AI_POLICY.md) makes agent
autonomy a **granted and revocable mode**, and AGENTS.md § Merge gates states
"**Auto-merge applies only when explicitly authorised**". Here authorization is
granted by a side effect of the agent's own tool call — the agent effectively
self-authorizes, and the user's answer to the one question that exists to gate it has
no mechanical effect.

It also contradicts the watcher's own shipped design decision,
[`smatchet-merge-watcher.md:102`](../../../plans/shipped/smatchet-merge-watcher.md)
("Explicit owner transfer on register … Clean ownership boundary; no race between
orchestrator + watcher + user"). Ownership transfer was specified as an explicit user
act; the hook made it automatic and silent while the docs kept describing it as
explicit.

The failure is quiet by construction. Registration emits only a `systemMessage`
inside a hook result, so nothing in the post-ship summary tells the user that a
background daemon now holds merge rights on their PR. The user learns only if they
run `merge-watch list` — or after the merge lands.

Same family as the 2026-06-19 "Intent gate bypassed via non-poller merge" entry (since
applied — see [`applied.md`](../applied.md)): enforcement that lives on the merge-actor
side is defeated by a path that never consults it. There the bypass was the merge
*mechanism*; here it is the merge *authorization*.

## Concrete next action

Split registration from authorization — keep the hook, drop its power. The hook's
observability value is real (gate-polling and the stuck-nudge caught wedged PRs
#2024 and #2027); the defect is that registration silently implies merge rights.

1. **Registry gains `authorized` (bool, default `false`).** Hook-driven registration
   writes `false`. Explicit user authorization — post-ship option 3, in-session
   "merge when green" — writes `true`.
2. **`merge-watcher.py` merges only on `authorized: true`.** An unauthorized entry
   still polls gates, still nudges on stuck/stale, still escalates — it just never
   calls the merge REST endpoint or `gh pr merge --auto`. Preserves everything the
   hook was written for.
3. **Add `merge-watch authorize <pr>` / `deauthorize <pr>`** verbs; make `register`
   take `--authorized` for the explicit-consent path. Have `authorize` print the
   ownership-transfer notice that `smatchet-merge-watcher.md:102` already specifies.
4. **Fix `docs/agent-rules/merge-gates.md:84`** — replace "OR any PR registered with
   `smatchet-merge-watcher`" with "OR a PR whose registry entry has
   `authorized: true`". As written today the clause is the actual bug in doc form.
5. **bats coverage** (sibling of `test-merge-watcher-bats.sh` /
   `test-merge-watcher-integration-bats.sh`): a hook-registered entry with **all gates
   PASS** must not be merged; `authorize` must flip it to mergeable; `register` +
   `--authorized` must be equivalent to `register` then `authorize`.
6. **Surface it in the post-ship summary.** If the PR is registered but unauthorized,
   say so in the ship report — a background daemon holding a PR should never be
   invisible state.

Est ~0.5d. Rejected alternative: delete `autoregister-pr.sh`. That trades one real
gap (forgotten registration → no gate-polling, no stuck-nudge) for another, when the
coupling between registration and authorization is the only thing actually broken.

## Status

open

## Cross-ref

- `docs/harness/claude-code/hooks/autoregister-pr.sh`
- `docs/harness/claude-code/settings.json.tmpl:145`, `:161`
- `docs/agent-rules/merge-gates.md:84`
- `agents/scripts/core/merge-watcher-cli.py`, `agents/scripts/core/merge-watcher.py`
- `docs/plans/shipped/smatchet-merge-watcher.md:102` (explicit owner transfer)
- AGENTS.md § Merge gates (auto-merge authorization), § Autonomous ship-loop default
  (post-ship 4-option question)
- `AI_POLICY.md` § Two loop modes (autonomy as granted / revocable)
- PR #2027 (observed instance), PR #2024 (second registered entry, same session),
  PR #2031 (this entry's own PR — reproduced the bypass on creation)
