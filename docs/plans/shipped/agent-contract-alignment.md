# Agent contract alignment
<!-- plan-date: 2026-05-16 -->

Fix seven drift points between [`AGENTS.md`](../../AGENTS.md), the agent prompts under [`agents/`](../../agents/), and the telemetry hook at [`agents/_shared/token-tracking/agent-token-log.py`](../../agents/_shared/token-tracking/agent-token-log.py). All are agent-config-only — no `Source_Core/` code changes, no schema, no build.

## Goal

The four output-contract classes declared in `AGENTS.md` § Agent output contract are enforceable today only if (a) each agent's prompt mandates the contract's exact required headings, (b) the read-only / read-edit classification matches the agent's actual capability set, (c) the telemetry hook keys on `## Outcome:` not `## Self-improvement` proxy, and (d) the manual-step zero-tolerance rule in `AGENTS.md` § Verification automation isn't quietly contradicted by per-agent prompts. Today only (b) holds (and not for `debug-detective`). This plan fixes the other three for every drifted agent and updates `AGENTS.md` where the table itself is wrong.

## Affected components

| File | Change shape |
|---|---|
| `agents/architect.md` | Capability fix — either flip `read-only: true → false` + add `file-edit` / `shell` capabilities, **or** move plan write + commit responsibility to the orchestrator prompt and strike line 45's directive. |
| `AGENTS.md` § Agent output contract (line ~207) | Reclassify `debug-detective` — promote to a new "Diagnostic read-edit" row (or merge into Implementer with a note), since its prompt is `read-only: false` and explicitly writes `[temp-debug]` instrumentation + the NDJSON helper. |
| `agents/code-review.md` | Either bump banner to `sonnet/high` (line 31) **or** bump frontmatter to `effort: medium` (line 25) — pick one source of truth. Telemetry / statusline reads frontmatter; the banner is user-facing prose. |
| `agents/perf-measure.md` § Fallback (line 81+) | Replace "tell the user to open Perf panel and paste rows back" with "report CLI gap as blocked; hand off to `build-doctor` or `test-author`". The current fallback violates `AGENTS.md` § 2 (zero manual steps). |
| `agents/git-janitor.md` § Final report (line 267) | Promote the four required `Maintenance` headings (`## Pre-flight`, `## Mutations applied`, `## Regression gate`, `## Residue requiring user action`) from inside the fenced sample block to real top-level Markdown headings the hook can parse. |
| `agents/build-doctor.md`, `agents/test-author.md` | Add the same four `Maintenance` required headings explicitly. Hook + downstream consumers expect them. |
| `agents/tracker-backend.md` (line 50), `agents/grid-engine.md` (line 53), `agents/command-system.md` (line 52), `agents/offline-sync.md`, `agents/lua-binder.md`, `agents/mcp-toolsmith.md`, `agents/p4-blame.md`, `agents/unreal-bridge.md`, `agents/mechanic.md` | Replace one-line `Report: ...` directive with the three required `Implementer` headings: `## Files changed`, `## Smoke-test result`, `## Manual residue` (with the "must say 'none' if none" wording). |
| Every agent prompt (10 files: implementers + investigators + helpers + maintenance) | Mandate trailing `## Outcome: <state>` line per `AGENTS.md` line 212. State enum: `applied | halted | failed | partial | aborted`. |
| `agents/_shared/token-tracking/agent-token-log.py` (line ~181 `_infer_outcome`) | Tighten: remove rule 3 (`## Self-improvement` present → `applied`). Without an explicit `## Outcome:` tag, default to `applied` only when no halt keyword **and** no failure-shape detected; otherwise classify `partial` (so the silent-drift case becomes visible in telemetry instead of being painted green). |

## Interface contracts

No code interface changes. Prompt-shape contract changes:

- **Outcome tag** — every agent's final report **must** include `## Outcome: <state>` as its last heading before any optional `## Session context append`. Hook reads this; absence falls through to a tightened inference (see above).
- **Implementer report shape** — three headings, exact spelling: `## Files changed` (bullet list of relative paths), `## Smoke-test result` (one line: which `--preset` ran, pass / fail, optional scenario name), `## Manual residue` (bullet list or `none`).
- **Maintenance report shape** — four headings, exact spelling: `## Pre-flight`, `## Mutations applied`, `## Regression gate`, `## Residue requiring user action`. `git-janitor`'s current fenced sample becomes real headings.
- **Investigator report shape** — already in spec; debug-detective moves out of this class.
- **Banner ↔ frontmatter** — banner string `model/effort` substring **must** match `harness-hints.claude-code.{model,effort}` byte-for-byte. Lint script can grep both lines and diff them per agent.

