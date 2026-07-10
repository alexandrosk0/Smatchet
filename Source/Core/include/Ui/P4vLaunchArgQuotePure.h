#ifndef SMATCHET_P4V_LAUNCH_ARG_QUOTE_PURE_H
#define SMATCHET_P4V_LAUNCH_ARG_QUOTE_PURE_H

// Pure C++14, header-only, no platform headers. Quotes a single wide-char
// argv element so it round-trips through Windows' CommandLineToArgvW parser
// (the rule set ShellExecuteW / CreateProcessW apply to the lpParameters /
// command-line buffer). Header-only + std::wstring->std::wstring so the
// doctest rig can exercise the trailing-backslash + embedded-quote corner
// cases without a Windows toolchain or a spawned process.
// Implements the canonical Microsoft argument-quoting algorithm
// ("Everyone quotes command line arguments the wrong way", Daniel Colascione):
//   * A run of N backslashes followed by a literal '"' must emit 2N+1
//     backslashes then \" so the parser reads N literal backslashes + a
//     literal quote.
//   * A run of N backslashes immediately before the CLOSING wrap quote must
//     emit 2N backslashes — otherwise the parser pairs them with the wrap
//     and the argument boundary shifts (the trailing-backslash injection bug).
//   * Backslashes not adjacent to a quote pass through unchanged.

#include <string>

namespace P4vLaunch {

inline std::wstring QuoteWinArgWidePure(const std::wstring& arg) {
    // CPP_CODE_AUDIT.md #33 (empty argument dropped): an empty `arg` also has no
    // whitespace/quote, so it used to take the bare-token fast path below and return
    // "" (zero characters) — CommandLineToArgvW needs the literal two-character `""`
    // to preserve an empty argument as its own argv slot; returning nothing makes the
    // argument vanish when the caller joins args with spaces, silently shifting every
    // argument after it. Must be checked before the bare-token fast path.
    if (arg.empty()) {
        return L"\"\"";
    }
    // Bare token with no whitespace and no quote: paste verbatim. (A trailing
    // backslash here is harmless because there is no wrap quote to collide
    // with — CommandLineToArgvW treats unquoted backslashes literally.)
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.reserve(arg.size() + 4);
    out.push_back(L'"');
    size_t bsRun = 0; // consecutive backslashes pending emission
    for (size_t i = 0; i < arg.size(); ++i) {
        const wchar_t c = arg[i];
        if (c == L'\\') {
            ++bsRun;
            continue;
        }
        if (c == L'"') {
            // Double every preceding backslash, then escape the quote.
            out.append(bsRun * 2 + 1, L'\\');
            out.push_back(L'"');
            bsRun = 0;
            continue;
        }
        if (bsRun != 0) {
            out.append(bsRun, L'\\');
            bsRun = 0;
        }
        out.push_back(c);
    }
    // Backslashes immediately before the closing wrap quote must be doubled.
    out.append(bsRun * 2, L'\\');
    out.push_back(L'"');
    return out;
}

/// #1712 — a custom-command template ({file}/{cl}) substitutes the raw field value
/// into a command line whose quoting the TEMPLATE AUTHOR controls; the launcher
/// cannot re-quote per-arg there. Two field values corrupt the argument boundary
/// when a template wraps the placeholder like `"{file}"`:
///   * a value containing a double-quote closes the wrap quote and can inject new
///     args/flags; and
///   * a value ENDING IN A BACKSLASH escapes the template's closing wrap quote
///     (`"foo\"`), un-terminating the argument and swallowing following template
///     tokens — the exact trailing-backslash vector QuoteWinArgWidePure guards on
///     the helper path, but here the raw field (not the helper) is the vector.
/// Returns true when the raw field value must be rejected. Pure — doctest-covered.
inline bool P4vCustomCommandFieldRejected(const std::string& value) {
    if (value.find('"') != std::string::npos) {
        return true;
    }
    if (!value.empty() && value.back() == '\\') {
        return true;
    }
    return false;
}

} // namespace P4vLaunch

#endif // SMATCHET_P4V_LAUNCH_ARG_QUOTE_PURE_H
