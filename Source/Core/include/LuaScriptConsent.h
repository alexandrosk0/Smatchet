#pragma once

// LuaScriptConsent — the pure decision core behind the first-run Lua script
// consent gate. A Scripts/*.lua file may only be executed once the user has
// approved that exact content (keyed on path + sha-256 of the bytes). A script
// that is new, or whose content changed since approval, is BLOCKED — fail closed
// — so a teammate-shared or silently-dropped script cannot run the app's
// high-privilege Lua bindings (tracker mutations, config, provided HTTP) on the
// next automation job / MCP call / startup hook replay without the user seeing it.
//
// This header holds ONLY pure, side-effect-free logic (no file I/O, no config
// save, no threads) so it is unit-testable off-target and reusable from every
// execution site. The AppController owns the I/O: reading the script bytes,
// hashing them (Sha256Hex), consulting the persisted approval list, seeding the
// list once on first upgrade, and refusing / prompting per context.

#include <string>
#include <vector>

#include "Sha256.h"

namespace smatchet {
namespace lua_consent {

// One persisted approval: the resolved-or-relative script path plus the sha-256
// (lowercase hex) of the exact bytes that were approved. Persisted in
// TrackerConfig::ApprovedLuaScripts.
struct LuaScriptApproval {
    std::string Path;
    std::string Sha256;
};

// Fingerprint the script bytes. The same bytes that will be handed to the Lua
// loader must be hashed (hash-then-load on one read) so the approval can't be
// bypassed by a time-of-check/time-of-use swap between hashing and execution.
inline std::string FingerprintLuaScript(const std::string& scriptBytes) {
    return smatchet::hashing::Sha256Hex(scriptBytes);
}

// True iff `path` is approved with exactly `sha256`. A path present with a
// DIFFERENT hash is NOT approved (content changed → must be re-approved).
inline bool IsLuaScriptApproved(const std::vector<LuaScriptApproval>& approvals, const std::string& path,
                                const std::string& sha256) {
    for (const LuaScriptApproval& a : approvals) {
        if (a.Path == path) {
            return a.Sha256 == sha256;
        }
    }
    return false;
}

enum class LuaConsentVerdict {
    Approved, // execution may proceed
    Blocked   // execution must be refused (fail closed)
};

// The gate decision. When enforcement is off (operator kill-switch) everything is
// Approved. Otherwise a script is Approved only if its current content matches a
// stored approval. Pure — the caller performs the read/hash and reacts to the
// verdict (refuse + log in non-interactive contexts; refuse + surface an approval
// affordance in interactive ones).
inline LuaConsentVerdict DecideLuaConsent(bool enforced, const std::vector<LuaScriptApproval>& approvals,
                                          const std::string& path, const std::string& sha256) {
    if (!enforced) {
        return LuaConsentVerdict::Approved;
    }
    return IsLuaScriptApproved(approvals, path, sha256) ? LuaConsentVerdict::Approved : LuaConsentVerdict::Blocked;
}

// Insert/replace the approval for `path` with `sha256`, returning the updated
// list. A prior entry for the same path (e.g. an earlier content version) is
// replaced, so the list holds at most one fingerprint per path. Pure — the caller
// persists the result to config.
inline std::vector<LuaScriptApproval> WithApproval(std::vector<LuaScriptApproval> approvals, const std::string& path,
                                                   const std::string& sha256) {
    for (LuaScriptApproval& a : approvals) {
        if (a.Path == path) {
            a.Sha256 = sha256;
            return approvals;
        }
    }
    LuaScriptApproval added;
    added.Path = path;
    added.Sha256 = sha256;
    approvals.push_back(added);
    return approvals;
}

// Remove any approval for `path`, returning the updated list. Pure.
inline std::vector<LuaScriptApproval> WithoutApproval(std::vector<LuaScriptApproval> approvals,
                                                      const std::string& path) {
    std::vector<LuaScriptApproval> out;
    out.reserve(approvals.size());
    for (LuaScriptApproval& a : approvals) {
        if (a.Path != path) {
            out.push_back(a);
        }
    }
    return out;
}

} // namespace lua_consent
} // namespace smatchet
