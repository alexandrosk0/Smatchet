#include "CallstackParser.h"

#include "AsciiCharClass.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>

// The three frame formats are matched by hand-written scans, one per format, each reproducing the
// leftmost match of the regex quoted above it (tests/Core/CallstackParserScan.test.cpp runs the two
// side by side). std::regex_search tried every start position and ran a greedy class to the end of
// the token from each, so its cost grew faster than the line: on MSVC up to 14.50 a 1,000-character
// line stalled the UI thread for seconds, and from 14.51 on the matcher gave up on its complexity
// budget and a real frame later on the line was dropped (#2311). Here every candidate start is
// decided in constant time from per-line tables built in one pass each, so a line costs linear time.

namespace {

const size_t kNone = static_cast<size_t>(-1);

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

// True when `line` holds `lead` immediately followed by an ASCII digit. Every format below ends its
// path with one fixed character followed by the line number — `(` for MSVC, `:` for the other two —
// so a line without that pair cannot match the format and its tables are not built at all.
bool HasCharThenDigit(const std::string& line, char lead) {
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char next = line[i + 1];
        if (line[i] == lead && next >= '0' && next <= '9') {
            return true;
        }
    }
    return false;
}

// Character classes, in the classic locale the patterns were written for: `\s` is the six ASCII
// whitespace bytes; letters and digits are ASCII only (the patterns' ranges and `icase` agree).
namespace ascii = smatchet::ascii;
using ascii::IsSpace;
bool IsSlash(char c) { return c == '/' || c == '\\'; }

// `[^\s:()]` — a path character in the MSVC format.
bool IsMsvcPathChar(char c) { return !IsSpace(c) && c != ':' && c != '(' && c != ')'; }
// `[^\s:]` — a path character in the path:line format.
bool IsColonPathChar(char c) { return !IsSpace(c) && c != ':'; }

// `(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m)` spanning exactly [begin, end), ignoring ASCII case.
bool IsSourceExtension(const std::string& s, size_t begin, size_t end) {
    static const char* const kExtensions[] = {"cpp", "c", "cc", "cxx", "h", "hpp", "inl", "cs", "java", "mm", "m"};
    const size_t length = end - begin;
    for (const char* ext : kExtensions) {
        size_t i = 0;
        while (i < length && ext[i] != '\0' && ascii::ToLower(s[begin + i]) == ext[i]) {
            ++i;
        }
        if (i == length && ext[i] == '\0') {
            return true;
        }
    }
    return false;
}

// End of the digit run that starts at `pos` (pos itself when there is none).
size_t DigitRunEnd(const std::string& s, size_t pos) {
    while (pos < s.size() && ascii::IsDigit(s[pos])) {
        ++pos;
    }
    return pos;
}

// Per-line lookup tables, reused across the lines of one paste. Index i runs over [0, size]; the
// entry at size stands for "end of line".
struct ScanTables {
    std::vector<size_t> runEnd;         // first index >= i outside the run class (i itself if s[i] is outside)
    std::vector<size_t> lastDot;        // last '.' in [i, runEnd[i]), or kNone
    std::vector<size_t> stop;           // first index >= i holding a stop character, or size
    std::vector<size_t> prevColonDigit; // largest q < i with s[q] == ':' and s[q + 1] a digit, or kNone
};

template <typename InRun> void BuildRunTables(const std::string& s, InRun inRun, ScanTables& t) {
    const size_t n = s.size();
    t.runEnd.assign(n + 1, n);
    t.lastDot.assign(n + 1, kNone);
    for (size_t i = n; i-- > 0;) {
        if (!inRun(s[i])) {
            t.runEnd[i] = i;
            continue;
        }
        t.runEnd[i] = t.runEnd[i + 1];
        t.lastDot[i] = (t.lastDot[i + 1] != kNone) ? t.lastDot[i + 1] : (s[i] == '.' ? i : kNone);
    }
}

template <typename IsStop> void BuildStopTable(const std::string& s, IsStop isStop, ScanTables& t) {
    const size_t n = s.size();
    t.stop.assign(n + 1, n);
    for (size_t i = n; i-- > 0;) {
        t.stop[i] = isStop(s[i]) ? i : t.stop[i + 1];
    }
}

