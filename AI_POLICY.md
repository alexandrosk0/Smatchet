# AI_POLICY.md — human-authority charter

> **Governance layer.** This charter sits **above** the operating contract in
> [`AGENTS.md`](AGENTS.md). `AGENTS.md` says *how* to build; `AI_POLICY.md` says
> *who is in control and when the agent must stop*. Where the two appear to
> conflict, this charter's authority/escalation rules bound the operating
> contract's autonomy — they do not delete any operating rule, they place it
> under the mode spectrum defined here. The operating mechanics
> (ship-loop stages, merge gates) live in `AGENTS.md` and
> [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md).

## Authority

Humans own **quality and cost**. Agent autonomy is a **granted, revocable
mode**, not a default right. Everything the agent does is **auditable** — the
existing PR / commit / `## Self-improvement` / [`postmortems`](docs/self-improvement/postmortems.md)
trail lets the human reconstruct what shipped and why, at any time. The agent
never takes an action it cannot make auditable.

## Two loop modes (human-selected)

Set per session/task via the `SMATCHET_LOOP_MODE` env var; surfaced at
SessionStart by the `## === loop-mode: <on|in> ===` banner
(`agents/scripts/core/clear-session-context.sh`). Config defaults live in
`project.config.json` § `governance`.

- **human-on-the-loop** (`SMATCHET_LOOP_MODE=on`) — the action-biased mode
  defined at [`ship-loops.md`](docs/agent-rules/ship-loops.md) § Standing user
  default: commit / push / open-PR autonomously; resolve **reversible** forks
  with a sensible default and surface them in the turn summary; pause only on
  the enumerated ship-loop exceptions. The agent acts; the human monitors and
  can interrupt.

- **human-in-the-loop** (`SMATCHET_LOOP_MODE=in`) — execute **only within an
  approved plan**; pause at each decision point **not covered by the plan**; do
  not improvise scope. The human authorises a plan, then the agent runs it,
  escalating rather than guessing on anything the plan did not settle.

**Default = `project.config.json` § `governance.loop_mode`** (operator-owned;
read at SessionStart by `agents/scripts/core/clear-session-context.sh`, with an
explicit `SMATCHET_LOOP_MODE` env var overriding per session, and a fail-safe to
`in` when the config is unreadable). **Currently set to `on`** (human-on-the-loop
/ autonomous) — the operator selected action-biased autonomy. Flip the config
value back to `in`, or export `SMATCHET_LOOP_MODE=in`, to return to the
conservative plan-gated mode (recommended for unattended high-risk work).

**Standing auto-merge grant (`governance.auto_merge`).** A separate, revocable
grant (same resolution: `SMATCHET_AUTOMERGE` env > config > `off` fallback): when
`on`, the agent auto-squash-merges a PR once the **full merge-gates poll passes**
(CI + CodeRabbit + Bugbot + unresolved comments), **without** the per-PR post-ship
merge prompt — the gates still bind; only the *asking* is removed. It does not
grant merging past a real blocker, and a rate-limited / capped review bot is an
out-of-band downgrade (`*-out-of-band` label), not a licence to skip a genuine
finding. Revoke by setting `governance.auto_merge: off`.

## Escalate, don't assume (invariant in BOTH modes)

Before acting, the agent must be able to **autonomously validate** the action —
a gate / test / spec confirms correctness, the scope is authorised, and the cost
is bounded. If it **cannot**, it **escalates** via `AskUserQuestion` with the
blocker named; it never fills the gap with an assumption. Concrete triggers:
ambiguous spec, no gate/test to confirm correctness, irreversible-and-unauthorised,
or cost-unbounded. This is ship-loop pause-exception **(6)
cannot-autonomously-validate / cost-unbounded** and fires in on-the-loop mode
too — it is a *new pause trigger*, not a weakening of on-the-loop autonomy
within its authorised scope. The runnable-validation surface **per environment**
is declared in [`project.config.json`](project.config.json) § environments
(surfaced by `scripts/dev/doctor.sh --tier <name>`): a check that lands as
`[n/a]` there is a *validate-elsewhere* signal — treat it as
cannot-validate-**here** (escalate / defer to the tier that owns it), never as a
silent pass. "Validated" means a **concrete** signal
(gate/test/spec, authorised scope, bounded cost); rationalising an assumption as
"validated" is itself a misjudgement the
[`gate-escape-postmortem`](agents/_shared/skills/gate-escape-postmortem/SKILL.md)
loop catches after the fact.

## Cost control

Token / compute spend is a **human-governed budget** (gauged by
`agents/_shared/token-tracking/`). The agent surfaces cost and **escalates
before** an unbounded or expensive autonomous run. Runaway spend without
validation is forbidden.

The automated backstop is the **advisory session cost-ceiling check**
(`agents/scripts/core/cost-ceiling-check.py`, run at SessionStart via
`cost-ceiling-nudge.sh`): when delegated-subagent spend recorded by the
token-tracking layer meets `project.config.json` §
`governance.session_token_ceiling`, it prints an ESCALATE banner directing the
agent to pause and confirm budget with the human before further delegation.
Advisory (never blocks — the WARN-first ramp every gate here starts on);
graduation to blocking is a deliberate future decision (`--blocking` exists).
The ceiling is the human's number: tune or disable (0) in config.

The second cost lever is **per-agent model tiering**
([`delegation.md § Model tiering`](docs/agent-rules/delegation.md)): each
agent pins a `model:` tier (opus / sonnet / haiku) matched to its task class
instead of inheriting the session model.

## Scope of this charter

Governs the **autonomous agents' relationship to human control** on this
solo-maintained, prerelease project. It does **not** govern outside human
contributors (no external-contributor disclosure / denounce rules while solo —
revisit if the repo opens up, same trigger as the solo-merge-review ADR), and it
adds no AI-generated-content rules (irrelevant to a C++ app harness).

## Attribution

The pattern of a root **`AI_POLICY.md` governance charter separated from the
`AGENTS.md` instruction contract** is adapted from **[Ghostty](https://github.com/ghostty-org/ghostty)**
(Mitchell Hashimoto et al., MIT-licensed) — the comparison that surfaced this
governance gap. Ghostty's `AI_POLICY.md` governs *outside human contributors
using AI*; Smatchet's charter is a distinct adaptation governing the
*autonomous agents' relationship to human control* (loop modes, escalate-when-
unvalidatable, cost), so the structure is borrowed but the content is original
to this project.
