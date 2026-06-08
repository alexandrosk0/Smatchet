#pragma once

// UiThreadAffinity — low-layer UI-thread registry so that blocking sync-I/O wrappers
// (ConfigManager disk writes, LoadPersistentViewsFromDisk, P4RunCommand) can detect when
// they are (wrongly) invoked on the UI render thread and surface it — closing the Pillar-2
// gate gap where the static pillar2-scan.sh is blind to these *wrapper* functions
// (docs/plans/close-gate-gaps.md, Gap A / Slice 1a).
// Why here and not AppController: AppController already owns a private `uiThreadId_` +
// `IsOnUiThread()`, but the wrappers live BELOW AppController (in Config/ and P4Annotate),
// so they cannot depend on it without a layering inversion. This util holds the same
// publish-once thread id in a dependency-free place both layers can query.
// Threading: `SetUiThread()` is called exactly once at UI startup (before any worker
// thread spawns), then never mutated — the same publish-once discipline as
// `AppController::IsOnUiThread()`. Reads from worker threads are race-free under
// publish-once semantics (acquire/release on the registered flag).

namespace UiThreadAffinity {

/// Register the calling thread as the UI thread. Call once, at UI init, before any worker
/// thread is spawned. Idempotent re-registration (same thread) is harmless.
void SetUiThread();

/// True iff called on the registered UI thread. Returns **false** when the UI thread has not
/// been registered yet (process startup before `SetUiThread()`), so callers fail open during
/// startup rather than mis-flagging early config reads.
bool IsOnUiThread();

/// If currently on the UI thread, emit a Pillar-2 violation `LOG_ERROR` naming `context`
/// (the blocking wrapper) and return true; otherwise no-op and return false. Warn-only by
/// design for Slice 1a — it must not abort while the known violators (#565/#611/#732/#761)
/// are still live; a follow-up hardens this to a debug-build assert once Slice 1b clears them.
bool WarnIfOnUiThread(const char* context);

} // namespace UiThreadAffinity
