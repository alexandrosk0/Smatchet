# Cursor adapter

Generated locally at `.cursor/rules/agents.mdc` by:

```bash
bash scripts/setup-harness.sh cursor
# or, on Windows-native PowerShell:
pwsh scripts/setup-harness.ps1 cursor
```

Idempotent. The script never overwrites a user-modified rule file.

## What it does

Cursor auto-loads `.mdc` files from `.cursor/rules/` and applies them based on the rule's frontmatter (globs + `alwaysApply`). The shipped `agents.mdc` template:

- Sets `alwaysApply: true` so the rule is always in context.
- Points Cursor at `AGENTS.md` (project rules + delegation table) and `agents/*.md` (per-agent definitions).

That's the whole adapter — Cursor's built-in file-search + the rule's pointer are enough for the agent files to be discovered.

## Template source

`docs/harness/cursor/rules/agents.mdc` — edit there to change the project default. Edits to `.cursor/rules/agents.mdc` after setup are local-only.

## Refreshing after a `git pull`

Re-run `bash scripts/setup-harness.sh cursor`. The script copies the template only if you haven't locally modified `.cursor/rules/agents.mdc`.

## Removing the adapter

```bash
rm -rf .cursor
```

Safe — fully regenerable via the setup script.
