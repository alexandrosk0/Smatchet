#include "CallstackParser.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <regex>

namespace {

// Parse a matched `([0-9]+)` digit string into an `int` line number, range-checking
// against INT_MAX explicitly rather than relying on std::stoi's overflow behaviour.
// The MSVC Debug CRT was observed to SATURATE std::stoi("2147483648") to INT_MAX
// instead of throwing std::out_of_range, so a line one past INT_MAX would leak
// through as LineNumber==INT_MAX. std::stoll holds every value the regex can match
// (a pure digit run is non-negative and far below LLONG_MAX for any realistic frame),
// and we reject anything exceeding INT_MAX. Returns false (caller drops the frame)
// on overflow or any parse failure — toolchain-independent, no exceptions-as-flow.
bool ParseLineNumberInRange(const std::string& digits, int& outLine) {
    try {
        const long long v = std::stoll(digits);
        if (v < 0 || v > static_cast<long long>(INT_MAX)) {
            return false;
        }
        outLine = static_cast<int>(v);
        return true;
    } catch (...) { // catch-all-ok: stoll on untrusted callstack line (e.g. > LLONG_MAX)
        return false;
    }
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

void TryExtractUnrealOrModuleFunctionPrefix(const std::string& line, const std::string& path,
                                            std::string& outFunction) {
    if (path.empty() || !outFunction.empty()) {
        return;
    }
    const size_t pos = line.find(path);
    if (pos == std::string::npos || pos == 0) {
        return;
    }
    std::string prefix = line.substr(0, pos);
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) {
        prefix.pop_back();
    }
    while (!prefix.empty() && prefix.back() == '[') {
        prefix.pop_back();
    }
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) {
        prefix.pop_back();
    }
    prefix = Trim(prefix);
    if (prefix.empty()) {
        return;
    }
    const size_t bang = prefix.find('!');
    if (bang != std::string::npos) {
        outFunction = Trim(prefix.substr(bang + 1));
    } else {
        outFunction = prefix;
    }
}

// True when `line` holds `lead` immediately followed by an ASCII digit. Every format below
// captures its line number as `[0-9]+` right after one fixed character — `(` for MSVC, `:` for
// the other two — so a line without that pair cannot match the format and its regex is not run
// at all. That keeps plain prose, and frames written in another format, out of the super-linear
// scans. The digit test is the same ASCII range the patterns use, so the two agree in any locale.
bool HasCharThenDigit(const std::string& line, char lead) {
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char next = line[i + 1];
        if (line[i] == lead && next >= '0' && next <= '9') {
            return true;
        }
    }
    return false;
}

// std::regex_search that reports a std::regex_error as "no match" instead of letting it escape.
// The matcher gives up on input it judges too expensive: the MSVC STL from toolset 14.51 on allows
// one search 300000 + 256 * length state transitions and throws error_complexity past that, and the
// format patterns below are super-linear on a line that does not match — an unbroken token of a
// few hundred characters is enough. Older STLs throw error_stack / error_complexity on their own
// limits.
bool RegexSearchNoThrow(const std::string& line, std::smatch& m, const std::regex& re) {
    try {
        return std::regex_search(line, m, re);
    } catch (const std::regex_error&) {
        // Config/parse tier: untrusted text the matcher refused is a line this format does not
        // match. The caller moves on to the next format; no frame is the default.
        return false;
    }
}