## Risks

- **Hook backward compatibility** — tightening `_infer_outcome` rule 3 will reclassify in-flight historic JSONL rows on next read. Acceptable — the report scripts only key on per-row outcome, not aggregates over history.
- **Agent-token-log path drift** — `scripts/agent-token-log.py` is the path the user-supplied review references, but the file actually lives at `agents/_shared/token-tracking/agent-token-log.py` (with a junction copy under `.claude/hooks/`). Confirm the canonical path before editing; never edit the `.claude/hooks/` copy directly (it's a junction target).
- **architect capability flip vs prompt change** — flipping `read-only: true → false` widens the agent's blast radius (now harness will allow `Edit` / `Bash`). Safer alternative is moving plan write+commit to the orchestrator and keeping architect read-only. Decision pre-resolved below.
- **Existing in-flight agent calls** during the rollout — agents already running with the old prompts will continue with the old contract. No mitigation needed: at-most-one-prompt-revision-per-agent is fine; the hook tightening lands last.
- **Comment discipline** — these prompt edits are config, not code. No code comments will be added.

## Pre-resolved decisions

1. **architect** stays `read-only: true`. The plan write + immediate commit responsibility moves to the orchestrator's delegation packet. The architect emits the plan body as its report; the orchestrator writes it to `docs/plans/active/<slug>.md` and commits with `wip(plan): <slug>`. Line 45 in `architect.md` is rewritten to: "Emit the plan body as your report; the orchestrator persists and commits it."
2. **debug-detective** moves to a new fifth class row in the output-contract table: "Diagnostic read-edit" with required sections `## Hypotheses` → `## Evidence` → `## Cause` → `## Files changed (temp-debug)` → `## Cleanup verified` → `## Handoff`. The "temp-debug-only" write set is the discriminator; the `[temp-debug]` cleanup-verified heading is what guarantees no instrumentation ships.
3. **code-review** banner is wrong; frontmatter is right. Banner becomes `sonnet/high`. Telemetry / statusline already trust the frontmatter; the user-facing banner was the stale side.
4. **perf-measure fallback** is rewritten to: emit `## Outcome: halted` with `halt_reason: cli-gap`, name the missing CLI surface (e.g. "MCP socket unreachable"), and hand off to `build-doctor` (if MCP build broken) or `test-author` (if the scenario lacks a non-MCP CLI surface). No user keyboard / mouse step.
5. **Outcome tag** is mandated in every agent prompt. The hook stops inferring `applied` from `## Self-improvement` presence — that rule has produced false-green telemetry on halted runs. Replacement default is `partial` (visible in reports) when no explicit tag and no halt keyword and no clear success signature.
6. **Maintenance four headings** become real headings; `git-janitor`'s fenced sample block is split: the four `##` headings live outside the fenced ASCII art, the inner block becomes a non-canonical "rendering hint" for the user.
7. **Implementer three headings** replace the one-line `Report:` directives across all nine implementer agents. Existing one-line guidance becomes bullet content inside `## Files changed` / `## Smoke-test result` / `## Manual residue`.

## Implementation handoff

Single-slice plan — all changes are agent-prompt config + one Python file. No subsystem specialist needed; the orchestrator (or `mechanic` for the bulk find-and-replace passes) ships it as one PR.

| Step | Owner | Allowed write set | Notes |
|---|---|---|---|
| 1 | orchestrator | `AGENTS.md` § Agent output contract | Reclassify `debug-detective` row; document new "Diagnostic read-edit" class. |
| 2 | orchestrator | `agents/architect.md` (line 45) | Move plan-write responsibility to orchestrator; keep `read-only: true`. Update `docs/harness/SETUP.md` if it documents the old contract. |
| 3 | orchestrator | `agents/code-review.md` (line 31 banner) | `sonnet/medium` → `sonnet/high`. |
| 4 | orchestrator | `agents/perf-measure.md` § Fallback | Rewrite fallback to halted + handoff, no manual step. |
| 5 | mechanic | `agents/git-janitor.md`, `agents/build-doctor.md`, `agents/test-author.md` | Promote / add four `Maintenance` headings as real `##` Markdown. |
| 6 | mechanic | `agents/{tracker-backend,grid-engine,command-system,offline-sync,lua-binder,mcp-toolsmith,p4-blame,unreal-bridge,mechanic}.md` | Replace one-line `Report:` directive with three `Implementer` headings. |
| 7 | mechanic | every agent prompt (~17 files) | Append `## Outcome: <state>` mandate at the end of each prompt's "every response" block, with the enum spelled out. |
| 8 | orchestrator | `agents/_shared/token-tracking/agent-token-log.py` (`_infer_outcome`) | Drop rule 3; tighten default. Add a unit test under `agents/_shared/token-tracking/tests/` if a test rig exists; otherwise add a `__main__` smoke harness. |
| 9 | orchestrator | `agents/_shared/token-tracking/agent-token-log.py` docstring + `scripts/agent-tokens-report.py` if it documents the inferred-outcome rules | Sync docstring to new behaviour. |
| 10 | orchestrator | `docs/backlog/AGENT_SELF_IMPROVEMENT.md` | Flip the entries (if any) that this plan resolves; record the contract sweep as one entry. |

## Verification

Every item bucketed per `test-author` taxonomy (`AGENTS.md` § Verification automation). Zero manual steps.

| Item | Bucket | Concrete check |
|---|---|---|
| All ten implementer prompts contain the three required headings | A · CLI | `python` script greps each named file for the three exact heading strings; exits non-zero on any miss. Lives under `scripts/dev/test-agent-contract.sh`. |
| All three maintenance prompts contain the four required headings | A · CLI | Same script, second pass. |
| Every agent prompt ends with `## Outcome:` mandate text | A · CLI | Same script, third pass — greps for the literal mandate sentence. |
| Banner ↔ frontmatter match per agent | A · CLI | Same script — parses YAML frontmatter `harness-hints.claude-code.{model,effort}` and the `Banner` line; diffs the substring. |
| architect prompt no longer instructs the agent to commit | A · CLI | Same script — `architect.md` must NOT contain `git commit` directive in the body. |
| `_infer_outcome` returns `partial` when input lacks `## Outcome:` and contains `## Self-improvement` but no halt keyword | A · CLI | New `tests/_token-tracking/test_infer_outcome.py` (or equivalent), invoked from `scripts/dev/test-agent-contract.sh`. Asserts the four old test cases still pass and the new one classifies `partial` instead of `applied`. |
| `_infer_outcome` returns `halted` when halt keyword present | A · CLI | Same test file. |
| `_infer_outcome` returns the explicit tag when `## Outcome: failed` is present | A · CLI | Same test file. |
| `scripts/dev/test-all.sh` exits 0 after the new test rig is wired in | A · CLI | Unified runner. Part of CI gate. |
| No `Source_Core/` build break (paranoia check — should be a no-op since plan is prompt-only) | A · CLI | `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`. |

Bucket E (ImGui Test Engine) not needed — no UI surface touched. Bucket D (sanitizer) not needed — no C++ touched. Bucket B (scenario) not needed — no command-registry change. Bucket C (screenshot) not needed — no theme / locale change.

## Open questions

1. **agent-token-log.py path canonicality** — should the editing pass touch `agents/_shared/token-tracking/agent-token-log.py` and let the junction propagate to `.claude/hooks/agent-token-log.py`, or are these two files independent? If independent, both need the same edit. Confirm before step 8.
2. **architect-as-emitter contract** — the orchestrator currently expects architect to land its own plan doc. Moving plan-write to the orchestrator means the orchestrator now needs a "wait for architect's report, write to `docs/plans/active/<slug>.md`, commit `wip(plan): <slug>`, then continue" sub-loop. Is that worth the contract simplification, vs the alternative of flipping `architect.read-only` to `false` and adding `file-edit` / `shell` capabilities? Pre-resolved decision was "stay read-only", but worth a sanity check before step 2.
3. **debug-detective output contract** — the new "Diagnostic read-edit" class adds two headings (`## Files changed (temp-debug)` + `## Cleanup verified`) that no other class has. Is the cost of a fifth class worth it, or should `debug-detective` collapse into Implementer with a note that `## Manual residue` covers cleanup? Pre-resolved decision was "fifth class"; alternative is one less heading per debug-detective run.
4. **Telemetry rerun** — should historic JSONL be re-classified once `_infer_outcome` tightens, or is forward-only fine? Pre-resolved: forward-only. Confirm if anything reads aggregates over history before step 8.

## Implementation log

- **bf0bd167** · `chore(agents): split debug-detective into new Diagnostic class + architect emit-only` — AGENTS.md output-contract table extended from 4 → 5 classes. New **Diagnostic read-edit** row with `debug-detective` as sole member; required headings `## Hypotheses` → `## Evidence` → `## Cause` → `## Files changed (temp-debug)` → `## Cleanup verified` → `## Handoff`. `agents/architect.md` L45 rewritten to "emit-only" — orchestrator persists + commits the plan body; agent stays `read-only:true`. architect bumped v1 → v2.
- **e36c35c5** · `chore(agents): land output-contract required headings (Implementer + Maintenance classes)` — 14 files: 9 implementer prompts (`tracker-backend`, `grid-engine`, `command-system`, `offline-sync`, `lua-binder`, `mcp-toolsmith`, `p4-blame`, `unreal-bridge`, `mechanic`) replaced one-line `Report:` directive with the 3 Implementer headings; build-doctor + test-author appended `## Final report — Maintenance class` sections with the 4 Maintenance headings; git-janitor added the 2 missing Maintenance headings (`## Mutations applied` + `## Residue requiring user action`) and renamed `## Regression gate (final, mandatory)` → `## Regression gate` to match spec; code-review banner `sonnet/medium` → `sonnet/high` (frontmatter `effort:high` is truth-source); perf-measure manual-fallback rewritten to halted-with-handoff (no more "open the Perf panel + paste back"). Every touched agent's version + banner bumped.
- **2c79c3bf** · `chore(agents): mandate ## Outcome: line per AGENTS.md output contract` — 16 files: every prompt missing the `## Outcome:` mandate text gets it appended before the existing `## Self-improvement` directive. Post-condition: 24/24 agent prompts contain the mandate. perf-detective, perf-instrument, security-review, spike-hunter received their first version bump in this PR.
- **<sha-tbd>** · `chore(telemetry): tighten _infer_outcome + ship contract-alignment audit script` — `agents/_shared/token-tracking/agent-token-log.py:_infer_outcome` rule 3 removed (`## Self-improvement` → `applied` mapping); default tightened to `partial` when no explicit tag and no halt keyword. Hook copy at `.claude/hooks/agent-token-log.py` mirrored (Windows-side independent copy per `setup-harness.sh` `link_file()` short-circuit when destination exists). `scripts/dev/test-agent-contract.sh` ships as the bucket-A audit rig (8 sub-checks: required headings × 3 classes, Outcome mandate coverage, banner↔frontmatter match, architect emit-only, AGENTS.md 5-class table, `_infer_outcome` unit cases). `agents/_shared/token-tracking/tests/test_infer_outcome.py` ships 9 unit cases covering the new + existing behaviour. `scripts/agent-tokens-report.py` comment updated to reflect the new historic-row default. debug-detective bumped v3 → v4 (renamed `## Instrumentation (now stripped)` → `## Files changed (temp-debug)` + `## Proposed Fix For Handoff` → `## Handoff (proposed fix)`).

**Verification** (run 2026-05-19):

- ✅ `bash scripts/dev/test-agent-contract.sh` exits 0 — 18/18 sub-checks pass
- ✅ `python agents/_shared/token-tracking/tests/test_infer_outcome.py` — 9/9 cases pass
- ✅ 24/24 agent prompts contain `## Outcome:` mandate (was 6/24)
- ✅ `_infer_outcome` returns `partial` for `## Self-improvement` only (was `applied`)
- ✅ AGENTS.md § Agent output contract has 5 class rows (was 4)
- ✅ Hook copy at `.claude/hooks/agent-token-log.py` byte-identical to canonical

## Deviations from plan

- **debug-detective heading rename instead of additions** — plan said "no body edits needed". Audit run found 2 of the 6 Diagnostic-class headings didn't match AGENTS.md spec (debug-detective had `## Instrumentation (now stripped)` and `## Proposed Fix For Handoff`). Resolved via 2 surgical renames + v3 → v4 bump rather than adding new headings (would have duplicated existing content). Same outcome for downstream consumers; minimum diff.
- **Audit class-row regex fix** — first audit run failed AGENTS.md 5-row check because the regex `^\| \*\*(Investigator|Diagnostic|Implementer|Helper|Maintenance)\*\*` couldn't match `**Diagnostic read-edit**` (whitespace between `Diagnostic` and the closing `**`). Relaxed to `\b` word-boundary. Plan didn't anticipate the per-row trailing-text variance.
- **Hook copy mirror via `cp -f` not setup-harness** — plan listed both options (`cp` mirror OR `rm + setup-harness.sh` refresh). Chose `cp -f` for determinism; setup-harness L77-79 short-circuits when destination exists, so a refresh would have been a no-op without the prior `rm`. Audit script + unit test rig both read the canonical path; `.claude/hooks/` copy is only consumed by the live Claude Code SubagentStop hook.
