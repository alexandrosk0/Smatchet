// libFuzzer driver for the AI-provider error-body redactor —
// smatchet::ai::pure::RedactProviderErrorBody (AiErrorRedact.cpp). Provider 4xx
// bodies have been observed echoing the Authorization header, request body, and
// org/project ids; this strips secrets before the text reaches a UI error strip or
// a log line (and it also redacts nlohmann parse-error text, which embeds the
// offending input window — #1286). It consumes a RAW untrusted body string, so the
// fuzz bytes go straight in. Bug classes (OOB / UB in the regex-free char scanner)
// surface via ASan+UBSan. nightly-monkey-tester Layer 3.
#include "AiErrorRedact.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string body(reinterpret_cast<const char*>(data), size);
    (void)smatchet::ai::pure::RedactProviderErrorBody(body);
    return 0;
}
