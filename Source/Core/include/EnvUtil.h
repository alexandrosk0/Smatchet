#pragma once

// EnvUtil — one portable environment-variable read. MSVC deprecates getenv (C4996) and the
// warnings-as-errors build rejects it, so the Win32 path uses _dupenv_s; POSIX uses getenv.
// Header-only + inline so any TU can include it with no CMake wiring. Returns "" when the
// variable is unset (or on any read failure), never null.

#include <cstdlib>
#include <string>

namespace smatchet {
namespace env {

inline std::string ReadVar(const char* name) {
#if defined(_WIN32) && defined(_MSC_VER)
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name) != 0 || !buf) {
        if (buf) {
            std::free(buf);
        }
        return {};
    }
    std::string out(buf);
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

} // namespace env
} // namespace smatchet
