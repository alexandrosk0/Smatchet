#include "AiErrorRedact.h"

#include <cctype>
#include <cstring>

namespace {

// Case-insensitive ASCII substring search. Returns the index of the first match at
// or after `from`, or std::string::npos. Used for credential prefixes / header names
// a proxy may echo with arbitrary casing (issue #1286 — the `sk-` / `x-api-key`
// sweeps must not be defeated by an upper-cased reflection).
std::size_t FindCaseInsensitive(const std::string& hay, const char* needle, std::size_t from) {
    const std::size_t nl = std::strlen(needle);
    if (nl == 0) {
        return from <= hay.size() ? from : std::string::npos;
    }
    if (hay.size() < nl) {
        return std::string::npos;
    }
    for (std::size_t i = from; i + nl <= hay.size(); ++i) {
        std::size_t k = 0;
        for (; k < nl; ++k) {
            if (std::tolower(static_cast<unsigned char>(hay[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) {
                break;
            }
        }
        if (k == nl) {
            return i;
        }
    }
    return std::string::npos;
}

} // namespace

namespace smatchet {
namespace ai {
namespace pure {

std::string RedactProviderErrorBody(const std::string& body) {
    std::string s = body;

    // HTTP auth-scheme credentials: "Bearer <token>" / "Basic <b64>" up to the next
    // whitespace / quote / comma / brace. The scheme word is preserved so a redacted
    // line still reads "Bearer [REDACTED]"; only the credential is replaced. Covers a
    // reflected `Authorization: Bearer …` / `Authorization: Basic …` header echo
    // whether the body is a raw header line or a JSON string value. Basic is handled
    // explicitly (issue #1286) because a base64 Basic-auth credential carries none of
    // the `sk-` / Bearer shapes the other sweeps key on.
    auto redactAuthScheme = [&](const char* scheme) {
        const std::string needle = std::string(scheme) + " ";
        size_t i = 0;
        while ((i = s.find(needle, i)) != std::string::npos) {
            const size_t valStart = i + needle.size();
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
    };
    redactAuthScheme("Bearer");
    redactAuthScheme("Basic");

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
    // GitHub PAT — Bundle B SH3. Cover both the snake_case config-field name
    // and the camelCase variant some error bodies echo. The header form
    // ("X-GitHub-...") never carries the token verbatim; the value is what
    // matters and the `gh*_` prefix sweep below catches the literal token
    // shape regardless of surrounding key.
    redactJsonField("github_pat");
    redactJsonField("githubPat");
    redactJsonField("GitHubPat");

    // Common id prefixes:
    //   OpenAI:  sk-..., sk_..., org-..., proj_..., asst_...
    //   GitHub:  ghp_..., gho_..., ghs_..., ghu_..., ghr_... (PAT / OAuth /
    //           server / user-to-server / refresh tokens — Bundle B SH3).
    //           github_pat_ also handled below as a longer literal.
    static const char* kIdPrefixes[] = {"sk-",  "sk_",         "org-", "proj_", "asst_",
                                        "ghp_", "gho_",        "ghs_", "ghu_",  "ghr_",
                                        "github_pat_"};
    for (const char* prefix : kIdPrefixes) {
        size_t i = 0;
        const size_t pl = std::strlen(prefix);
        // Case-insensitive: a proxy/upstream may echo the token with its prefix
        // upper-cased (issue #1286); the original casing is left in place (only the
        // suffix is replaced), so the redacted form still reads e.g. "SK-[REDACTED]".
        while ((i = FindCaseInsensitive(s, prefix, i)) != std::string::npos) {
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

    // Raw (unquoted) API-key header-line echoes: "x-api-key: <key>",
    // "anthropic-api-key: <key>", "api-key: <key>" (case-insensitive header name).
    // The JSON field scrubber above only catches the quoted "field":"value" form, and
    // the id-prefix sweep only catches keys with a known shape (sk-…). An explicit
    // header-line scrubber redacts the value REGARDLESS of prefix, so a provider key
    // rotation can't reopen the leak (issue #1286). We match the header name followed
    // directly by optional space + ':' (the quoted form has a '"' there, so it is not
    // redacted twice) and replace the value token up to the next whitespace. A single
    // "api-key" pass covers x-api-key / anthropic-api-key too, since both end in it.
    {
        const char* kApiKeyHeader = "api-key";
        const std::size_t hl = std::strlen(kApiKeyHeader);
        std::size_t i = 0;
        while ((i = FindCaseInsensitive(s, kApiKeyHeader, i)) != std::string::npos) {
            std::size_t k = i + hl;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) {
                ++k;
            }
            if (k >= s.size() || s[k] != ':') {
                i += hl; // not a header line ("api-key" not followed by ':'); skip past it
                continue;
            }
            ++k; // past ':'
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) {
                ++k;
            }
            const std::size_t valStart = k;
            std::size_t valEnd = valStart;
            while (valEnd < s.size() && !std::isspace(static_cast<unsigned char>(s[valEnd])) && s[valEnd] != '"' &&
                   s[valEnd] != ',' && s[valEnd] != '}') {
                ++valEnd;
            }
            if (valEnd > valStart) {
                s.replace(valStart, valEnd - valStart, "[REDACTED]");
                i = valStart + 10;
            } else {
                i = valStart; // empty value; valStart is past the colon so this advances
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
