# Plan — PR intent capture (prompt → PR `## Intent`, redacted)

> **Slug**: `pr-intent-capture` (matches this file's basename without `.md`).
>
> **Status**: `active`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

PR descriptions today carry no durable trace of the **originating human prompt** — a reviewer (or future archaeologist) sees the diff and a freeform `## Summary`, but not *why the work was asked for in the first sentence the user typed*. The user wants traceability: human ask → diff. Intended outcome — **after this lands, every ship-loop PR opens with a `## Intent` section holding a redacted one-line summary of the prompt(s) that drove it, and a WARN-first CI gate nudges when that section is missing/empty.**

Security constraint baked into the design: the captured prompt can contain secrets (tokens, key=value pairs, private-key blocks) and home-dir paths that leak a username. The capture path therefore runs every prompt through a **fail-safe redaction pass** before anything is persisted — a raw, unredacted prompt must NEVER reach the capture file (and thus never the public PR body). Over-redaction is acceptable; under-redaction is not.

## Approach

Four cooperating mechanisms (the user's "a+b+c+d"):

- **(a) Template stub** — add a `## Intent` section to `.github/pull_request_template.md` with an HTML-comment placeholder. The placeholder is the "unfilled" sentinel the gate (d) keys on.
- **(b) Ship-loop rule** — a rule in `docs/agent-rules/ship-loops.md` instructing the orchestrator, at `gh pr create` time, to read the branch's capture file and synthesise the `## Intent` body from the accumulated redacted one-liners (batched feature → many prompts → one PR → one synthesised Intent).
- **(c) Capture hook + redactor** — a `UserPromptSubmit` Claude Code hook (`capture-intent.sh`) parses `.prompt` from the event JSON, pipes it through a committed, portable Python redactor (`redact-intent.py`) that strips high-confidence secrets + collapses home-dir usernames and emits **one safe line**, then appends that line to a gitignored, branch-keyed capture file. The hook prints **nothing** to stdout (UserPromptSubmit stdout is injected into model context) and exits 0 unconditionally. Fail-safe: if the redactor errors or `python3` is absent, the hook writes **nothing** — never the raw prompt.
- **(d) WARN-first gate** — a new **non-required** `pull_request`-only job in `.github/workflows/doc-validation.yml` reads `github.event.pull_request.body`, and if the `## Intent` section is missing or still only the placeholder, emits a `::warning::` annotation. **Always exits 0** — advisory, never red (AGENTS.md § Merge gates: "Never merge past ANY red check").

The redactor is split out as its own committed Python unit (not inline bash regex) for two reasons: (1) the secret-redaction regex set needs case-insensitivity + alternation that sed BRE/ERE handles poorly, and (2) a standalone unit is directly + deterministically testable (bats drives `python3 redact-intent.py` with crafted inputs). The bash hook stays thin: JSON-parse → pipe → append.

Harness-wiring note: `.claude/` is gitignored and regenerated, so the committed source of truth is the template under `docs/harness/claude-code/` + registration in `settings.json.tmpl` + a `copy_template` line in `setup-harness.sh`; `sync-settings-hooks.sh` additively heals the new `UserPromptSubmit` event into already-provisioned settings on next session start.

## Files to modify

1. `agents/scripts/core/redact-intent.py` (**new**) — portable Python3 redactor. stdin raw text → stdout single redacted line. The testable security unit.
2. `docs/harness/claude-code/hooks/capture-intent.sh` (**new**) — `UserPromptSubmit` hook: parse `.prompt` (jq, sed fallback), pipe through redactor, append to capture file, no stdout, exit 0.
3. `docs/harness/claude-code/settings.json.tmpl` — register the `UserPromptSubmit` event → `capture-intent.sh` (no such event today; healer adds it cleanly).
4. `agents/scripts/core/setup-harness.sh:~240` — add a `copy_template` line for `capture-intent.sh` in `setup_claude_code()`.
5. `.github/pull_request_template.md` — add the `## Intent` section + placeholder near the top.
6. `docs/agent-rules/ship-loops.md` — add the (b) orchestrator rule near the PR-creation/PR-batching content.
7. `.github/workflows/doc-validation.yml` — add the non-required `pull_request`-only WARN-first Intent-lint job.
8. `.gitignore` — ignore the capture path (`.session-intent/`).
9. `tests/bats/capture_intent.bats` (**new**) — redaction + one-liner-collapse + fail-safe + end-to-end-hook tests.

## Existing utilities reused

- `docs/harness/claude-code/hooks/clear-tree-dirty.sh` — reference hook pattern (stdin JSON, jq-with-sed-fallback `.tool_input.command` parse, `CLAUDE_PROJECT_DIR` resolution, exit 0). The capture hook mirrors its shape, parsing `.prompt` instead.
- `agents/scripts/core/sync-settings-hooks.sh` — additive jq heal of NEW template hook events into a deployed `settings.json`; confirmed to append a brand-new top-level `UserPromptSubmit` event key.
- `agents/scripts/core/setup-harness.sh` `copy_template()` — copies + chmods `.sh`, skips user-modified files.
- `tests/bats/postmortem_owed.bats` — bats convention (REPO_ROOT resolve, PATH-stub temp dirs, env seams).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — no `Source/Core/` / UI-thread code touched; the hook runs out-of-process on prompt submit, not in the frame loop.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — same reason; no sync I/O reaches `ImGui::*`.
- **Pillar 3 (never crash)**: no impact on product binary. Hook robustness is the fail-safe design (errors/absent-python → write nothing, exit 0; never blocks prompt submission).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no UI surface added.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

N/A — the diff touches only CI workflow, agent-rule docs, harness hook templates, a Python script, `.gitignore`, the PR template, and a bats test. No `Source/Core/` / `project.config.json` `lint.zones` / `perf` paths. All five perf-system gates (PR-fast CI, Pillar-2 scanner, dispatcher drain, bucket-E cue, marker inventory) are **N/A** for this reason.

## Risks / non-goals

- **Risk — redaction misses a secret class** → a secret could reach the gitignored capture file and (if the orchestrator copies it verbatim) the public PR body. Mitigation: fail-safe over-redaction (generic key=value + long hex/base64 catch-alls beyond the named token formats); the orchestrator rule (b) instructs synthesise-don't-paste; an adversarial `security-review` pass on the redactor before merge; the bats suite asserts each secret class is stripped.
- **Risk — false-positive over-redaction** mangles a legitimate prompt summary. Accepted: a clipped Intent line is a cosmetic loss; a leaked secret is not. Over-redaction is the safe failure direction by design.
- **Risk — `UserPromptSubmit` stdout leaks into model context.** Mitigation: hook writes only to the capture file; emits nothing on stdout; verified by a bats assertion that stdout is empty.
- **Non-goal — retroactively backfilling Intent on existing/older PRs.** Forward-only.
- **Non-goal — making the gate (d) blocking/required.** WARN-first by explicit user choice; revisiting to blocking is a separate calibration decision (mirrors the `duplication` WARN-first precedent).
- **Non-goal — porting the hook to non-Claude-Code harnesses.** The capture file + ship-loop rule are harness-agnostic, but the `UserPromptSubmit` capture mechanism is Claude-Code-specific this slice; other harnesses fall back to the orchestrator filling `## Intent` from the live prompt.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no `Source/Core/` pure helper added; the redactor is Python, tested via bats below.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**: `tests/bats/capture_intent.bats` — drives `python3 redact-intent.py` with crafted inputs asserting each secret class (JWT, `gh_*`/AWS/Slack keys, key=value pairs, PRIVATE KEY block, long hex/base64) is redacted, home-dir usernames are collapsed, emails are preserved, output is a single line; plus an end-to-end hook test (JSON stdin → capture file written, stdout empty) and the fail-safe (no-python / redactor-error → no raw prompt written). Run: `bats tests/bats/capture_intent.bats`.
- **Build gate**: N/A — no C++ touched; `cmake --build` not exercised (pure docs/CI/shell/Python diff).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint / markdown-links / required-context-parity — defer to the script for the sub-step list). A red doc-validation job blocks merge even though non-required. The new (d) job is itself added to this workflow; `test-required-context-parity` must still pass (the new job is **non-required**, so it need not emit unconditionally).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the harness-wiring + redaction-failsafe model before finalising; record the outcome.
- **Manual residue**: none expected — the redactor + hook are fully bats-driven. If the `UserPromptSubmit` live-fire (real session writing a real capture line) can't be asserted in CI, that residue is covered by the end-to-end hook bats test (synthetic JSON stdin), so no manual step remains.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them. (This is a net-new feature, so no prior "deferred-as-current" refs are expected.)

- **Per-harness capture parity** (Codex/Cursor/Aider `UserPromptSubmit` equivalents) — follow-up if the feature proves out on Claude Code; no-action this slice.
- **Promoting (d) to a required/blocking gate** — separate calibration decision; no-action now.
- **Structured multi-prompt Intent rendering** (e.g. bulleted per-prompt history vs one synthesised line) — the rule (b) synthesises one line; richer rendering is a follow-up if reviewers want it.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
