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

} // namespace TrackerSetupPure
