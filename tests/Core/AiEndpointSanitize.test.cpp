#include <doctest/doctest.h>

#include "AiEndpointSanitize.h"

using smatchet::ai::pure::EndpointVerdict;
using smatchet::ai::pure::EndpointVerdictDescription;
using smatchet::ai::pure::SanitizeAiEndpointUrl;

TEST_CASE("SanitizeAiEndpointUrl allows empty URL (use provider default)" * doctest::test_suite("[security]")) {
    std::string out = "non-empty";
    CHECK(SanitizeAiEndpointUrl("", out) == EndpointVerdict::Allowed);
    CHECK(out.empty());
}

TEST_CASE("SanitizeAiEndpointUrl allows https provider hosts" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com", out) == EndpointVerdict::Allowed);
    CHECK(out == "https://api.openai.com");
    CHECK(SanitizeAiEndpointUrl("https://api.anthropic.com/v1/messages", out) == EndpointVerdict::Allowed);
    CHECK(out == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("SanitizeAiEndpointUrl allows http loopback (ollama)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("http://localhost:11434", out) == EndpointVerdict::Allowed);
    CHECK(out == "http://localhost:11434");
    CHECK(SanitizeAiEndpointUrl("http://127.0.0.1:11434/v1", out) == EndpointVerdict::Allowed);
}

TEST_CASE("SanitizeAiEndpointUrl rejects non-http schemes" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("file:///etc/passwd", out) == EndpointVerdict::RejectedBadScheme);
    CHECK(out.empty());
    CHECK(SanitizeAiEndpointUrl("ftp://example.com", out) == EndpointVerdict::RejectedBadScheme);
    CHECK(SanitizeAiEndpointUrl("javascript:alert(1)", out) == EndpointVerdict::RejectedBadScheme);
    CHECK(SanitizeAiEndpointUrl("api.openai.com", out) == EndpointVerdict::RejectedBadScheme); // missing scheme
}

TEST_CASE("SanitizeAiEndpointUrl rejects CR/LF/NUL (header smuggling)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com\r\nX-Evil: yes", out) == EndpointVerdict::RejectedControlChars);
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com\nFoo", out) == EndpointVerdict::RejectedControlChars);
    std::string withNul = "https://api.openai.com";
    withNul.push_back('\0');
    withNul.append("/extra");
    CHECK(SanitizeAiEndpointUrl(withNul, out) == EndpointVerdict::RejectedControlChars);
}

TEST_CASE("SanitizeAiEndpointUrl rejects cloud-metadata IPs (SSRF)" * doctest::test_suite("[security]")) {
    std::string out;
    // AWS / GCP / Azure IMDS.
    CHECK(SanitizeAiEndpointUrl("http://169.254.169.254/latest/meta-data/", out) ==
          EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("https://169.254.169.254", out) == EndpointVerdict::RejectedCloudMetadata);
    // Alibaba.
    CHECK(SanitizeAiEndpointUrl("http://100.100.100.200/latest/meta-data/", out) ==
          EndpointVerdict::RejectedCloudMetadata);
}

TEST_CASE("SanitizeAiEndpointUrl rejects link-local 169.254/16 (SSRF)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("http://169.254.1.1", out) == EndpointVerdict::RejectedLinkLocal);
    CHECK(SanitizeAiEndpointUrl("http://169.254.254.254/path", out) == EndpointVerdict::RejectedLinkLocal);
    // Boundary: 169.254.169.254 is RejectedCloudMetadata, not LinkLocal, by precedence.
    CHECK(SanitizeAiEndpointUrl("http://169.254.169.254", out) == EndpointVerdict::RejectedCloudMetadata);
}

TEST_CASE("SanitizeAiEndpointUrl rejects malformed (missing host)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https:///path", out) == EndpointVerdict::RejectedMalformed);
    CHECK(SanitizeAiEndpointUrl("http:///", out) == EndpointVerdict::RejectedMalformed);
}

TEST_CASE("EndpointVerdictDescription returns non-empty for every value" * doctest::test_suite("[security]")) {
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::Allowed)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedBadScheme)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedControlChars)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedCloudMetadata)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedLinkLocal)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedMalformed)).size() > 0);
}
