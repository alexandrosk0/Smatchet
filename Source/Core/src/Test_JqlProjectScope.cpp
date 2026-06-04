// Compile-only self-test for JqlProjectScope. Not wired to a runner — the
// project has no test harness yet. The function below is never called; its
// purpose is to ensure the helper compiles and to document expected outputs
// inline. CI can wire it later.

#include "JqlProjectScope.h"

#include <cassert>
#include <string>

namespace {

bool RunJqlProjectScopeSelfTest_Unused() {
    using JqlProjectScope::ExtractSingleProject;
    using JqlProjectScope::HasProjectScope;

    bool ok = true;
    ok = ok && ExtractSingleProject("project = PROJ") == "PROJ";
    ok = ok && ExtractSingleProject("project = \"FOO-BAR\"") == "FOO-BAR";
    ok = ok && ExtractSingleProject("project in (A)") == "A";
    ok = ok && ExtractSingleProject("project in (A, B)") == "";
    ok = ok && ExtractSingleProject("assignee = currentUser()") == "";
    ok = ok && ExtractSingleProject("PROJECT = proj") == "proj";
    ok = ok && ExtractSingleProject("project=PROJ AND status=Open") == "PROJ";

    ok = ok && HasProjectScope("project = PROJ");
    ok = ok && HasProjectScope("project in (A, B)");
    ok = ok && !HasProjectScope("assignee = currentUser()");
    ok = ok && !HasProjectScope("");

    return ok;
}

// Reference the function from a never-evaluated static so the compiler must
// instantiate it but the linker can drop the call. Avoids "unused function"
// warnings under -Wunused-function.
struct ForceInstantiate {
    bool (*p)() = &RunJqlProjectScopeSelfTest_Unused;
};
// clang-cl defines _MSC_VER but still honours [[gnu::used]] and emits
// -Wunused-variable for the bare static; key on the attribute's availability
// (Clang/GCC) rather than on the MSVC macro.
#if defined(__clang__) || defined(__GNUC__)
[[gnu::used]] static ForceInstantiate s_forceInstantiate{};
#else
static ForceInstantiate s_forceInstantiate{};
#endif

} // namespace
