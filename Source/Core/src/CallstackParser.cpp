#include "CallstackParser.h"

// [asan-fix-smoke] Throwaway no-op touch — flips source_core_cpp=true so the
// Slice C ASAN/UBSan jobs run their fixed ctest steps on this PR. NEVER merged;
// this branch is closed once the sanitizer jobs report green.

#include <algorithm>
#include <cctype>
#include <regex>

namespace {

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

bool TryParsePathLinePair(const std::string& line, std::string& outPath, int& outLine, std::string& outFunction) {
    outPath.clear();
    outLine = 0;
    outFunction.clear();

    // MSVC / Unreal: ...\File.cpp(123) or File.cpp(123)
    {
        static const std::regex re(
            R"(([A-Za-z]:[^()\r\n]*|\S(?:[^\s:()]*[/\\])?[^\s:()]*\.(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m))\((\d+)\))",
            std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            outPath = Trim(m[1].str());
            try {
                outLine = std::stoi(m[2].str());
            } catch (...) { // catch-all-ok: stoi on untrusted callstack line
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

    // path:line[:column] (Clang, GDB "at /path/foo.cpp:42", Rust backtraces)
    {
        static const std::regex re(
            R"(([A-Za-z]:[^:\r\n]+|(?:/|\.\.?/|\S[/\\])[^\s:]+\.(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m)):(\d+)(?::\d+)?)",
            std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            outPath = Trim(m[1].str());
            try {
                outLine = std::stoi(m[2].str());
            } catch (...) { // catch-all-ok: stoi on untrusted callstack line
                return false;
            }
            // Optional "at Function " prefix
            static const std::regex atFn(R"(at\s+([^\s]+)\s+)", std::regex::icase);
            std::smatch mf;
            if (std::regex_search(line, mf, atFn)) {
                outFunction = Trim(mf[1].str());
            }
            TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
            return true;
        }
    }

    // Unreal-style: [File:Line] or File:Line in brackets
    {
        static const std::regex re(R"(\[?\s*([A-Za-z]:[^\]\r\n]+|(?:/|\S[/\\])[^\]\r\n]+):(\d+)\s*\]?)",
                                   std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string p = Trim(m[1].str());
            if (!p.empty() && (p.find('.') != std::string::npos || p.find('/') != std::string::npos ||
                               p.find('\\') != std::string::npos)) {
                outPath = p;
                try {
                    outLine = std::stoi(m[2].str());
                } catch (...) {
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
