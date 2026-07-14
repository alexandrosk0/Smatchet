#ifndef SMATCHET_DIAGNOSTICS_ENGINE_HOST_CONTEXT_H
#define SMATCHET_DIAGNOSTICS_ENGINE_HOST_CONTEXT_H

#include <cstdint>
#include <string>

// EngineHostContext — landing seam for the host-engine context snapshot pushed
// across the C ABI (SmatchetHost_SetHostContextJson). The Unreal plugin pushes a
// small JSON snapshot (~1 Hz, game thread) describing the live editor session:
// engine version, project, current map, PIE state, selected actors, Output Log
// tail. The quick-create issue popup formats it into the description prefill
// (EngineContextFormat.h). Standalone never writes it, so GetSnapshotJson()
// returns "" there and consumers skip the engine section entirely.
//
// docs/plans/quick-create-issue-unreal-context.md.

namespace smatchet {
namespace hostctx {

/// Replace the current snapshot. Thread-safe (any thread; the Unreal plugin
/// calls from the game thread while the UI reads from the render thread).
/// An empty string clears the snapshot. Oversized payloads (> the internal
/// ~128 KB cap) are rejected and logged, keeping a hostile/buggy host from
/// ballooning core memory. Bumps the revision on every accepted change.
void SetSnapshotJson(const std::string& json);

/// Copy of the latest accepted snapshot ("" when none). Thread-safe.
std::string GetSnapshotJson();

/// Monotonic revision of the accepted snapshot (0 = never set). Lets the UI
/// cheaply detect "context changed since my prefill" without comparing bodies.
std::uint64_t Revision();

} // namespace hostctx
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_ENGINE_HOST_CONTEXT_H
