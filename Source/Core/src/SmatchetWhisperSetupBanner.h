#pragma once

// SmatchetWhisperSetupBanner — first-run dictation consent banner pinned to
// the top of the main window immediately under the menu bar.
//
// Lifecycle: visible while `cfg.WhisperSetupCompleted == false`. Three
// terminal user actions plus one "decide later":
//   - "Enable ▾"        — expands an inline picker → "Download + enable"
//                         triggers a model fetch; on success sets
//                         WhisperEnabled=true + WhisperSetupCompleted=true.
//   - "Decide later"    — no state change; banner re-shows next launch.
//   - "No thanks"       — WhisperSetupCompleted=true, WhisperEnabled=false;
//                         banner never reappears (reversible via Preferences).
//   - "Cancel" (inside picker) — collapses back to the three-button banner.
//
// Owns a static ModelDownloader instance shared with Preferences so a fetch
// kicked off from the banner survives a banner re-render. The static lives
// in the TU; clients reach it via `BannerOwnedDownloader()` so Preferences
// can poll the same progress.

#include <string>

class IAppThreading; // narrow AppController threading facet — see Commands/IAppThreading.h
struct TrackerConfig;

namespace smatchet {
namespace whisper {

class ModelDownloader;

namespace banner {

/// Renders the banner if `cfg.WhisperSetupCompleted` is false. Returns
/// true when the caller should re-save the config (a button click changed
/// fields on cfg). The caller (SmatchetUI.cpp main draw) is responsible
/// for `ConfigManager::Save(cfg)` on a true return.
///
/// MUST be called between the main menu bar and the dock space — the
/// banner uses ImGui::SetNextWindowPos to pin itself to the top of the
/// viewport's work-area so the menu bar stays clickable above it.
bool Render(IAppThreading& app, TrackerConfig& cfg);

/// Returns a reference to the banner-owned ModelDownloader. Same instance
/// used by Preferences for the "Download" button so a fetch started from
/// the banner shows progress on the Preferences tab if the user opens it
/// mid-fetch.
ModelDownloader& BannerOwnedDownloader();

} // namespace banner
} // namespace whisper
} // namespace smatchet
