// libFuzzer driver for the C++ / callstack syntax lexers (CppLexLine and the
// callstack-detection pair) that highlight p4-annotate views over untrusted
// pasted/annotated source text. Slice E2b, testing-surface.md §6 P1 Gap 3 /
// security.md §59-60.
//
// libFuzzer drives this via its own main() (linked by -fsanitize=fuzzer). The
// lexers are total — they walk a (char*, len) byte range and must never read
// OOB or trigger UB on ANY input; ASan+UBSan (in the fuzzer sanitizer set)
// catch violations. We also exercise the callstack-detect + callstack lexer on
// the same bytes when the heuristic fires, covering that branch.
#include "Ui/CppSyntaxLex.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* line = reinterpret_cast<const char*>(data);
    (void)CppLexLine(line, size);
    if (CppLexLooksLikeCallstack(line, size)) {
        (void)CppLexCallstackLine(line, size);
    }
    return 0;
}
