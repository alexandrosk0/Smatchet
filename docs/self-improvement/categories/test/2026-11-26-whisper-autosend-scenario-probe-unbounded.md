# The Whisper autosend scenario driver can hang, and fails on the shared profile

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-26
- **Observed on**: PR #2496 (input UX Wave 2 leftover, one dictation reload drain), local verification.
- **Status**: open

## What happened

`scripts/dev/test-whisper-ai-assistant-autosend.sh` first probes `Smatchet.exe cmd commands.list --spawn` to see whether `whisper.status` is registered. On the developer's machine it ran against the default user-data profile, which other sessions' instances share, and failed in two ways:

- One run hung for 1 h 45 min. The ephemeral child (`--ephemeral --mcp-port …`) listened on its port and wrote nothing to its spawn log. The probe has no timeout, so the script waited until the child was killed. It then printed `SKIP: whisper.status not registered (SMATCHET_WITH_WHISPER=OFF)`, although Whisper was compiled in.
- Later runs failed fast: `--spawn: ephemeral instance rejected the MCP auth token (HTTP 401) — spawn token handshake failed.`, again reported as the Whisper-OFF SKIP.

With `SMATCHET_USER_DATA` set to a fresh directory the scenario passed 7/7. The bucket-E drivers do not hit this, because `scripts/dev/lib/ui-test-driver.sh` isolates the profile. This scenario driver does not source it.

A probe failure is reported as "Whisper is off" and exits 0, so on a machine where the probe breaks the scenario never runs and still looks green.

## Concrete next action

- Isolate the profile the way the bucket-E drivers do: a throwaway `SMATCHET_USER_DATA` and `SMATCHET_UPDATE_CHECK=0`, from the shared preamble if it fits.
- Put a timeout on the probe and on the `scenario.run --spawn` call.
- Tell a failed probe (non-zero exit, `"ok":false`, or a timeout) apart from a probe that answered without `whisper.status`. Only the second is a SKIP; the first is a FAIL.
- Check `scripts/dev/test-whisper-roundtrip.sh`, which uses the same probe shape.
