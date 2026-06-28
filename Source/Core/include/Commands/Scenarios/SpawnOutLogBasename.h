#pragma once

#include <string>

namespace smatchet {
namespace cmd {

/// Derive a confinement-safe basename for a `--spawn` child's `outLog` from the caller's
/// requested path. The child's ui_test.run confines `outLog` under <userData>/ui-tests/
/// (ConfinePathUnderSubdir) and rejects absolute or `..`-bearing paths, so a trusted spawn
/// parent must forward only a leaf basename; the parent relocates the child's output back to
/// the caller's absolute target afterwards.
/// The result is GUARANTEED confine-safe: it contains no path separators and no `..`. The leaf
/// filename is extracted from @p requested (every directory component stripped); an empty / `.`
/// / `..` leaf falls back to "outlog.txt". The leaf is prefixed with "spawn-<entropy>-" where
/// <entropy> is a fresh random hex token, so successive/concurrent spawns sharing the confinement
/// base never clobber each other (entropy, not a coarse timestamp — millisecond stamps collide
/// when two spawns start in the same tick). Pure: no I/O, no logging; one random draw per call,
/// invoked once per spawn (not a steady-state path).
std::string MakeConfineSafeSpawnOutLogBasename(const std::string& requested);

} // namespace cmd
} // namespace smatchet
