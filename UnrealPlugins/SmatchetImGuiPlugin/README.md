# Smatchet Unreal Plugin Communication Manual

This plugin embeds the Smatchet ImGui UI in Unreal and exposes the same unified
Smatchet command system to Blueprint and C++.

Use this guide when you want an Unreal project to talk to Smatchet from gameplay,
editor utility code, automation, or Blueprint UI.

## Communication Paths

There are two supported ways to communicate with the plugin.

1. Human UI: press `Ctrl+Shift+J` in Unreal to show or hide the Smatchet overlay.
   Once the overlay is visible, use Smatchet normally, including the command
   palette inside the overlay.
2. Programmatic command bridge: call `USmatchetImGuiCommandBridge` from
   Blueprint or C++. This sends commands to Smatchet in-process and returns JSON
   results asynchronously.

`Ctrl+Alt+J` is only a cursor diagnostic toggle. It suppresses or restores
Smatchet's software cursor in game viewports.

## Command Model

Commands are named with dotted identifiers such as:

- `commands.list`
- `commands.help`
- `app.version`
- `config.get`
- `config.set`

Every command takes a JSON object string as its arguments. Use `{}` when the
command has no arguments.

Every result is a JSON envelope:

```json
{
  "ok": true,
  "command": "app.version",
  "data": {
    "version": "..."
  }
}
```

Failures use the same envelope shape:

```json
{
  "ok": false,
  "command": "missing.command",
  "error": {
    "code": "unknown-command",
    "message": "No command named 'missing.command'.",
    "hint": "Did you mean 'commands.list'?",
    "suggestions": ["commands.list"]
  }
}
```

The Unreal bridge is asynchronous. Enqueueing a command returns immediately with
a request id. The result becomes available on a later tick after the native
Smatchet host drains its command queue.

## Blueprint API

The Blueprint function library is:

```cpp
USmatchetImGuiCommandBridge
```

It appears under the Blueprint category:

```text
Smatchet | Commands
```

Available Blueprint-callable functions:

- `EnqueueSmatchetCommand`
- `EnqueueSmatchetCommandWithCallback`
- `IsSmatchetCommandResultReady`
- `TakeSmatchetCommandResultJson`

### Preferred Blueprint Flow

Use `EnqueueSmatchetCommandWithCallback` for most Blueprint work.

Inputs:

- `CommandName`: dotted command name, for example `commands.list`
- `ArgsJson`: JSON object string, for example `{"full":true,"limit":500}`
- `bConfirmedDestructive`: set `true` only when intentionally running a
  destructive command
- `bDryRun`: set `true` to preview commands that support dry-run
- `OnComplete`: delegate that receives the result JSON string

Example:

```text
CommandName: commands.help
ArgsJson: {"name":"app.version"}
bConfirmedDestructive: false
bDryRun: false
```

The callback receives the JSON result. Parse the string, check `ok`, then read
either `data` or `error`.

### Polling Blueprint Flow

Use polling only when you need to store the request id yourself.

1. Call `EnqueueSmatchetCommand`.
2. Store the returned request id.
3. On a timer or tick, call `IsSmatchetCommandResultReady`.
4. When ready, call `TakeSmatchetCommandResultJson`.

`TakeSmatchetCommandResultJson` consumes the result. Calling it again for the
same request id will return `false`.

Do not use both callback and manual polling for the same request id. The callback
path takes the result internally.

## C++ API

Include the public bridge header:

```cpp
#include "SmatchetImGuiCommandBridge.h"
```

Callback example:

```cpp
UCLASS()
class AMyActor : public AActor {
    GENERATED_BODY()

public:
    UFUNCTION()
    void OnSmatchetResult(const FString& ResultJson);

    void QuerySmatchetVersion() {
        FSmatchetCommandResultDelegate OnComplete;
        OnComplete.BindDynamic(this, &AMyActor::OnSmatchetResult);

        const int64 RequestId =
            USmatchetImGuiCommandBridge::EnqueueSmatchetCommandWithCallback(
                TEXT("app.version"),
                TEXT("{}"),
                false,
                false,
                OnComplete);

        if (RequestId == 0) {
            // Native host is not available yet.
        }
    }
};
```

Polling example:

```cpp
const int64 RequestId = USmatchetImGuiCommandBridge::EnqueueSmatchetCommand(
    TEXT("commands.list"),
    TEXT("{\"full\":true,\"limit\":500}"),
    false,
    false);

// Later, on tick or a timer:
if (USmatchetImGuiCommandBridge::IsSmatchetCommandResultReady(RequestId)) {
    FString ResultJson;
    if (USmatchetImGuiCommandBridge::TakeSmatchetCommandResultJson(RequestId, ResultJson)) {
        // Parse ResultJson.
    }
}
```