// Where a format matched: the path capture, the line-number digits, and where the match begins.
struct FormatMatch {
    size_t pathBegin = 0;
    size_t pathEnd = 0;
    size_t digitsBegin = 0;
    size_t digitsEnd = 0;
    size_t matchBegin = 0;
};

// Remembers the last `(digits)` check so a run of candidate starts that share one '(' (the common
// case: every start inside one long token) pays for its digit run once. The opens a scan asks about
// never decrease, so one slot per kind of candidate keeps the scan linear.
class ParenLineNumberCache {
  public:
    // True when s[open] is '(' followed by one or more digits and then ')'; sets digitsEnd.
    bool Check(const std::string& s, size_t open, size_t& digitsEnd) {
        if (open != open_) {
            open_ = open;
            ok_ = false;
            if (open < s.size() && s[open] == '(') {
                digitsEnd_ = DigitRunEnd(s, open + 1);
                ok_ = digitsEnd_ > open + 1 && digitsEnd_ < s.size() && s[digitsEnd_] == ')';
            }
        }
        digitsEnd = digitsEnd_;
        return ok_;
    }

  private:
    size_t open_ = kNone;
    size_t digitsEnd_ = 0;
    bool ok_ = false;
};

// MSVC / Unreal: ...\File.cpp(123) or File.cpp(123). Regex (icase):
//   ([A-Za-z]:[^()\r\n]*|\S(?:[^\s:()]*[/\\])?[^\s:()]*\.(?:ext))\(([0-9]+)\)
// Drive form from p: everything up to the first paren or line break, which must open `(digits)`.
// Relative form from p: one non-space character, then the run of path characters after it; the
// '(' must end that run (no path character is a paren), and the run must end in `.ext`, which can
// only follow its last '.'. The optional `[/\\]` group is covered by the run, so it adds nothing.
bool MatchMsvcFormat(const std::string& s, ScanTables& t, FormatMatch& m) {
    const size_t n = s.size();
    BuildRunTables(s, IsMsvcPathChar, t);
    BuildStopTable(s, [](char c) { return c == '(' || c == ')' || c == '\r' || c == '\n'; }, t);
    ParenLineNumberCache driveParen;
    ParenLineNumberCache relativeParen;
    for (size_t p = 0; p < n; ++p) {
        size_t digitsEnd = 0;
        if (p + 1 < n && ascii::IsAlpha(s[p]) && s[p + 1] == ':') {
            const size_t open = t.stop[p + 2];
            if (driveParen.Check(s, open, digitsEnd)) {
                m.pathBegin = p;
                m.pathEnd = open;
                m.digitsBegin = open + 1;
                m.digitsEnd = digitsEnd;
                m.matchBegin = p;
                return true;
            }
        }
        if (!IsSpace(s[p])) {
            const size_t open = t.runEnd[p + 1];
            const size_t dot = t.lastDot[p + 1];
            if (dot != kNone && IsSourceExtension(s, dot + 1, open) && relativeParen.Check(s, open, digitsEnd)) {
                m.pathBegin = p;
                m.pathEnd = open;
                m.digitsBegin = open + 1;
                m.digitsEnd = digitsEnd;
                m.matchBegin = p;
                return true;
            }
        }
    }
    return false;
}

// Where the path run of the path:line format starts for a match at p, or kNone. The regex tries
// `/`, then `\.\.?/`, then `\S[/\\]`; all three leave the same run (every prefix ends in a slash,
// which is itself a path character), so the earliest applicable run start is the most permissive.
size_t ColonFormatRunBegin(const std::string& s, size_t p) {
    const size_t n = s.size();
    if (s[p] == '/') {
        return p + 1;
    }
    if (s[p] == '.' && p + 1 < n && s[p + 1] == '/') {
        return p + 2;
    }
    if (s[p] == '.' && p + 2 < n && s[p + 1] == '.' && s[p + 2] == '/') {
        return p + 3;
    }
    if (!IsSpace(s[p]) && p + 1 < n && IsSlash(s[p + 1])) {
        return p + 2;
    }
    return kNone;
}

bool IsColonDigitAt(const std::string& s, size_t q) {
    return q + 1 < s.size() && s[q] == ':' && ascii::IsDigit(s[q + 1]);
}

