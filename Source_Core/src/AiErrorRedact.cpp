#include "AiErrorRedact.h"

#include <cctype>
#include <cstring>

namespace smatchet {
namespace ai {
namespace pure {

std::string RedactProviderErrorBody(const std::string& body) {
    std::string s = body;

    // Bearer tokens (matches "Bearer <token>" up to next whitespace / quote / comma / brace).
    {
        size_t i = 0;
        while ((i = s.find("Bearer ", i)) != std::string::npos) {
            const size_t valStart = i + 7;
            size_t valEnd = valStart;
            while (valEnd < s.size() && !std::isspace(static_cast<unsigned char>(s[valEnd])) && s[valEnd] != '"' &&
                   s[valEnd] != ',' && s[valEnd] != '}') {
                ++valEnd;
            }
            if (valEnd > valStart) {
                s.replace(valStart, valEnd - valStart, "[REDACTED]");
                i = valStart + 10;
            } else {
                i = valStart;
            }
        }
    }

    // JSON-style "<field>":"<value>" — conservative, single-level only.
    auto redactJsonField = [&](const std::string& field) {
        size_t i = 0;
        const std::string needle = std::string("\"") + field + "\"";
        while ((i = s.find(needle, i)) != std::string::npos) {
            size_t j = i + needle.size();
            while (j < s.size() && (s[j] == ' ' || s[j] == ':' || s[j] == '\t'))
                ++j;
            if (j < s.size() && s[j] == '"') {
                const size_t valStart = j + 1;
                size_t valEnd = valStart;
                while (valEnd < s.size() && s[valEnd] != '"') {
                    if (s[valEnd] == '\\' && valEnd + 1 < s.size())
                        ++valEnd;
                    ++valEnd;
                }
                if (valEnd > valStart) {
                    s.replace(valStart, valEnd - valStart, "[REDACTED]");
                    i = valStart + 10;
                    continue;
                }
            }
            i += needle.size();
        }
    };
    redactJsonField("api_key");
    redactJsonField("apiKey");
    redactJsonField("Authorization");
    redactJsonField("authorization");
    redactJsonField("x-api-key");
    redactJsonField("X-Api-Key");
    redactJsonField("anthropic-api-key");

    // Common id prefixes (OpenAI: sk-..., org-..., proj_..., asst_...).
    static const char* kIdPrefixes[] = {"sk-", "sk_", "org-", "proj_", "asst_"};
    for (const char* prefix : kIdPrefixes) {
        size_t i = 0;
        const size_t pl = std::strlen(prefix);
        while ((i = s.find(prefix, i)) != std::string::npos) {
            const size_t valStart = i + pl;
            size_t valEnd = valStart;
            while (valEnd < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[valEnd])) || s[valEnd] == '-' || s[valEnd] == '_')) {
                ++valEnd;
            }
            if (valEnd - valStart >= 8) {
                s.replace(valStart, valEnd - valStart, "[REDACTED]");
                i = valStart + 10;
            } else {
                i = valEnd;
            }
        }
    }

    if (s.size() > kMaxProviderErrorBodyChars) {
        s.resize(kMaxProviderErrorBodyChars);
        s.append("…");
    }
    return s;
}

} // namespace pure
} // namespace ai
} // namespace smatchet
