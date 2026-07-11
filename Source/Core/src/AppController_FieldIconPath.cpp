// AppController_FieldIconPath.cpp — field-icon asset-path resolver extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/active/appcontroller-clusters-followup.md). The method DECLARATION stays in
// AppController.h; only the definition and its two file-local helpers moved, so linkage
// and behavior are identical. The two helpers are used exclusively by this resolver, so
// they move with it in a fresh anonymous namespace. This TU never touches the pImpl, so it
// does not need the companion-TU subsystem superset.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController field-icon path resolver needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "ConfigManager.h"
#include "StringUtil.h"

#include <ghc/filesystem.hpp>

#include <string>
#include <system_error>

namespace {

bool FieldIconHasCaseInsensitivePrefix(const std::string& value, const std::string& prefix) {

    if (prefix.empty() || value.size() < prefix.size()) {

        return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i) {

        unsigned char a = static_cast<unsigned char>(value[i]);

        unsigned char b = static_cast<unsigned char>(prefix[i]);

        if (a >= 'A' && a <= 'Z') {

            a = static_cast<unsigned char>(a - 'A' + 'a');
        }

        if (b >= 'A' && b <= 'Z') {

            b = static_cast<unsigned char>(b - 'A' + 'a');
        }

        if (a != b) {

            return false;
        }
    }

    if (value.size() == prefix.size()) {

        return true;
    }

    const char next = value[prefix.size()];

    return next == '/' || next == '\\';
}

// True when absStr is inside the Lua scripts directory or the runtime-asset directory.
// Both roots are weakly-canonicalised before the case-insensitive prefix compare.
bool FieldIconPathIsAllowed(const std::string& absStr, const std::string& luaScriptsDirectory) {
    namespace fs = ghc::filesystem;
    std::error_code ec;

    if (!luaScriptsDirectory.empty()) {

        const fs::path scriptsRoot = fs::weakly_canonical(fs::path(luaScriptsDirectory), ec);

        if (!ec && FieldIconHasCaseInsensitivePrefix(absStr, scriptsRoot.string())) {

            return true;
        }

        ec.clear();
    }

    const std::string base = ConfigManager::GetRuntimeAssetDirectory();

    if (!base.empty()) {

        const fs::path baseRoot = fs::weakly_canonical(fs::path(base), ec);

        if (!ec && FieldIconHasCaseInsensitivePrefix(absStr, baseRoot.string())) {

            return true;
        }

        ec.clear();
    }

    return false;
}

} // namespace

std::string AppController::ResolveFieldIconAssetPath(const std::string& pathOrUrl) const {

    namespace fs = ghc::filesystem;

    {
        auto cit = fieldIconAssetPathCache_.find(pathOrUrl);
        if (cit != fieldIconAssetPathCache_.end()) {
            return cit->second;
        }
    }

    const std::string t = TrimCopyAsciiWhitespace(pathOrUrl);

    if (t.empty()) {

        return std::string();
    }

    if (t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0) {

        if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
            fieldIconAssetPathCache_.clear();
        }
        fieldIconAssetPathCache_.emplace(pathOrUrl, t);
        return t;
    }

    std::error_code ec;

    auto isAllowedPath = [&](const fs::path& absPath) -> bool {
        return FieldIconPathIsAllowed(absPath.string(), luaScriptsDirectory_);
    };

    fs::path inp(t);

    if (!inp.is_absolute()) {

        if (luaScriptsDirectory_.empty()) {

            return std::string();
        }

        std::string rel = t;

        if (rel.size() >= 7 && FieldIconHasCaseInsensitivePrefix(rel, "Scripts")) {

            rel = rel.size() == 7 ? std::string() : rel.substr(8);
        }

        const fs::path combined = fs::path(luaScriptsDirectory_) / fs::path(rel);

        const fs::path absRel = fs::weakly_canonical(combined, ec);

        std::string out;
        if (!ec && isAllowedPath(absRel)) {
            out = absRel.string();
        }
        if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
            fieldIconAssetPathCache_.clear();
        }
        fieldIconAssetPathCache_.emplace(pathOrUrl, out);
        return out;
    }

    const fs::path abs = fs::weakly_canonical(inp, ec);

    std::string out;
    if (!ec && isAllowedPath(abs)) {
        out = abs.string();
    }
    if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
        fieldIconAssetPathCache_.clear();
    }
    fieldIconAssetPathCache_.emplace(pathOrUrl, out);
    return out;
}
