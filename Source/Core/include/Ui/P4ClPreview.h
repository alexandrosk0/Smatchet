#ifndef SMATCHET_P4_CL_PREVIEW_H
#define SMATCHET_P4_CL_PREVIEW_H

#include "ConfigManager.h"
#include "P4Annotate.h"

#include <chrono>
#include <string>

/**
 * Async changelist-preview tooltip, extracted from AnnotateAnalysisUi_Modals.cpp
 * so non-Annotate windows (User Info window) can reuse it. The hover slot +
 * describe cache live here (keyed by CL), no longer on the Annotate window's
 * pimpl singleton. UI-thread only, except Cache() which is thread-safe.
 */
namespace P4ClPreview {

/**
 * Draw the CL tooltip for the hovered cell. First call for a new `cl` kicks an
 * async `p4 describe` fetch (never blocks the UI thread); subsequent frames show
 * "Loading CL info..." until ready. Call between the hovered item and EndTooltip
 * scope rules — this opens/closes its own BeginTooltip/EndTooltip.
 */
void DrawClTooltipAsync(const std::string& cl, const AnnotateAnalysisConfig& cfg, const AnnotateUiThemeColors& theme);

/**
 * Detach an in-flight hover fetch (call when the owning window closes so the
 * future is not abandoned mid-flight). Detached futures are reaped by
 * ReapDetached().
 */
void DetachInFlight();

/** Reap completed detached hover futures. Call once per frame from a poll site. */
void ReapDetached();

/** Process-wide `p4 describe -s` cache shared by the tooltip and detail loaders. Thread-safe. */
P4ChangelistDescribeCache& Cache();

/**
 * CPP_CODE_AUDIT.md #30: the hover/detached futures live in a Meyers singleton whose
 * destructor runs at static-destruction time; a `p4 describe` still in flight there
 * blocks process exit for however long that call takes, with no diagnostic. Call this
 * explicitly from the app's shutdown path (BEFORE static destruction) so an unfinished
 * fetch is waited on with a logged timeout instead.
 *
 * The default matches (with headroom) P4Annotate.cpp's own `kP4ProcessTimeoutMs`
 * (120000 ms) — the hard cap SubprocessCapture::Run already enforces on every
 * `p4 describe` call. That means this wait isn't just theater papering over the same
 * eventual block: in every case except SubprocessCapture itself failing to honor its
 * own timeout, the fetch WILL be ready before this returns, so the destructor at
 * static-destruction time finds an already-ready future and doesn't block at all. This
 * function intentionally does NOT reset/detach `HoverFut` on timeout — the future stays
 * live so a later completion can still be observed, and so `Cache` (destroyed after
 * these futures, in reverse member-declaration order) is never touched by a task whose
 * future has already been discarded.
 */
void DrainForShutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(130000));

} // namespace P4ClPreview

#endif