bool TryParsePathLinePair(const std::string& line, std::string& outPath, int& outLine, std::string& outFunction) {
    outPath.clear();
    outLine = 0;
    outFunction.clear();

    // MSVC / Unreal: ...\File.cpp(123) or File.cpp(123)
    if (HasCharThenDigit(line, '(')) {
        static const std::regex re(
            R"(([A-Za-z]:[^()\r\n]*|\S(?:[^\s:()]*[/\\])?[^\s:()]*\.(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m))\(([0-9]+)\))",
            std::regex::icase);
        std::smatch m;
        if (RegexSearchNoThrow(line, m, re)) {
            outPath = Trim(m[1].str());
            if (!ParseLineNumberInRange(m[2].str(), outLine)) {
                return false;
            }
            const size_t pos = line.find(m[0].str());
            if (pos != std::string::npos && pos > 0) {
                std::string fn = Trim(line.substr(0, pos));
                const size_t bang = fn.find('!');
                if (bang != std::string::npos) {
                    fn = Trim(fn.substr(bang + 1));
                }
                outFunction = fn;
            }
            TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
            return true;
        }
    }

    // Both remaining formats end in `:<line>`.
    if (!HasCharThenDigit(line, ':')) {
        return false;
    }

    // path:line[:column] (Clang, GDB "at /path/foo.cpp:42", Rust backtraces)
    {
        static const std::regex re(
            R"(([A-Za-z]:[^:\r\n]+|(?:/|\.\.?/|\S[/\\])[^\s:]+\.(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m)):([0-9]+)(?::[0-9]+)?)",
            std::regex::icase);
        std::smatch m;
        if (RegexSearchNoThrow(line, m, re)) {
            outPath = Trim(m[1].str());
            if (!ParseLineNumberInRange(m[2].str(), outLine)) {
                return false;
            }
            // Optional "at Function " prefix
            static const std::regex atFn(R"(at\s+([^\s]+)\s+)", std::regex::icase);
            std::smatch mf;
            if (RegexSearchNoThrow(line, mf, atFn)) {
                outFunction = Trim(mf[1].str());
            }
            TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
            return true;
        }
    }

    // Unreal-style: [File:Line] or File:Line in brackets
    {
        static const std::regex re(R"(\[?\s*([A-Za-z]:[^\]\r\n]+|(?:/|\S[/\\])[^\]\r\n]+):([0-9]+)\s*\]?)",
                                   std::regex::icase);
        std::smatch m;
        if (RegexSearchNoThrow(line, m, re)) {
            std::string p = Trim(m[1].str());
            if (!p.empty() && (p.find('.') != std::string::npos || p.find('/') != std::string::npos ||
                               p.find('\\') != std::string::npos)) {
                outPath = p;
                if (!ParseLineNumberInRange(m[2].str(), outLine)) {
                    return false;
                }
                TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
                return true;
            }
        }
    }

    return false;
}

// Per-line cap on input fed into TryParsePathLinePair. The three format regexes
// above (MSVC path(line), Clang path:line, Unreal [File:Line]) are super-linear
// in line length on libstdc++ / MinGW UCRT — empirical timings at -O2 against
// the MSVC alternation: 1 KiB ~21 ms, 2 KiB ~101 ms, 4 KiB ~403 ms; >~32 KiB
// stack-overflows the runner (0xC00000FD). Skip oversized lines wholesale so a
// malicious paste cannot DoS the parser. 16 KiB exceeds any realistic
// IDE-pasted frame; lines beyond it are almost certainly garbage or attacker-
// crafted.
static constexpr size_t kMaxLineLengthForRegex = 16384;

} // namespace

std::string ApplyPathRemaps(std::string path, const std::vector<PathRemapRule>& remaps) {
    size_t bestLen = 0;
    size_t bestIdx = remaps.size();
    for (size_t i = 0; i < remaps.size(); ++i) {
        const std::string& from = remaps[i].FromPrefix;
        if (from.empty()) {
            continue;
        }
        if (path.size() >= from.size() && path.compare(0, from.size(), from) == 0 && from.size() >= bestLen) {
            bestLen = from.size();
            bestIdx = i;
        }
    }
    if (bestIdx < remaps.size()) {
        path.replace(0, bestLen, remaps[bestIdx].ToPrefix);
    }
    return path;
}

bool FrameMatchesIgnoreKeywords(const ParsedCallstackFrame& frame, const std::vector<std::string>& keywords) {
    for (const std::string& kw : keywords) {
        if (kw.empty()) {
            continue;
        }
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        const std::string needle = lower(kw);
        const std::string hay = lower(frame.RawLine + " " + frame.Function + " " + frame.FilePath);
        if (hay.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<ParsedCallstackFrame> ParseCallstackText(const std::string& text) {
    std::vector<ParsedCallstackFrame> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        if (line.size() > kMaxLineLengthForRegex) {
            continue;
        }

        ParsedCallstackFrame f;
        f.RawLine = line;
        std::string path;
        int lineNum = 0;
        std::string fn;
        if (TryParsePathLinePair(line, path, lineNum, fn)) {
            f.FilePath = std::move(path);
            f.LineNumber = lineNum;
            f.Function = fn.empty() ? std::string("<unknown>") : std::move(fn);
            out.push_back(std::move(f));
        }
    }
    return out;
}
