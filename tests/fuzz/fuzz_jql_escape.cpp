// libFuzzer driver for the JQL literal escaper — tracker_jql::QuoteLiteral
// (JqlEscape.cpp). Every value crossing a foreign trust boundary (a
// tracker-server-supplied issue key, accountId, or display name) is escaped here
// before it is spliced into a double-quoted JQL literal; a missed `"` lets the
// server rewrite the query (security H3 + E1). The function takes a RAW string, so
// the fuzz bytes go straight in — no pre-parse. Bug classes (OOB / UB) surface via
// ASan+UBSan under the fuzzer preset. nightly-monkey-tester Layer 3.
#include "Tracker/JqlEscape.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string value(reinterpret_cast<const char*>(data), size);
    (void)tracker_jql::QuoteLiteral(value);
    return 0;
}
