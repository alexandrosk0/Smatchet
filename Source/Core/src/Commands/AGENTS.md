# Commands subsystem — agent rules

Scoped rules for `Source/Core/src/Commands/` (the unified command system: CLI + Palette + MCP + Lua + Scenarios). Global rules stay in the root [`AGENTS.md`](../../../../AGENTS.md). Design: [`docs/plans/shipped/command-system-plan.md`](../../../../docs/plans/shipped/command-system-plan.md).

This is a **strict lint zone** (root `AGENTS.md` § Tiered enforcement zones).

## Invariants

- **Command signature is fixed.** A command handler takes a `const CommandContext&`, returns the structured error envelope (success flag + machine-readable error), and defaults its `args` to `{}` so a no-arg invocation is valid. Don't add out-of-band parameters or throw across the command boundary — surface failure in the envelope.
- **One registry, no bypass.** CLI, Command Palette, MCP, Lua, and Scenarios all dispatch through `CommandRegistry`. Flag any path that reaches a command's effect directly (e.g. an MCP tool or Lua binding calling the underlying service instead of `Execute`-ing the registered command) — bypasses fork validation, logging, and the error envelope.
- **Register once, surface everywhere.** A new command is added to the registry (see `BuiltinCommands` / `ViewCommands`) and is then automatically reachable from every front-end. Don't special-case a command into one front-end only.
- **Destructive confirm is source-aware and never auto-set by a binding.** `Dispatch` gates destructive commands through the pure predicate `RequiresExplicitConfirm(source, destructive, confirmed, dryRun)` in `Commands/Command.h`; the gate is uniform (no source bypasses confirm) and dispatches from automation sources (CLI/MCP/Lua, see `IsAutomationSource`) are audit-logged via `LOG_WARN`. The binding layers must set `ctx.ConfirmedDestructive` **only** from an explicit per-call signal — `--yes` (CLI), `__confirm:true` in args (MCP/Lua), palette Shift+Enter / Unreal console flag. A binding that blanket-sets `ConfirmedDestructive=true` for any non-UI source re-opens security audit #2/#3.

## Before you edit

- A new command = one `CommandRegistry` registration + its handler; the Palette, CLI, MCP schema, and Lua binding pick it up from the registry. Adding a config key or scenario step routes through the same registry contract.
