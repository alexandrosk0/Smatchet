// CliExitCodes.test.cpp — the unified-CLI error-code -> process-exit-code contract
// (Source/Standalone/CliExitCodes.cpp). Scripts and CI lanes branch on these codes, so a
// mis-mapping is a silent behavioural break. Regression coverage for GitHub Issue #1747 finding 1:
// the "transport" code was unmapped and fell through to kExitHandler (4) instead of kExitTransport.

#include "CliCommandRunner_Internal.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::cli::detail::ExitCodeForErrorCode;
using smatchet::cli::detail::kExitConfirmRequired;
using smatchet::cli::detail::kExitHandler;
using smatchet::cli::detail::kExitNotConnected;
using smatchet::cli::detail::kExitOk;
using smatchet::cli::detail::kExitTimeout;
using smatchet::cli::detail::kExitTransport;
using smatchet::cli::detail::kExitUnknownCommand;
using smatchet::cli::detail::kExitValidation;

TEST_CASE("ExitCodeForErrorCode maps transport failures to kExitTransport (Issue #1747)") {
    // Previously unmapped -> fell through to kExitHandler; a --spawn transport failure must exit 7.
    CHECK(ExitCodeForErrorCode("transport") == kExitTransport);
}

TEST_CASE("ExitCodeForErrorCode maps every known code to its exit constant") {
    CHECK(ExitCodeForErrorCode("ok") == kExitOk);
    CHECK(ExitCodeForErrorCode("unknown-command") == kExitUnknownCommand);
    CHECK(ExitCodeForErrorCode("missing-required-arg") == kExitValidation);
    CHECK(ExitCodeForErrorCode("validation-error") == kExitValidation);
    CHECK(ExitCodeForErrorCode("handler-error") == kExitHandler);
    CHECK(ExitCodeForErrorCode("backend-error") == kExitHandler);
    CHECK(ExitCodeForErrorCode("not-found") == kExitHandler);
    CHECK(ExitCodeForErrorCode("confirm-required") == kExitConfirmRequired);
    CHECK(ExitCodeForErrorCode("not-connected") == kExitNotConnected);
    CHECK(ExitCodeForErrorCode("transport") == kExitTransport);
    CHECK(ExitCodeForErrorCode("timeout") == kExitTimeout);
    CHECK(ExitCodeForErrorCode("child-died") == kExitHandler); // --spawn child crashed before its result landed
    CHECK(ExitCodeForErrorCode("dry-run-unsupported") == 9);   // no kExit* constant yet (see the mapper)
}

TEST_CASE("ExitCodeForErrorCode falls back to kExitHandler for unknown / empty codes") {
    CHECK(ExitCodeForErrorCode("some-future-code") == kExitHandler);
    CHECK(ExitCodeForErrorCode("") == kExitHandler);
}
