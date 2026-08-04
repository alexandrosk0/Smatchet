#pragma once
// First-run tracker-setup decisions (dev-onboarding-first-run-quickstart plan, slice 2).
// Pure + ImGui-free so the Preferences Tracker tab and the bucket-A rig can share one
// definition of "is this install still being set up?". No network, no disk, no globals.
//
// State source: this helper deliberately owns NO persisted flag of its own. The
// "has this install ever had a working tracker?" latch already exists as
// TrackerConfig::BackendHasBeenReachable (ConfigManager.h:148) — set once the live
// connectivity tick observes AuthenticatedReachable (SmatchetUI.cpp:689-693), persisted
// via the kBoolFields table, and already gating the menu bar (SmatchetUI_MainMenu.cpp:144)
// and the grid body (SmatchetActiveProjectGridTable.cpp:484). A second
// `TrackerSetupCompleted` key would be a second source of truth for one state; see the
// plan's § Deviations.

// Bare include per the source-core-dir-reorg convention: Source/Core/include/Config is on
// the include path for both the core targets and the doctest rig.
#include "ConfigManager.h"

#include <string>

namespace TrackerSetupPure {

/// True when every credential field the ACTIVE backend needs is non-empty. Thin alias over
/// ConfigManager::BackendCredentialsPresent (ConfigManager_Views.cpp:283) resolved against
/// cfg.TrackerType — the per-backend required-field table lives there and stays single-source.
bool CredentialFieldsComplete(const TrackerConfig& cfg);

/// True while the Tracker tab should show the first-run explainer: either the backend has
/// never been confirmed reachable, or the credential fields are incomplete. Veterans migrate
/// for free — `backend_has_been_reachable` is already persisted, so an existing user who has
/// ever connected reads false here without a migration pass.
bool NeedsSetup(const TrackerConfig& cfg);

/// Stable digest of the credential fields the ACTIVE backend authenticates with, used to pin a
/// green "Test connection" verdict to the exact values that were probed. Save & Sync recomputes
/// it from the live buffers and refuses to clear read-only when it differs, so probing, then
/// editing the token, then saving cannot unlock on an unverified value.
/// FNV-1a over backend key + fields, hex-encoded. Not a security primitive — it never leaves
/// memory and only has to change when a field changes. Deliberately returns a digest rather
/// than the raw values so the session struct does not hold a second plaintext copy of a token.
std::string CredentialFingerprint(const TrackerConfig& cfg);

/// Applies the first-run unlock that Save & Sync performs, and reports whether it fired.
/// Unlocks only when read-only is still on AND `verifiedFingerprint` is a non-empty pin that
/// still matches `cfg` — i.e. the exact values a "Test connection" probe returned
/// AuthenticatedReachable for. On that edge BOTH halves of the first-run state move together:
/// ReadOnlyMode clears and BackendHasBeenReachable latches. Clearing only the former would
/// leave NeedsSetup(), the locked main menu and the grid welcome state all still reading
/// first-run until the connectivity monitor independently latched the same verdict a tick
/// later. Pure so the pairing is covered by the bucket-A rig rather than only in a live frame.
/// The pin is ONE-SHOT: it is consumed (cleared) on the firing edge, which is why it is taken
/// by non-const reference. Without that, a user who re-checks "Read-only mode" later in the
/// same Preferences session and saves again with unchanged credentials would have the surviving
/// pin match a second time and their deliberate choice silently reverted with no cue.
bool ApplyVerifiedSaveUnlock(TrackerConfig& cfg, std::string& verifiedFingerprint);

} // namespace TrackerSetupPure
