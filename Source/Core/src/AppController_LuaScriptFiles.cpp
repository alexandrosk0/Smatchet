// AppController_LuaScriptFiles.cpp — Lua-script-file handling extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-clusters-followup.md). Method DECLARATIONS stay in
// AppController.h; only the definitions moved, so linkage and behavior are identical.
// The cluster is compiled unconditionally (it is not Lua-gated — script-path resolution
// and Scripts/ enumeration work in the no-Lua build too, matching the pre-move layout).
// Includes are curated from what the moved bodies actually use; this TU never touches
// the pImpl, so it does not need the companion-TU subsystem superset.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController Lua-script-file methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "ConfigManager.h" // ConfigManager::AtomicWriteTextFile — hardened temp-then-rename writer
#include "Logger.h"

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

std::string AppController::ResolveLuaScriptPath(const std::string& filename) const {

    if (filename.empty() || filename.find("..") != std::string::npos || filename.find(':') != std::string::npos ||

        (!filename.empty() && (filename[0] == '/' || filename[0] == '\\'))) {

        LOG_WARN("ResolveLuaScriptPath: blocked suspicious script path=%s", filename.c_str());

        return std::string();
    }

    if (!luaScriptsDirectory_.empty()) {

        return luaScriptsDirectory_ + filename;
    }

    // Fail closed when the configured scripts root is unset (GetRuntimeAssetDirectory empty).
    // The old cwd-relative "Scripts/" fallback meant launching Smatchet from an untrusted
    // working directory surfaced — and resolved for execution — whatever .lua files that
    // directory happened to carry.
    LOG_WARN("ResolveLuaScriptPath: scripts directory is not configured; refusing cwd-relative "
             "resolution of %s",
             filename.c_str());

    return std::string();
}

std::vector<std::string> AppController::ListLuaScriptFiles() const {

    namespace fs = ghc::filesystem;

    std::vector<std::string> out;

    try {

        std::error_code ec;

        if (luaScriptsDirectory_.empty()) {

            // Match ResolveLuaScriptPath: no configured scripts root -> no cwd-relative
            // enumeration (an untrusted working directory's .lua files must not appear in
            // the script picker).
            return out;
        }

        const fs::path root(luaScriptsDirectory_);

        if (!fs::is_directory(root, ec)) {

            return out;
        }

        fs::directory_iterator it(root, ec);
        if (ec) {
            LOG_WARN("ListLuaScriptFiles: failed to enumerate %s: %s", root.string().c_str(), ec.message().c_str());
            return out;
        }
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {

            if (ec) {

                break;
            }

            const auto& ent = *it;
            if (!ent.is_regular_file(ec)) {

                continue;
            }

            const std::string fname = ent.path().filename().string();

            if (fname.size() < 5) {

                continue;
            }

            std::string ext = fname.substr(fname.size() - 4);

            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (ext != ".lua") {

                continue;
            }

            out.push_back(fname);
        }

        std::sort(out.begin(), out.end());

        out.erase(std::unique(out.begin(), out.end()), out.end());

    } catch (const std::exception& ex) {

        LOG_WARN("ListLuaScriptFiles: exception (returning partial/empty): %s", ex.what());

        out.clear();

    } catch (...) {

        LOG_WARN("ListLuaScriptFiles: unknown exception (returning empty).");

        out.clear();
    }

    return out;
}

std::string AppController::GetAutomationScriptContent() {

    std::string path = ResolveLuaScriptPath("Automation.lua");

    if (path.empty())
        return "";

    std::ifstream ifs(path, std::ios::binary);

    if (!ifs.is_open())
        return "";

    // Byte-capped read (SECURITY_AUDIT #33 class): an oversized Automation.lua must not be
    // slurped fully into memory before any validation. 8 MiB dwarfs any real automation script.
    constexpr std::streamoff kMaxAutomationScriptBytes = 8ll * 1024 * 1024;
    ifs.seekg(0, std::ios::end);
    const std::streamoff size = ifs.tellg();
    if (size < 0 || size > kMaxAutomationScriptBytes) {
        LOG_WARN("GetAutomationScriptContent: refusing %s (size=%lld bytes, cap=%lld).", path.c_str(),
                 static_cast<long long>(size), static_cast<long long>(kMaxAutomationScriptBytes));
        return "";
    }
    ifs.seekg(0, std::ios::beg);

    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !ifs.read(&content[0], size)) {
        LOG_WARN("GetAutomationScriptContent: short read on %s.", path.c_str());
        return "";
    }

    // The pre-cap implementation opened in text mode (CRLF -> LF on Windows); the binary open
    // above makes the size cap exact, so normalize line endings here to keep the editor/Lua
    // round-trip identical.
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());

    return content;
}

bool AppController::SaveAutomationScriptContent(const std::string& content, std::string& outError) {

    std::string path = ResolveLuaScriptPath("Automation.lua");

    if (path.empty()) {

        outError = "Invalid path";

        return false;
    }

    // Atomic write via temp-then-rename so an I/O error mid-write cannot destroy the existing
    // Automation.lua. The previous std::ofstream(trunc) truncated the file up front, wrote with no
    // flush, and returned true without a stream-state check — a disk-full/quota/device error after
    // open silently lost the script while the caller was told the save succeeded (#1742). Reuses
    // ConfigManager's hardened writer (POSIX O_NOFOLLOW fd / Windows MoveFileEx, parent-dir fsync).
    if (!ConfigManager::AtomicWriteTextFile(path, content)) {

        outError = "Could not write file: " + path;

        return false;
    }

    return true;
}
