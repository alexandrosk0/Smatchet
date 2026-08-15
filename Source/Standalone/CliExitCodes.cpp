// CliExitCodes.cpp — the unified-CLI error-code -> process-exit-code mapping. Split out of
// CliArgs.cpp into its own tiny TU so the exit-code contract (which scripts and CI lanes depend on)
// is unit-tested directly, without linking the heavy CliArgs.cpp (httplib / winsock / command
// registry) into the doctest rig. See tests/Core/CliExitCodes.test.cpp.
//
// Fixes GitHub Issue #1747 finding 1: the "transport" error code was unmapped, so transport
// failures fell through to kExitHandler (4) instead of returning kExitTransport (7). "timeout" now
// uses the kExitTimeout constant rather than a bare literal.

#include "CliCommandRunner_Internal.h"

#include <string>

namespace smatchet {
namespace cli {
namespace detail {

int ExitCodeForErrorCode(const std::string& code) {
    if (code == "ok")
        return kExitOk;
    if (code == "unknown-command")
        return kExitUnknownCommand;
    if (code == "missing-required-arg")
        return kExitValidation;
    if (code == "validation-error")
        return kExitValidation;
    if (code == "handler-error")
        return kExitHandler;
    if (code == "backend-error")
        return kExitHandler;
    if (code == "not-found")
        return kExitHandler;
    if (code == "confirm-required")
        return kExitConfirmRequired;
    if (code == "not-connected")
        return kExitNotConnected;
    if (code == "transport")
        return kExitTransport;
    if (code == "dry-run-unsupported")
        return 9; // no kExit* constant yet — reachable once dry-run lands (see the header note)
    if (code == "timeout")
        return kExitTimeout;
    if (code == "child-died")
        return kExitHandler; // --spawn child exited before writing its result (a crash, not a timeout)
    return kExitHandler;
}

} // namespace detail
} // namespace cli
} // namespace smatchet
