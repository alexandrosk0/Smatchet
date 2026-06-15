#ifndef SMATCHET_TRACKER_HTTP_PURE_H
#define SMATCHET_TRACKER_HTTP_PURE_H

#include <string>

// Pure, cpr-free TLS-trust seam for the tracker HTTP layer (WS2 of
// docs/plans/mobile-mvp-completion.md, Issue #1068). The Android host shell
// resolves the CA-bundle path via JNI at boot and feeds it into Core before the first
// HTTPS request, without pulling cpr into the host TU. This header carries the path
// holder plus resolution logic with std::string only; the cpr-side attachment (the
// CURLOPT_CAINFO option) lives in TrackerHttpUtils.cpp.
// Trust model: this seam only ever adds an explicit CAINFO cafile; it never disables
// peer or host verification, and there is no knob to turn verification off. An empty
// path means no explicit CAINFO, so libcurl keeps its compile-time or system-store
// default (the desktop path, unchanged).
// Test surface: tests/Core/TrackerHttpSslPure.test.cpp.
namespace TrackerHttpPure {

// Resolved SSL trust decision. When attach is false, leave libcurl's default CAINFO
// untouched; when attach is true, set CURLOPT_CAINFO to caInfoPath.
struct SslConfig {
    std::string caInfoPath;
    bool attach = false;
};

// Decide whether to attach an explicit CAINFO for a given CA-bundle path. An empty path
// yields no attachment (libcurl default, desktop); a non-empty path yields an explicit
// cafile attachment (Android). Pure: no globals, no cpr. Verification is never disabled.
SslConfig ResolveSslConfig(const std::string& caBundlePath);

// Process-global CA-bundle path holder. The host sets it ONCE at boot (before any tracker
// HTTP request); the verb helpers read it per request. Set-once-before-use is the
// contract — deliberately unsynchronized (mirrors the prior process-global CURL_CA_BUNDLE
// env approach). Empty (default) on desktop.
void SetCaBundlePath(const std::string& path);
const std::string& GetCaBundlePath();

// Is `host` a loopback/localhost literal? Case-folds and strips an optional `:port` and
// bracketed IPv6. Accepts any `127.x.x.x`, `localhost`, `::1`, `[::1]`. Pure: no DNS,
// literal-only (mirrors the MCP DNS-rebinding loopback policy).
bool IsLoopbackHost(const std::string& host);

// Cleartext-credential hardening (audit LOW: NormalizeBaseUrl accepts `http://`). A tracker
// base URL configured as cleartext `http://` sends the Basic-auth credentials in the clear.
// Decision: a cleartext `http://` base whose host is NON-loopback must be upgraded to
// `https://` (returns true). Loopback `http://` (local dev / a loopback dev config) and any
// `https://` are left untouched (returns false). `rawBase` is the scheme-prefixed base
// (e.g. "http://corp-jira.example.com"). Pure: no I/O.
bool ShouldUpgradeCleartextBase(const std::string& rawBase);

} // namespace TrackerHttpPure

#endif