// path:line[:column] (Clang, GDB "at /path/foo.cpp:42", Rust backtraces). Regex (icase):
//   ([A-Za-z]:[^:\r\n]+|(?:/|\.\.?/|\S[/\\])[^\s:]+\.(?:ext)):([0-9]+)(?::[0-9]+)?
// Drive form: one or more characters up to the next ':' or line break, then `:digits`. Rooted form:
// the run of path characters (no ':') after the prefix ends at the line-number ':', and at least one
// character must precede the `.ext` that ends it.
bool MatchColonFormat(const std::string& s, ScanTables& t, FormatMatch& m) {
    const size_t n = s.size();
    BuildRunTables(s, IsColonPathChar, t);
    BuildStopTable(s, [](char c) { return c == ':' || c == '\r' || c == '\n'; }, t);
    for (size_t p = 0; p < n; ++p) {
        if (p + 1 < n && ascii::IsAlpha(s[p]) && s[p + 1] == ':') {
            const size_t colon = t.stop[p + 2];
            if (colon > p + 2 && IsColonDigitAt(s, colon)) {
                m.pathBegin = p;
                m.pathEnd = colon;
                m.digitsBegin = colon + 1;
                m.digitsEnd = DigitRunEnd(s, colon + 1);
                m.matchBegin = p;
                return true;
            }
        }
        const size_t runBegin = ColonFormatRunBegin(s, p);
        if (runBegin == kNone) {
            continue;
        }
        const size_t colon = t.runEnd[runBegin];
        const size_t dot = t.lastDot[runBegin];
        if (dot != kNone && dot > runBegin && IsSourceExtension(s, dot + 1, colon) && IsColonDigitAt(s, colon)) {
            m.pathBegin = p;
            m.pathEnd = colon;
            m.digitsBegin = colon + 1;
            m.digitsEnd = DigitRunEnd(s, colon + 1);
            m.matchBegin = p;
            return true;
        }
    }
    return false;
}

// The capture group of the bracket format, tried at g:
//   ([A-Za-z]:[^\]\r\n]+|(?:/|\S[/\\])[^\]\r\n]+):([0-9]+)
// The run may hold ':' and spaces, so the greedy run backs off to the LAST `:digit` before the
// first ']' or line break; it must leave at least one character in the run.
bool MatchBracketGroup(const std::string& s, size_t g, const ScanTables& t, FormatMatch& m) {
    const size_t n = s.size();
    if (g >= n) {
        return false;
    }
    size_t runBegin = kNone;
    if (g + 1 < n && ascii::IsAlpha(s[g]) && s[g + 1] == ':') {
        runBegin = g + 2;
    } else if (s[g] == '/') {
        runBegin = g + 1;
    } else if (!IsSpace(s[g]) && g + 1 < n && IsSlash(s[g + 1])) {
        runBegin = g + 2;
    } else {
        return false;
    }
    const size_t colon = t.prevColonDigit[t.stop[runBegin]];
    if (colon == kNone || colon <= runBegin) {
        return false;
    }
    m.pathBegin = g;
    m.pathEnd = colon;
    m.digitsBegin = colon + 1;
    m.digitsEnd = DigitRunEnd(s, colon + 1);
    return true;
}

// Unreal-style: [File:Line] or File:Line in brackets. Regex (icase):
//   \[?\s*(<group above>)\s*\]?
// From p the optional '[' and the spaces are taken greedily; giving spaces back cannot help (the
// group never starts on a space), but giving the '[' back can: "[\x:1" only matches as "\S[/\\]"
// with the '[' as its first character.
bool MatchBracketFormat(const std::string& s, ScanTables& t, FormatMatch& m) {
    const size_t n = s.size();
    BuildStopTable(s, [](char c) { return c == ']' || c == '\r' || c == '\n'; }, t);
    t.prevColonDigit.assign(n + 1, kNone);
    for (size_t i = 1; i <= n; ++i) {
        t.prevColonDigit[i] = IsColonDigitAt(s, i - 1) ? i - 1 : t.prevColonDigit[i - 1];
    }
    // The start g never moves backwards as p advances, and every position in [g, blankEnd) is
    // whitespace, so a start inside the previous run jumps to its end: each run is skipped once,
    // not once per start inside it (a 16 KiB run of spaces would otherwise cost O(n^2)).
    size_t blankEnd = 0;
    for (size_t p = 0; p < n; ++p) {
        const bool bracket = s[p] == '[';
        size_t g = std::max(bracket ? p + 1 : p, blankEnd);
        while (g < n && IsSpace(s[g])) {
            ++g;
        }
        blankEnd = g;
        if (MatchBracketGroup(s, g, t, m) || (bracket && MatchBracketGroup(s, p, t, m))) {
            m.matchBegin = p;
            return true;
        }
    }
    return false;
}

