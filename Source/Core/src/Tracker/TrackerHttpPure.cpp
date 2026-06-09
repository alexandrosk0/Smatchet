#include "TrackerHttpPure.h"

namespace TrackerHttpPure {

SslConfig ResolveSslConfig(const std::string& caBundlePath) {
    SslConfig out;
    if (caBundlePath.empty()) {
        return out; // {"", false} — keep libcurl's default CAINFO (desktop / system store).
    }
    out.caInfoPath = caBundlePath;
    out.attach = true;
    return out;
}

namespace {
// Set once at boot before the first tracker HTTP request, then read-only for the process
// lifetime. Function-local static so init order is well-defined across TUs; not synchronized
// (set-once-before-use contract — see header).
std::string& CaBundlePathStorage() {
    static std::string path;
    return path;
}
} // namespace

void SetCaBundlePath(const std::string& path) {
    CaBundlePathStorage() = path;
}

const std::string& GetCaBundlePath() {
    return CaBundlePathStorage();
}

} // namespace TrackerHttpPure
