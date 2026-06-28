#include "Commands/Scenarios/SpawnOutLogBasename.h"

#include <ghc/filesystem.hpp>

#include <cstdio>
#include <random>

namespace fs = ghc::filesystem;

namespace smatchet {
namespace cmd {

namespace {

/// 16 lowercase hex chars (64 bits) drawn from std::random_device. random_device is
/// non-deterministic on every supported host and each draw yields 32 bits. Two draws give
/// enough entropy that successive/concurrent spawns sharing the confinement base never collide
/// (a coarse millisecond timestamp collides when two spawns start in the same tick).
std::string RandomHexSuffix() {
    std::random_device rd;
    char buf[9];
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 2; ++i) {
        std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned int>(rd()));
        out += buf;
    }
    return out;
}

} // namespace

std::string MakeConfineSafeSpawnOutLogBasename(const std::string& requested) {
    // filename() strips every directory component, so the leaf can never carry a path
    // separator or a `..` traversal segment. An empty / `.` / `..` leaf has no usable name.
    std::string leaf = fs::path(requested).filename().string();
    if (leaf.empty() || leaf == "." || leaf == "..")
        leaf = "outlog.txt";
    return "spawn-" + RandomHexSuffix() + "-" + leaf;
}

} // namespace cmd
} // namespace smatchet
