# Feature-gated command registry (no disabled-feature stubs)

# Status

**Accepted (2026-05-24).** Plan: [`docs/plans/shipped/light-release-unreal-default.md`](../plans/shipped/light-release-unreal-default.md) grill Q3/Q7.

# Context

Optional Smatchet surfaces (AI assistant, MCP server, Whisper dictation) compile behind `SMATCHET_WITH_*` CMake flags per [`docs/adr/0002-plugin-shim-link-discipline.md`](0002-plugin-shim-link-discipline.md). Release **light feature profile** ([`docs/CONTEXT.md`](../CONTEXT.md) § Plugin architecture — *Light feature profile*) ships Lua + command palette ON and MCP/AI/Whisper OFF.

Historically, `RegisterAiCommands` registered **stub** `ai.*` commands when `SMATCHET_WITH_AI=OFF` so CLI/MCP/palette enumeration returned *handler-error* (“AI not built”) instead of `unknown-command`. That made sense when DX12 embed builds compiled AI TUs without the shim and agents might probe command names blindly.

Light/Unreal-default packaging now **removes** disabled-feature TUs from `CORE_SOURCES` (Whisper-style CMake gate) and targets a **clean palette** — users and agents should not see commands for features that are not in the binary.

# Decision

When an optional feature flag is **OFF**, its commands are **absent from `CommandRegistry`** — no stub handlers, no disabled menu toggles, no “feature not built” palette entries.

- **`commands.list` / palette / MCP tools/call** on an OFF build: disabled-feature names resolve to **`unknown-command`** with fuzzy `suggestions` among **registered** commands only.
- **Automation** that needs `ai.*`, MCP tools, or `whisper.*` must run against **full** `Smatchet.exe` (`ninja-publish-msvc`), not `Smatchet-Light.exe` or light/Unreal embed libs.
- **Lua glue** keeps unconditional no-op stubs on `AppController` ([`AppController.h`](../../Source_Core/include/AppController.h)) so scripts do not need parallel `#if` gates; only **registry-exposed** command names follow the absent-when-OFF rule.
- **Standalone CLI on MCP-OFF builds** (`Smatchet-Light.exe`): full `cmd` surface for registered commands via **in-process dispatch** (headless boot + `CommandRegistry::Dispatch` + JSON stdout) — not MCP attach. See plan [`light-release-unreal-default.md`](../plans/shipped/light-release-unreal-default.md) grill Q1 revision (2026-05-24).

# Considered options

- **Stub commands on OFF builds (previous contract).** Pro: agents discover `ai.*` exists and get a typed error. Con: palette noise; implies feature is present but broken; contradicts light-profile goal of honest surface area.
- **Absent when OFF (adopted).** Pro: registry reflects actual capabilities; smaller command set; matches MCP/Whisper OFF behavior (never stubbed). Con: scripts must know which exe/profile they target.

# Consequences

- Delete the `#else` stub block in [`BuiltinCommands_Ai.cpp`](../../Source_Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp); `RegisterAiCommands` no-ops when AI is OFF.
- Release smoke may assert **`ai.*` not in** `commands.list` output for light zips.
- [`docs/plans/shipped/command-system-plan.md`](../plans/shipped/command-system-plan.md) § Feature-gated builds documents the contract for agents.
- Reversing this later requires re-introducing stubs **and** updating smoke/ADR — treat as intentional API shape, not an implementation detail.
