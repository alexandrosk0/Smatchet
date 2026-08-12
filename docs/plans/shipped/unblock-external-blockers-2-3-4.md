# Unblock external blockers 2 / 3 / 4

Plan to retire three `docs/self-improvement/categories/external-blockers.md` entries that have low-cost in-repo workarounds even though each names an external upstream owner.

| # | Blocker (entry date) | Owner | Slice |
|---|---|---|---|
| 2 | Auto-merge disabled on the repo; `gh pr merge --auto` errors (2026-05-16) | GitHub repo settings | Slice 1 |
| 3 | vexp `<!-- vexp -->` block auto-regenerates in `AGENTS.md`; should land in `.claude/CLAUDE.md` instead (2026-05-13) | vexp tool upstream | Slice 2 |
| 4 | `mcp__vexp__run_pipeline` rejects `max_tokens` as float when JSON wire format is double (2026-05-12) | vexp tool upstream | Slice 3 |

Each slice ships independently. Slices 1 and 3 are ~15 min each; Slice 2 is ~1 h (hook + sweep + setup-harness wiring).

---

## Slice 1 — enable repo-level auto-merge

**Current state**: `gh api repos/alexandrosk0/Smatchet --jq .allow_auto_merge` returns `false`. `gh pr merge <N> --squash --auto --delete-branch` errors with `GraphQL: Auto merge is not allowed for this repository (enablePullRequestAutoMerge)`. The orchestrator + `git-janitor` fall back to direct `gh pr merge --squash --delete-branch` after the merge-gates poller greens, adding ~14 min wall-clock per PR.

**Target state**: `allow_auto_merge=true` at the repo level. Merge path collapses to a single `gh pr ready` + `gh pr merge --auto --squash --delete-branch` (no polling).

**Steps**:

1. Flip the setting via REST (one shot, idempotent):
   ```bash
   gh api -X PATCH repos/alexandrosk0/Smatchet \
     -F allow_auto_merge=true \
     -F delete_branch_on_merge=true
   ```
   `delete_branch_on_merge=true` is a free win that also removes the per-merge `--delete-branch` flag dependency. Currently `false`.

2. Verify:
   ```bash
   gh api repos/alexandrosk0/Smatchet --jq '{allow_auto_merge, delete_branch_on_merge}'
   # expect: {"allow_auto_merge":true,"delete_branch_on_merge":true}
   ```

3. Confirm branch protection on `develop` is compatible with auto-merge — auto-merge requires at least one required status check OR review-required, otherwise GitHub treats the PR as instantly mergeable and the `--auto` semantics degrade. Check:
   ```bash
   gh api repos/alexandrosk0/Smatchet/branches/develop/protection 2>&1 | head -40
   ```
   If branch protection is absent, document the constraint in `docs/plans/shipped/test-suite-expansion.md` § Auto-merge mechanics rather than enabling it as part of this slice (out of scope).

4. Restore `--auto` flag where it was previously stripped. Grep for the fallback:
   ```bash
   grep -rn "gh pr merge" agents/ scripts/ docs/plans/active/ AGENTS.md
   ```
   For each call site that currently uses `gh pr merge ... --squash --delete-branch` (no `--auto`), evaluate whether the caller is the orchestrator/`git-janitor` merge path or a deliberate immediate-merge (post-gates-pass). The merge-gates poller path stays immediate (gates already green by definition); the pre-gates path is the one that benefits from `--auto`.

5. Update `docs/plans/shipped/test-suite-expansion.md` § Auto-merge mechanics — replace "direct-merge fallback after CI greens (~270 s poll)" prose with the restored `--auto` flow. Cross-link to AGENTS.md § Merge gates so the two paths stay coherent.

6. Move the external-blockers entry to `docs/self-improvement/categories/applied.md` with a resolution stanza naming the REST `PATCH` + commit sha.

**Verification**:
- Open a throw-away PR (or piggyback on the next real one) and run `gh pr merge --auto --squash`. Expect success, not `enablePullRequestAutoMerge` error.
- `gh api repos/alexandrosk0/Smatchet --jq .allow_auto_merge` returns `true`.

**Risk**: low. Repo-settings flip is reversible via the same REST call with `=false`. No code change.

**Confirm-before-act**: this is a shared-state mutation on a remote (GitHub repo settings) — the orchestrator asks before running the `PATCH`.

---

## Slice 2 — relocate vexp block out of `AGENTS.md`

**Current state**: vexp tool regenerates `## vexp <!-- vexp v1.2.28 -->` … `<!-- /vexp -->` (33 lines) into three places — `AGENTS.md:398-430`, `.claude/CLAUDE.md:10-74` (gitignored, auto-generated), and `docs/harness/claude-code/CLAUDE.md.tmpl:10-74` (tracked, source for the harness template). The `AGENTS.md` and `.tmpl` copies are tracked and create cross-harness noise: Codex / Cursor / Aider read `AGENTS.md` per the agents.md spec and load ~30 lines of Claude-Code-specific MCP guidance they cannot use. Editing in place fights the regenerator.

