#pragma once

// Android platform helpers for the host shell: APK asset font loading + display-density
// resolution, plus the host-shell logcat logging macros. This is the only host TU that
// includes <android/asset_manager.h> / <android/configuration.h>, keeping those headers
// out of every other shell file (and entirely out of Core).
//
// Logging: the host shell logs via __android_log_print (logcat) — the platform-native
// logging facility for an Android NativeActivity, analogous to the Source/Standalone/
// pre-logger-init exception. Core code keeps using LOG_* (Logger.h); these macros are
// for the platform layer only (boot breadcrumbs, EGL/JNI errors visible in `adb logcat`).

#include <android/log.h>

#include <vector>

struct AAssetManager;
struct AConfiguration;

#define SMATCHET_ANDROID_LOG_TAG "Smatchet"
#define SLOG(...) ((void)__android_log_print(ANDROID_LOG_INFO, SMATCHET_ANDROID_LOG_TAG, __VA_ARGS__))
#define SLOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, SMATCHET_ANDROID_LOG_TAG, __VA_ARGS__))

namespace smatchet {
namespace mobile {

// Read a file bundled in the APK assets (e.g. "fonts/Roboto-Medium.ttf" or "certs/cacert.pem")
// into a byte blob. Returns an empty vector + logs on failure (asset missing / unreadable); the
// caller then falls back (default font / no CA bundle). The blob is returned by value — the caller
// retains/consumes it (font: pass .data()/.size() to SmatchetSetInjectedFontBytes; cert: write to disk).
std::vector<unsigned char> ReadApkAsset(AAssetManager* assetManager, const char* assetName);

// Display density scale = AConfiguration_getDensity() / 160 (160 dpi == mdpi == scale 1.0).
// Returns 1.0 when the configuration is null or reports an unknown/zero density (caller error
// or pre-config boot) so SmatchetTheme::ApplyUiDensityScale stays a safe no-op.
float ResolveDensityScale(AConfiguration* config);

} // namespace mobile
} // namespace smatchet
