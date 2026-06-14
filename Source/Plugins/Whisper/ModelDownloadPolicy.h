#pragma once

// ModelDownloadPolicy — pure (no cpr / no I/O) network-hardening decisions for the
// Whisper model download (security audit LOW: "Whisper model-download redirect — no
// host-pin, no size cap"). ModelDownloader streams catalog `.bin` files from the
// huggingface mirror with redirects enabled (huggingface LFS sends a 30x from the
// resolve-main pointer over to a CDN host, so disabling redirects is not an option);
// instead every URL the transfer touches — the initial URL AND each redirect target —
// must resolve to a host on the huggingface allow-list, and the streamed/declared size
// must stay under a sane model-size ceiling. SHA-256 verification (in ModelCatalog)
// already pins artefact identity; this layer caps the blast radius of a redirect to an
// attacker host and an unbounded-size DoS BEFORE the bytes hit disk.
// Test surface: tests/Plugins/Whisper/ModelDownloadPolicy.test.cpp.

#include <cstdint>
#include <string>

namespace smatchet {
namespace whisper {
namespace policy {

// Hard ceiling on a model download, in bytes. The largest catalog model (ggml-medium.en)
// is ~1.53 GB; 4 GiB leaves generous headroom for a future larger model while still
// rejecting an attacker/redirect that streams an unbounded body to exhaust disk. Any
// Content-Length above this, or a streamed byte count crossing it, aborts the transfer.
constexpr std::uint64_t kMaxModelDownloadBytes = 4ull * 1024ull * 1024ull * 1024ull;

// Extract the lowercased bare host from an absolute URL (`scheme://[user@]host[:port]/...`).
// Returns empty on a URL with no `://` or no host. Pure: no DNS, no I/O.
std::string ExtractHost(const std::string& url);

// Is `host` on the huggingface model-download allow-list? Accepts the apex `huggingface.co`
// and any subdomain of it (`*.huggingface.co` — the LFS/Xet CDN hosts the `resolve/main`
// pointer 30x-redirects to, e.g. `cdn-lfs.huggingface.co`, `cas-bridge.xethub.hf.co` is
// NOT covered intentionally — see note) plus `*.hf.co` (the short HF domain the newer Xet
// CDN uses). Case-insensitive, exact-suffix (a `evilhuggingface.co` is rejected). Pure.
bool IsAllowedModelHost(const std::string& host);

// Combined host-pin decision for an URL the transfer is about to touch (initial or
// redirect target). Returns true iff the URL is https AND its host is allow-listed.
// A non-https URL is rejected (the catalog URLs are all https; a redirect that downgrades
// to http would leak the request and is refused). Pure.
bool IsAllowedModelUrl(const std::string& url);

// Size-cap decision. Returns true iff `bytes` exceeds the ceiling (i.e. the transfer must
// abort). Used both on the Content-Length header and on the running streamed byte count.
bool ExceedsModelSizeCap(std::uint64_t bytes);

} // namespace policy
} // namespace whisper
} // namespace smatchet
