#ifndef SMATCHET_SEMANTIC_VERSION_PURE_H
#define SMATCHET_SEMANTIC_VERSION_PURE_H

#include <string>

/**
 * Pure semantic-version parse/compare lifted from AttachmentAppUpdateService.cpp's anonymous
 * namespace so the update-check version logic can be unit-tested and FUZZED without linking the
 * service's heavy transitive deps (cpr, Logger, TrackerHttpUtils).
 *
 * Implementation is byte-identical to the original; only namespace + linkage changed.
 * `ParseSemanticVersion` runs over a GitHub release tag drawn straight from an update-check HTTP
 * response (untrusted network ingress), so it must terminate on ANY input and never overflow:
 * each dotted segment is validated all-digits then range-gated via `std::stoll` rather than
 * `std::atoi`, which is UB on a digit run past INT_MAX (CPP_CODE_AUDIT.md #17).
 *
 * Owner: AttachmentAppUpdateService (update check) — pure helper lives here.
 * Test surface: tests/fuzz/fuzz_semver_parse.cpp (libFuzzer over the raw tag bytes).
 */
namespace smatchet {
namespace version {

struct SemanticVersion {
    int Major = 0;
    int Minor = 0;
    int Patch = 0;
    bool Valid = false;
};

/// Parse a `[v]MAJOR.MINOR.PATCH[-prerelease]` tag into a SemanticVersion. A leading `v` and a
/// `-prerelease` suffix are stripped; every segment must be an all-digit run within [0, INT_MAX].
/// Any malformed input yields `{0,0,0, Valid=false}` — never a throw, hang, or overflow.
SemanticVersion ParseSemanticVersion(const std::string& raw);

/// Order two versions by (Major, Minor, Patch): -1 / 0 / 1.
int CompareSemanticVersion(const SemanticVersion& a, const SemanticVersion& b);

} // namespace version
} // namespace smatchet

#endif
