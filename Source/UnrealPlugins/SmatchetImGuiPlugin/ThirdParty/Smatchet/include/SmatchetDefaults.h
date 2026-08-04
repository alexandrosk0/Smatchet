#pragma once

// <cstdlib> is no longer used by this header (its only consumer was the deleted GetEnvString —
// gate-blind-spot-sweep Slice 1b) but is RETAINED deliberately: this header reaches most of the
// tree through ConfigManager.h / SmatchetUiSession.h / McpPlugin.h, and several TUs
// (StandaloneAppBootstrap.cpp, SmatchetBugReportUi.cpp, CrashSink.cpp, SemanticVersionPure.cpp)
// call std::getenv / std::exit / std::atoi while relying on exactly this transitive path. Dropping
// it is an include-what-you-use sweep with its own blast radius, not part of a dead-code deletion.
#include <cstdlib>
#include <string>

namespace SmatchetDefaults {

constexpr char kDefaultDbPath[] = "Smatchet_LocalCache.sqlite";
constexpr char kDefaultBackendType[] = "Jira";

// UI font size (points). Single source of truth for the default + the legible clamp range so the
// config default, the load-time clamp, the ui.zoom.* command clamp, and the View > Appearance
// menu enable-gates can't drift apart. Default 16 matches the legacy hardcoded value.
constexpr int kFontSizeDefaultPt = 16;
constexpr int kFontSizeMinPt = 8;
constexpr int kFontSizeMaxPt = 32;

// Bug-report relay — seeded as the first-run default so a fresh install can
// submit bug/crash reports with zero setup. The relay holds the GitHub token
// server-side; the key below is a low-value shared access key (abuse
// mitigation, rate-limited + rotatable on the worker — NOT a credential).
// Users can point at their own relay or clear it in Preferences.
constexpr char kDefaultBugReportRelayUrl[] = "https://smatchet-bug-report-relay.smatchet.workers.dev/report";
constexpr char kDefaultBugReportRelayKey[] = "SmatchetKey";

namespace Mcp {
constexpr int kDefaultPort = 42360;
constexpr char kBindLocalhost[] = "127.0.0.1";
constexpr char kBindAny[] = "0.0.0.0";
constexpr char kSsePath[] = "/mcp/sse";
} // namespace Mcp
} // namespace SmatchetDefaults