**Target state**: vexp block lives in `.claude/CLAUDE.md` (Claude-Code-specific) and in `docs/harness/claude-code/CLAUDE.md.tmpl` (template source). `AGENTS.md` carries a one-line pointer instead of the full block. Re-regeneration by the vexp tool is auto-stripped from `AGENTS.md` on session start.

**Steps**:

1. Author `scripts/dev/vexp-block-strip-agents-md.sh` — idempotent stripper. Pseudocode:
   ```bash
   #!/bin/bash
   # Remove the vexp block from AGENTS.md if present.
   # Safe to run repeatedly. Exit 0 always (idempotent).
   set -euo pipefail
   AGENTS="${CLAUDE_PROJECT_DIR:-.}/AGENTS.md"
   [ -f "$AGENTS" ] || exit 0
   if ! grep -q '<!-- vexp v' "$AGENTS"; then exit 0; fi

   # Delete from "## vexp <!-- vexp v…" through "<!-- /vexp -->" inclusive,
   # plus the preceding blank line if present.
   awk '
     /^## vexp <!-- vexp v/ { skipping=1; next }
     skipping && /<!-- \/vexp -->/ { skipping=0; next }
     skipping { next }
     { print }
   ' "$AGENTS" > "$AGENTS.tmp" && mv "$AGENTS.tmp" "$AGENTS"

   # Drop trailing blank lines to keep the file tidy.
   sed -i -e :a -e '/^\s*$/{$d;N;ba' -e '}' "$AGENTS" || true
   ```

2. Wire as SessionStart hook in `.claude/settings.json` so every session begins with `AGENTS.md` clean:
   ```json
   {
     "hooks": {
       "SessionStart": [
         { "type": "command", "command": "bash scripts/dev/vexp-block-strip-agents-md.sh" }
       ]
     }
   }
   ```
   (Merge with whatever SessionStart entries already exist — caveman mode hook etc.)

3. One-shot manual strip + commit so the baseline is clean:
   ```bash
   bash scripts/dev/vexp-block-strip-agents-md.sh
   git add AGENTS.md
   ```
   Append a one-line pointer to AGENTS.md § Semantic-search exceptions (or its own micro-section) that names the canonical location:
   ```markdown
   ## vexp — Claude-Code-only

   vexp MCP-tool guidance lives in `.claude/CLAUDE.md` (regenerated by the vexp
   tool). Codex / Cursor / Aider fall back to text-search per § Harness adapter.
   ```

4. Decide policy for `docs/harness/claude-code/CLAUDE.md.tmpl`. Two options:
   - **A — keep block in template**: template is the source of truth for `.claude/CLAUDE.md` regen via `scripts/setup-harness.sh claude-code`. The vexp block belongs there. Continues to be tracked + auto-regenerated. No change.
   - **B — strip from template too**: forces the vexp tool to write directly into `.claude/CLAUDE.md` post-setup. Risk: setup-harness re-run wipes the vexp block until the user re-invokes vexp. Worse UX.

   **Pick A.** Strip is only from `AGENTS.md`.

5. File an upstream issue against the vexp tool with the following draft body (user task; orchestrator drafts the text but does not post):
   ```
   Title: vexp regenerates MCP guidance into AGENTS.md; please target .claude/CLAUDE.md only

   AGENTS.md is the harness-agnostic root per the agents.md spec
   (https://agents.md/). Codex / Cursor / Aider load it and ignore the
   Claude-Code-specific vexp section. Today the vexp installer/updater
   writes the `## vexp <!-- vexp vX.Y.Z -->` … `<!-- /vexp -->` block into
   AGENTS.md alongside .claude/CLAUDE.md. We work around this with a
   SessionStart hook that strips the block from AGENTS.md, but a clean fix
   is to target .claude/CLAUDE.md only (Claude-Code-specific harness
   surface) and leave AGENTS.md alone.

   Repro: …
   ```

6. Move the external-blockers entry to `applied.md` with a stanza naming the strip script, the SessionStart hook wire-up commit, and the upstream issue URL once filed.

**Verification**:
- After running `bash scripts/dev/vexp-block-strip-agents-md.sh` on a dirty `AGENTS.md`, `grep -c '<!-- vexp v' AGENTS.md` returns `0` and `.claude/CLAUDE.md` is untouched.
- Re-running the script on a clean `AGENTS.md` is a no-op (exit 0, no diff).
- Start a fresh Claude Code session; if vexp regenerates the block into `AGENTS.md` during boot, the SessionStart hook fires and `git diff AGENTS.md` shows no vexp block.
- Subagent that calls `run_pipeline` still works (vexp guidance is in `.claude/CLAUDE.md`, which Claude Code auto-loads via the existing `@../AGENTS.md` import pattern).

**Risk**: the awk delete is brittle if the vexp block format changes (e.g. opening line stops matching `## vexp <!-- vexp v…`). Pin the regex to the exact comment-marker form `<!-- vexp v` so a template tweak by the vexp tool breaks loudly rather than silently mis-stripping.

