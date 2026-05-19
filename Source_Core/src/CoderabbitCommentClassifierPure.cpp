// CoderabbitCommentClassifierPure.cpp — phase-2 of the `coderabbit-react-loop`
// plan. Pure C++14 helpers backing the abstract `PrCommentClassifier`
// interface (Source_Core/include/PrCommentClassifier.h) ahead of the phase-3
// concrete `CoderabbitCommentClassifier`.
//
// No third-party / banned-dep includes — this TU compiles into both
// SmatchetStandalone (OpenGL) and SmatchetCore_DX12 (Unreal) under the
// `#if defined(SMATCHET_WITH_AGENTIC)` gate, and the doctest rig links
// it directly.

#include "CoderabbitCommentClassifierPure.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {
namespace coderabbit_pure {

namespace {

// Canonical CodeRabbit severity icons, encoded as UTF-8 byte sequences so
// source-encoding portability (MinGW UCRT vs MSVC) does not bite. The
// codepoints are:
//   Actionable  U+1F6E0 (wrench+screwdriver) + U+FE0F (VS-16)
//   Caution     U+26A0  (warning sign)       + U+FE0F (VS-16)
//   Nit         U+1F4A1 (light bulb)
//   Chore       U+1F9F9 (broom)
//
// We scan for the bare codepoint without VS-16 — that way the body may or
// may not carry the variation selector (GitHub strips it on some clients)
// and we still match. The VS-16 is purely a font-rendering hint and the
// CodeRabbit body shapes have been observed both with and without it.
const char* const kIconActionable = "\xF0\x9F\x9B\xA0"; // U+1F6E0
const char* const kIconCaution = "\xE2\x9A\xA0";        // U+26A0
const char* const kIconNit = "\xF0\x9F\x92\xA1";        // U+1F4A1
const char* const kIconChore = "\xF0\x9F\xA7\xB9";      // U+1F9F9

// `<!-- smatchet-handoff -->` marker — kept here as a single literal so the
// helpers stay self-contained. `AgenticHandoffController::HandoffBotMarker()`
// returns the same literal; the duplicate is intentional (this TU must not
// transitively pull in the controller header in order to detect it).
const char* const kSmatchetHandoffMarker = "<!-- smatchet-handoff -->";

char AsciiToLower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

std::string AsciiLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(AsciiToLower(c));
    }
    return out;
}

// Parse an integer that starts at `body[start]` and runs as long as digits
// continue. Returns the parsed value (>=0) on success or -1 when the first
// char is not a digit. `outEnd` receives the index just past the last digit
// consumed (== start when no digits were found).
int ParseUintRun(const std::string& body, std::size_t start, std::size_t& outEnd) {
    outEnd = start;
    if (start >= body.size()) {
        return -1;
    }
    if (body[start] < '0' || body[start] > '9') {
        return -1;
    }
    long value = 0;
    std::size_t i = start;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        const long digit = body[i] - '0';
        // Pre-check guards signed-overflow UB before the multiply/add — the
        // defensive cap is 1,000,000, so the next iteration value would be
        // `value * 10 + digit`; reject when that would exceed the cap.
        if (value > (1000000L - digit) / 10L) {
            return -1;
        }
        value = value * 10L + digit;
        ++i;
    }
    outEnd = i;
    return static_cast<int>(value);
}

// Strip a single trailing `[bot]` suffix from a lowercased login, leaving the
// base name. Returns the input unchanged when the suffix is absent.
std::string StripBotSuffix(const std::string& lowered) {
    const std::string suffix = "[bot]";
    if (lowered.size() >= suffix.size() &&
        lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return lowered.substr(0, lowered.size() - suffix.size());
    }
    return lowered;
}

} // namespace

SeverityIcon ExtractSeverityIcon(const std::string& body) {
    if (body.empty()) {
        return SeverityIcon::None;
    }

    // Find the first occurrence of any icon. Ordering matters for the
    // first-match-wins rule promised in the header — we walk the body once
    // and record the lowest position; that scales to N icons without N
    // string scans across the whole body.
    struct IconFind {
        SeverityIcon icon;
        std::size_t pos;
    };
    IconFind finds[4] = {
        {SeverityIcon::Actionable, body.find(kIconActionable)},
        {SeverityIcon::Caution, body.find(kIconCaution)},
        {SeverityIcon::Nit, body.find(kIconNit)},
        {SeverityIcon::Chore, body.find(kIconChore)},
    };

    std::size_t bestPos = std::string::npos;
    SeverityIcon bestIcon = SeverityIcon::None;
    for (const IconFind& f : finds) {
        if (f.pos != std::string::npos && f.pos < bestPos) {
            bestPos = f.pos;
            bestIcon = f.icon;
        }
    }
    return bestIcon;
}