// `at\s+([^\s]+)\s+` (icase), leftmost: the token after an "at" that is followed by whitespace.
bool FindAtFunction(const std::string& s, size_t& begin, size_t& end) {
    const size_t n = s.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        if (ascii::ToLower(s[i]) != 'a' || ascii::ToLower(s[i + 1]) != 't') {
            continue;
        }
        size_t j = i + 2;
        while (j < n && IsSpace(s[j])) {
            ++j;
        }
        if (j == i + 2) {
            continue;
        }
        size_t k = j;
        while (k < n && !IsSpace(s[k])) {
            ++k;
        }
        if (k > j && k < n) { // the token must be followed by at least one whitespace character
            begin = j;
            end = k;
            return true;
        }
    }
    return false;
}

std::string Slice(const std::string& s, size_t begin, size_t end) { return s.substr(begin, end - begin); }

bool TryParsePathLinePair(const std::string& line, ScanTables& tables, std::string& outPath, int& outLine,
                          std::string& outFunction) {
    outPath.clear();
    outLine = 0;
    outFunction.clear();

    FormatMatch m;
    if (HasCharThenDigit(line, '(') && MatchMsvcFormat(line, tables, m)) {
        outPath = Trim(Slice(line, m.pathBegin, m.pathEnd));
        if (!ParseLineNumberInRange(Slice(line, m.digitsBegin, m.digitsEnd), outLine)) {
            return false;
        }
        if (m.matchBegin > 0) {
            std::string fn = Trim(line.substr(0, m.matchBegin));
            const size_t bang = fn.find('!');
            if (bang != std::string::npos) {
                fn = Trim(fn.substr(bang + 1));
            }
            outFunction = fn;
        }
        TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
        return true;
    }

    // Both remaining formats end in `:<line>`.
    if (!HasCharThenDigit(line, ':')) {
        return false;
    }

    if (MatchColonFormat(line, tables, m)) {
        outPath = Trim(Slice(line, m.pathBegin, m.pathEnd));
        if (!ParseLineNumberInRange(Slice(line, m.digitsBegin, m.digitsEnd), outLine)) {
            return false;
        }
        size_t fnBegin = 0;
        size_t fnEnd = 0;
        if (FindAtFunction(line, fnBegin, fnEnd)) { // optional "at Function " prefix
            outFunction = Trim(Slice(line, fnBegin, fnEnd));
        }
        TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
        return true;
    }

    // Only the first bracket-format match is considered; a match whose path has no '.', '/' or '\'
    // ends the search (it is not a path), as it always has.
    if (MatchBracketFormat(line, tables, m)) {
        std::string p = Trim(Slice(line, m.pathBegin, m.pathEnd));
        if (!p.empty() && (p.find('.') != std::string::npos || p.find('/') != std::string::npos ||
                           p.find('\\') != std::string::npos)) {
            outPath = p;
            if (!ParseLineNumberInRange(Slice(line, m.digitsBegin, m.digitsEnd), outLine)) {
                return false;
            }
            TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
            return true;
        }
    }

    return false;
}

// Per-line cap on input fed into TryParsePathLinePair. The scans are linear, so this is no longer
// what keeps a hostile paste from stalling the parser; it bounds the per-line tables (a few vectors
// of line length) and keeps lines that are almost certainly garbage out of the frame list. 16 KiB
// exceeds any realistic IDE-pasted frame.
static constexpr size_t kMaxLineLength = 16384;

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
    ScanTables tables;
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
        if (line.size() > kMaxLineLength) {
            continue;
        }

        ParsedCallstackFrame f;
        f.RawLine = line;
        std::string path;
        int lineNum = 0;
        std::string fn;
        if (TryParsePathLinePair(line, tables, path, lineNum, fn)) {
            f.FilePath = std::move(path);
            f.LineNumber = lineNum;
            f.Function = fn.empty() ? std::string("<unknown>") : std::move(fn);
            out.push_back(std::move(f));
        }
    }
    return out;
}