## Discovering Commands

Start with the metadata commands.

List available commands:

```json
{
  "command": "commands.list",
  "args": {"full": true, "limit": 500}
}
```

Get schema and help for one command:

```json
{
  "command": "commands.help",
  "args": {"name": "config.get"}
}
```

Search by fuzzy name:

```json
{
  "command": "commands.search",
  "args": {"query": "sync", "limit": 10}
}
```

The Unreal default package is the light profile: Lua and the command registry
are available, while MCP, AI assistant implementation, and Whisper are disabled.
Commands for disabled features are intentionally not registered.

## Common Examples

Get Smatchet build information:

```text
CommandName: app.version
ArgsJson: {}
```

Read safe config values:

```text
CommandName: config.get
ArgsJson: {"key":"trackerType"}
```

List all exposed safe config values:

```text
CommandName: config.get
ArgsJson: {}
```

Preview a supported config write:

```text
CommandName: config.set
ArgsJson: {"key":"readOnlyMode","value":"true"}
bDryRun: true
```

Apply that config write:

```text
CommandName: config.set
ArgsJson: {"key":"readOnlyMode","value":"true"}
bDryRun: false
```

Run a destructive command:

```text
CommandName: app.quit
ArgsJson: {}
bConfirmedDestructive: true
```

If `bConfirmedDestructive` is `false`, destructive commands return a
`confirm-required` error envelope instead of executing.

## Host Availability

`EnqueueSmatchetCommand` returns `0` when the native host is unavailable. Common
causes:

- The plugin module is not loaded.
- The call happens very early during startup.
- The deployed plugin is missing native ThirdParty libraries.

The native host initializes lazily when the overlay is first shown. If a command
is enqueued before initialization, it can remain pending until the host starts
draining its queue. For manual smoke testing, press `Ctrl+Shift+J` once to show
the overlay and allow the host to initialize.

## Safety Rules

- Always send a JSON object string. Empty string and `{}` both mean no args.
- Prefer `commands.help` before invoking a command you do not know.
- Use `bDryRun=true` before applying changes when a command supports dry-run.
- Set `bConfirmedDestructive=true` only for commands that the user explicitly
  approved.
- Do not send secrets through `config.set`. Credentials are intentionally not in
  the command allowlist; use environment variables such as
  `SMATCHET_TRACKER_TOKEN` instead.
- Do not enqueue commands every frame. Enqueue on user action, timers, or
  automation steps, then wait for the result.

## Troubleshooting

`RequestId` is `0`:

- The native host is unavailable. Confirm the plugin is enabled and the project
  has the packaged `ThirdParty/Smatchet/lib/Win64/Development/*.lib` payload.

Callback never fires:

- Show the overlay once with `Ctrl+Shift+J` to initialize the native host.
- Make sure another code path did not already call `TakeSmatchetCommandResultJson`
  for the same request id.
- Check the Unreal Output Log for Smatchet native host startup messages.

Result says `unknown-command`:

- Call `commands.list` to inspect the commands registered in this build.
- Remember that Unreal light builds omit MCP, AI, and Whisper commands.

Result says `validation-error`:

- `ArgsJson` is not valid JSON or is not a JSON object.
- Call `commands.help` for the command and compare against `inputSchema`.

Unreal links against stale native libraries:

- Repackage from the repo with:

```powershell
.\scripts\dev\package_unreal_plugin_msvc.ps1 -Configuration Release -PackageOnly -ForceConfigure
```

For the local TestProject smoke rebuild, use:

```powershell
.\scripts\dev\rebuild_testproject_plugin.ps1 -Release
```

## Low-Level Native ABI

Game code should use `USmatchetImGuiCommandBridge`. The lower-level C ABI exists
so the Unreal module can talk to the native Smatchet host without relying on a
C++ ABI boundary:

- `SmatchetHost_EnqueueCommand`
- `SmatchetHost_IsCommandResultReady`
- `SmatchetHost_TakeCommandResultJson`
- `SmatchetHost_ReleaseCommandResultJson`

If you call the C ABI directly from plugin-maintenance code, every non-null
result returned by `SmatchetHost_TakeCommandResultJson` must be released with
`SmatchetHost_ReleaseCommandResultJson`.