int ExtractActionableHeaderCount(const std::string& body) {
    // Look for the literal `_Actionable comments posted: ` substring, then
    // parse the integer that follows. The closing `_` is informational —
    // the integer-run terminator is "first non-digit", which also handles
    // bodies where CodeRabbit's formatting drops the trailing marker.
    const std::string needle = "_Actionable comments posted: ";
    std::size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return -1;
    }
    std::size_t numberStart = pos + needle.size();
    std::size_t numberEnd = numberStart;
    int parsed = ParseUintRun(body, numberStart, numberEnd);
    return parsed;
}

int ExtractNitpickHeaderCount(const std::string& body) {
    const std::string needle = "_Nitpick comments (";
    std::size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return -1;
    }
    std::size_t numberStart = pos + needle.size();
    std::size_t numberEnd = numberStart;
    int parsed = ParseUintRun(body, numberStart, numberEnd);
    if (parsed < 0) {
        return -1;
    }
    // Defensive: require a `)` immediately after the number-run to confirm
    // this is the genuine header shape and not a noise hit.
    if (numberEnd >= body.size() || body[numberEnd] != ')') {
        return -1;
    }
    return parsed;
}

std::vector<std::string> ExtractSuggestionBlocks(const std::string& body) {
    std::vector<std::string> out;
    if (body.empty()) {
        return out;
    }

    // Walk the body looking for ```suggestion (optionally followed by a
    // newline or whitespace) and pair it with the next ``` fence closer.
    const std::string opener = "```suggestion";
    const std::string closer = "```";
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        std::size_t openPos = body.find(opener, cursor);
        if (openPos == std::string::npos) {
            break;
        }
        // Skip over the opener token + at most one trailing newline so the
        // suggestion-block body does not start with the fence-language line.
        std::size_t bodyStart = openPos + opener.size();
        // Advance past the rest of the opener line (could be `suggestion\n`
        // or `suggestion <something>\n` — we strip until newline).
        while (bodyStart < body.size() && body[bodyStart] != '\n') {
            ++bodyStart;
        }
        if (bodyStart < body.size() && body[bodyStart] == '\n') {
            ++bodyStart;
        }

        // Find the closing fence.
        std::size_t closePos = body.find(closer, bodyStart);
        if (closePos == std::string::npos) {
            // Malformed — opener without closer; per the header contract
            // we ignore this block and stop scanning (subsequent ``` markers
            // are inside an unclosed block from the parser's POV).
            break;
        }

        // Trim the trailing newline immediately before the closing fence so
        // callers see "foo" not "foo\n" when the closer was on its own line.
        std::size_t blockEnd = closePos;
        if (blockEnd > bodyStart && body[blockEnd - 1] == '\n') {
            --blockEnd;
        }

        if (blockEnd >= bodyStart) {
            out.push_back(body.substr(bodyStart, blockEnd - bodyStart));
        }
        cursor = closePos + closer.size();
    }
    return out;
}

bool IsBotLoginAllowed(const std::string& login, const std::vector<std::string>& allowList) {
    if (login.empty() || allowList.empty()) {
        return false;
    }
    // Exact-match-only with optional single `[bot]` suffix normalization on
    // both sides — substring matches accept spoofed logins
    // (e.g. `evil-coderabbitai` containing the configured `coderabbitai`).
    const std::string loginLower = AsciiLower(login);
    const std::string loginBase = StripBotSuffix(loginLower);
    for (const std::string& candidate : allowList) {
        if (candidate.empty()) {
            continue;
        }
        const std::string candidateLower = AsciiLower(candidate);
        if (candidateLower == loginLower) {
            return true;
        }
        // Allow the configured entry to be the base login (e.g.
        // `coderabbitai`) and the observed author the bracketed form
        // (`coderabbitai[bot]`), or vice versa. Compare base-against-base
        // after stripping at most one `[bot]` suffix from each side.
        const std::string candidateBase = StripBotSuffix(candidateLower);
        if (candidateBase == loginBase) {
            return true;
        }
    }
    return false;
}

bool HasSmatchetHandoffMarker(const std::string& body) {
    if (body.empty()) {
        return false;
    }
    return body.find(kSmatchetHandoffMarker) != std::string::npos;
}

} // namespace coderabbit_pure
} // namespace agentic
} // namespace smatchet