---

## Slice 3 — pin `max_tokens` int-literal convention

**Current state**: `mcp__vexp__run_pipeline` rejects `max_tokens: 12000.0` with `"floating point, expected usize"`. The single callsite in tracked docs (`docs/harness/claude-code/CLAUDE.md.tmpl:67`) already uses the int-literal form `max_tokens: 12000`. The issue surfaces when an agent / user types the call freehand with a decimal point.

**Target state**: the int-literal-only convention is documented in `AGENTS.md` § vexp section (or `.claude/CLAUDE.md`'s vexp section after Slice 2 lands) so future agents don't re-trip the same error. Existing usage is already correct.

**Steps**:

1. Audit current usage:
   ```bash
   grep -rn "max_tokens" agents/ scripts/ docs/ AGENTS.md .claude/CLAUDE.md 2>/dev/null
   ```
   Confirm zero `max_tokens: <int>.<frac>` patterns. If any are found, fix to int literal.

2. Add a one-line note to the vexp section's "Advanced Parameters" sub-section (in `.claude/CLAUDE.md` post-Slice-2, or `docs/harness/claude-code/CLAUDE.md.tmpl` for the regenerable source):
   ```markdown
   - `max_tokens: 12000` — increase total budget for complex tasks
     (**integer literal only**; the daemon rejects `12000.0` as "floating point, expected usize")
   ```

3. File the upstream issue against vexp asking the schema to accept integers-as-floats or to surface a clearer error message:
   ```
   Title: run_pipeline rejects max_tokens as float; JSON wire format is double

   JSON numbers are always doubles in the wire format. Callers writing
   `max_tokens: 12000.0` (or any non-int-literal int) get the cryptic
   "floating point, expected usize" error. Two options:

   1. Accept `f64` values that round-trip exactly to `usize` (i.e. integral
      doubles).
   2. Improve the error to name the parameter and require int literal:
      "max_tokens must be an integer literal, got 12000.0".

   Option 1 is the more JSON-spec-correct fix.
   ```

4. Move the external-blockers entry to `applied.md` with a stanza naming the docs commit + upstream issue URL.

**Verification**:
- `grep -rn 'max_tokens:.*\.' agents/ scripts/ docs/ AGENTS.md .claude/CLAUDE.md` returns no matches (no float-literal forms).
- Calling `run_pipeline({"task": "x", "max_tokens": 12000})` in a vexp-enabled session succeeds; `max_tokens: 12000.0` fails with the documented error (regression confirmation, not a new behaviour).

**Risk**: trivial. Pure doc edit. Upstream issue is informational.

---

## Out of scope

- Blocker 1 (Claude Code SDK worktree base ref) — distinct workaround (`git fetch origin develop && git rebase`) already documented in `docs/harness/SETUP.md`; reopening would be a separate plan.
- Branch-protection rules on `develop` — Slice 1 only flips `allow_auto_merge`; protection rule design is its own decision.
- Migrating other harness adapters (Codex `.codex/`, Cursor `.cursor/`) to a similar SessionStart-strip pattern for tool-specific blocks — none exist today; cross that bridge if a similar offender appears.

## Implementation log

- `2ba2c5bc` · #308 (2026-05-19) — unblocked external blockers 2/3/4 in one chore PR. Slice 1: enabled repo `allow_auto_merge` + `delete_branch_on_merge`. Slice 2: added `scripts/dev/vexp-strip-agents-md.sh` + wired it as a SessionStart hook in `.claude/settings.json`, plus the `## vexp — Claude-Code-only` pointer section in AGENTS.md. Slice 3: added the integer-literal-only `max_tokens` note (with the exact daemon error text) to `docs/harness/claude-code/CLAUDE.md.tmpl`.

## Deviations from plan

- **None material.** All three slices shipped as designed in a single PR rather than separately.

## Verification (actual)

- **Slice 1 — PASS (live re-check):** `gh api repos/<owner>/Smatchet` returns `allow_auto_merge: true`, `delete_branch_on_merge: true`.
- **Slice 2 — PASS:** `scripts/dev/vexp-strip-agents-md.sh` git-tracked, SessionStart hook present in `.claude/settings.json`, AGENTS.md pointer section present; strip is idempotent.
- **Slice 3 — PASS:** `CLAUDE.md.tmpl` carries the int-literal note + daemon error text; float-literal audit found zero real call-sites (only self-referential matches in this plan / backlog).
- **Open (user tasks, non-blocking):** two upstream vexp issues (Slice 2 step 5, Slice 3 step 3) drafted but not yet posted upstream — informational only.
