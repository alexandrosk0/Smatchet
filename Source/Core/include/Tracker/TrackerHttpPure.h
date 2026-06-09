#ifndef SMATCHET_TRACKER_HTTP_PURE_H
#define SMATCHET_TRACKER_HTTP_PURE_H

#include <string>

/**
 * Pure, cpr-free TLS-trust seam for the tracker HTTP layer (WS2 of
 * docs/plans/active/mobile-mvp-completion.md, Issue #1068).
 *
 * Why a separate, cpr-free header: the Android host shell (Source/Mobile/Android/
 * android_main.cpp) resolves the CA-bundle path via JNI at boot and must feed it into
 * Core *before* the first HTTPS request — but the host TU must not pull <cpr/cpr.h>.
 * This header carries the path holder + the resolution logic with std::string only;
 * the cpr-side attachment (cpr::SslOptions / CURLOPT_CAINFO) lives in
 * TrackerHttpUtils.cpp, the only consumer of cpr.
 *
 * Trust model: this seam only ever *adds* an explicit CAINFO (cafile). It never
 * disables peer/host verification — there is intentionally no knob to turn TLS
 * verification off. An empty path means "no explicit CAINFO" → libcurl keeps its
 * compile-time / system-store default (the desktop path, unchanged).
 *
 * Test surface: tests/Core/TrackerHttpSslPure.test.cpp.
 */
namespace TrackerHttpPure {

/** Resolved SSL trust decision. `attach == false` means "leave libcurl's default CAINFO
 *  untouched"; `attach == true` means "set CURLOPT_CAINFO to caInfoPath". */
struct SslConfig {
    std::string caInfoPath;
    bool attach = false;
};

/**
 * Decide whether to attach an explicit CAINFO for a given CA-bundle path.
 *  - Empty path        -> {caInfoPath: "", attach: false}  (libcurl default — desktop).
 *  - Non-empty path    -> {caInfoPath: path, attach: true} (explicit cafile — Android).
 * Pure: no globals, no cpr. Verification is never disabled by this seam.
 */
SslConfig ResolveSslConfig(const std::string& caBundlePath);

/**
 * Process-global CA-bundle path holder. The host sets it ONCE at boot (before any
 * tracker HTTP request); the verb helpers read it per request. Set-once-before-use is
 * the contract — it is deliberately unsynchronized (mirrors the prior process-global
 * CURL_CA_BUNDLE env approach). Empty (default) on desktop.
 */
void SetCaBundlePath(const std::string& path);
const std::string& GetCaBundlePath();

} // namespace TrackerHttpPure

#endif
