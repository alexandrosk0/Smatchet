#pragma once

// The one text matcher every picker, filter and the grid search use (docs/plans/input-ux-w2-one-matcher.md).
//
// A query is split on whitespace into words. Each word is case-folded (UTF-8 aware: ASCII, Latin-1, Latin
// Extended-A, Latin Extended Additional, Greek, Cyrillic, Armenian and fullwidth Latin fold; other text
// compares as written) and must occur in the candidate, in any order. A word that occurs as written
// ("substring") is scored on its best occurrence; in Mode::Fuzzy a word that does not occur may still match
// with its letters in order, scored on the best alignment. Scores reward letters at word starts, at the
// start of the text, consecutive letters, and a word typed whole. A list follows one rule: when any row
// matches every word as written, only those rows are listed (RankKey puts them first; RowListed drops
// the rest), so letters-in-order matches never drown exact ones.
//
// Pure, header-only, no ImGui, C++14. Every read is bounded by the byte counts passed in; an invalid UTF-8
// byte decodes as itself (kInvalidByteBase + byte), so a malformed string can never be read past its end.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace smatchet {
namespace textmatch {

constexpr std::size_t kMaxQueryWords = 16;       // words past this are dropped: the match only loosens
constexpr std::size_t kMaxWordCodepoints = 64;   // a longer word is cut to this many code points
constexpr std::size_t kMaxFuzzyCodepoints = 512; // letters-in-order matching looks this far into a candidate
constexpr std::size_t kMaxTextBytes = 16384;     // a candidate or query is read this far

/// An invalid UTF-8 byte b decodes as kInvalidByteBase + b: above every real code point, so it only ever
/// equals the same invalid byte.
constexpr char32_t kInvalidByteBase = 0x110000;

/// Decodes the code point at s[0] (n >= 1 bytes readable) and sets `len` to its byte length (1-4). An
/// overlong form, a surrogate, a truncated sequence or a stray continuation byte decodes as one invalid byte.
inline char32_t DecodeUtf8(const char* s, std::size_t n, std::size_t& len) {
    const unsigned char b0 = static_cast<unsigned char>(s[0]);
    len = 1;
    if (b0 < 0x80u) {
        return b0;
    }
    const auto cont = [s, n](std::size_t i) { return i < n && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u; };
    const auto at = [s](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(s[i]) & 0x3Fu); };
    if (b0 >= 0xC2u && b0 <= 0xDFu && cont(1)) {
        len = 2;
        return (static_cast<char32_t>(b0 & 0x1Fu) << 6) | at(1);
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu && cont(1) && cont(2)) {
        const unsigned char b1 = static_cast<unsigned char>(s[1]);
        const bool overlong = b0 == 0xE0u && b1 < 0xA0u;
        const bool surrogate = b0 == 0xEDu && b1 >= 0xA0u;
        if (!overlong && !surrogate) {
            len = 3;
            return (static_cast<char32_t>(b0 & 0x0Fu) << 12) | (at(1) << 6) | at(2);
        }
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u && cont(1) && cont(2) && cont(3)) {
        const unsigned char b1 = static_cast<unsigned char>(s[1]);
        const bool overlong = b0 == 0xF0u && b1 < 0x90u;
        const bool tooHigh = b0 == 0xF4u && b1 > 0x8Fu;
        if (!overlong && !tooHigh) {
            len = 4;
            return (static_cast<char32_t>(b0 & 0x07u) << 18) | (at(1) << 12) | (at(2) << 6) | at(3);
        }
    }
    return kInvalidByteBase + b0;
}

/// Appends `cp` as UTF-8; an invalid-byte code point is written back as that byte.
inline void AppendUtf8(std::string& out, char32_t cp) {
    if (cp >= kInvalidByteBase) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(cp - kInvalidByteBase)));
    } else if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

namespace detail {

// Upper case at an even code point, its lower case at the next one.
inline char32_t EvenUpper(char32_t c) { return (c & 1u) == 0u ? c + 1u : c; }
// Upper case at an odd code point, its lower case at the next one.
inline char32_t OddUpper(char32_t c) { return (c & 1u) != 0u ? c + 1u : c; }

inline char32_t FoldLatinExtendedA(char32_t c) { // U+0100-U+017F
    if (c == 0x130u || c == 0x131u || c == 0x138u || c == 0x149u) {
        return c; // dotted I / dotless i (no simple fold), kra, n-apostrophe
    }
    if (c == 0x178u) {
        return 0xFFu; // Y with diaeresis
    }
    if (c == 0x17Fu) {
        return 's'; // long s
    }
    if ((c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17Eu)) {
        return OddUpper(c);
    }
    return EvenUpper(c); // U+0100-012F, U+0132-0137, U+014A-0177
}

inline char32_t FoldGreekSymbol(char32_t c) { // U+03CF-U+03FF outside the paired run
    switch (c) {
    case 0x3CFu:
        return 0x3D7u;
    case 0x3D0u:
        return 0x3B2u; // beta symbol
    case 0x3D1u:
    case 0x3F4u:
        return 0x3B8u; // theta symbols
    case 0x3D5u:
        return 0x3C6u; // phi symbol
    case 0x3D6u:
        return 0x3C0u; // pi symbol
    case 0x3F0u:
        return 0x3BAu; // kappa symbol
    case 0x3F1u:
        return 0x3C1u; // rho symbol
    case 0x3F5u:
        return 0x3B5u; // lunate epsilon symbol
    case 0x3F7u:
    case 0x3FAu:
        return c + 1u;
    case 0x3F9u:
        return 0x3F2u; // capital lunate sigma
    default:
        break;
    }
    if (c >= 0x3FDu) {
        return c - 0x82u; // U+03FD-03FF -> U+037B-037D
    }
    return c;
}

inline char32_t FoldGreek(char32_t c) { // U+0370-U+03FF
    if (c == 0x370u || c == 0x372u || c == 0x376u) {
        return c + 1u; // Heta, archaic sampi, Pamphylian digamma
    }
    if (c == 0x37Fu) {
        return 0x3F3u; // yot
    }
    if (c == 0x386u) {
        return 0x3ACu;
    }
    if (c >= 0x388u && c <= 0x38Au) {
        return c + 37u;
    }
    if (c == 0x38Cu) {
        return 0x3CCu;
    }
    if (c == 0x38Eu || c == 0x38Fu) {
        return c + 63u;
    }
    if (c >= 0x391u && c <= 0x3ABu && c != 0x3A2u) {
        return c + 32u;
    }
    if (c == 0x3C2u) {
        return 0x3C3u; // final sigma
    }
    if (c >= 0x3D8u && c <= 0x3EFu) {
        return EvenUpper(c);
    }
    return c >= 0x3CFu ? FoldGreekSymbol(c) : c;
}

inline char32_t FoldCyrillic(char32_t c) { // U+0400-U+052F
    if (c < 0x410u) {
        return c + 80u;
    }
    if (c < 0x430u) {
        return c + 32u;
    }
    if (c < 0x460u) {
        return c;
    }
    if (c <= 0x481u || (c >= 0x48Au && c <= 0x4BFu) || c >= 0x4D0u) {
        return EvenUpper(c);
    }
    if (c == 0x4C0u) {
        return 0x4CFu; // palochka
    }
    if (c >= 0x4C1u && c <= 0x4CEu) {
        return OddUpper(c);
    }
    return c;
}

} // namespace detail

/// The simple case fold of `c` for the blocks whose mapping is regular; any other code point is returned
/// unchanged (compared as written, never mapped to a wrong letter).
inline char32_t FoldCodepoint(char32_t c) {
    if (c < 0x80u) {
        return (c >= 'A' && c <= 'Z') ? c + 32u : c;
    }
    if (c < 0x100u) {
        if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u) {
            return c + 32u;
        }
        return c == 0xB5u ? 0x3BCu : c; // micro sign folds to mu
    }
    if (c < 0x180u) {
        return detail::FoldLatinExtendedA(c);
    }
    if (c >= 0x370u && c < 0x400u) {
        return detail::FoldGreek(c);
    }
    if (c >= 0x400u && c < 0x530u) {
        return detail::FoldCyrillic(c);
    }
    if (c >= 0x531u && c <= 0x556u) {
        return c + 48u; // Armenian
    }
    if (c >= 0x1E00u && c <= 0x1EFFu) {
        if (c == 0x1E9Eu) {
            return 0xDFu; // capital sharp s
        }
        if (c == 0x1E9Bu) {
            return 0x1E61u; // long s with dot above
        }
        return (c <= 0x1E95u || c >= 0x1EA0u) ? detail::EvenUpper(c) : c;
    }
    if (c >= 0xFF21u && c <= 0xFF3Au) {
        return c + 32u; // fullwidth Latin
    }
    return c;
}

/// Whitespace that separates query words: ASCII space, tab, LF, VT, FF, CR, no-break space, ideographic space.
inline bool IsQuerySpace(char32_t c) { return c == ' ' || (c >= 0x09u && c <= 0x0Du) || c == 0xA0u || c == 0x3000u; }

/// A letter or digit for word-start scoring: ASCII alphanumerics, and any non-ASCII code point outside the
/// common punctuation and space blocks.
inline bool IsWordChar(char32_t c) {
    if (c < 0x80u) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    if (c >= kInvalidByteBase) {
        return false;
    }
    if ((c >= 0xA0u && c <= 0xBFu) || c == 0xD7u || c == 0xF7u) {
        return false; // Latin-1 punctuation and signs
    }
    if ((c >= 0x2000u && c <= 0x206Fu) || c == 0x3000u) {
        return false; // general punctuation, ideographic space
    }
    return true;
}

/// `s` (all `n` bytes: haystacks such as an audit event's JSON may be long) case-folded, as UTF-8, invalid
/// bytes kept. Linear, one allocation.
inline std::string FoldCopy(const char* s, std::size_t n) {
    std::string out;
    if (s == nullptr) {
        return out;
    }
    out.reserve(n);
    std::size_t i = 0;
    while (i < n) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80u) {
            out.push_back(static_cast<char>((b >= 'A' && b <= 'Z') ? b + 32u : b));
            ++i;
            continue;
        }
        std::size_t len = 1;
        const char32_t cp = DecodeUtf8(s + i, n - i, len);
        AppendUtf8(out, FoldCodepoint(cp));
        i += len;
    }
    return out;
}

inline std::string FoldCopy(const std::string& s) { return FoldCopy(s.data(), s.size()); }

/// The byte offset of the first occurrence of `needleFolded` (already FoldCopy'd) in `hay` compared through
/// the fold, at or after byte `from`; std::string::npos when there is none. An empty needle is found at
/// `from` (when from <= hayLen). No allocation: both sides are decoded as they are compared.
inline std::size_t FindFolded(const char* hay, std::size_t hayLen, const std::string& needleFolded,
                              std::size_t from = 0) {
    const std::size_t needleLen = needleFolded.size();
    if (hay == nullptr) {
        hayLen = 0;
    }
    if (from > hayLen) {
        return std::string::npos;
    }
    if (needleLen == 0) {
        return from;
    }
    const char* needle = needleFolded.data();
    std::size_t start = from;
    while (start < hayLen) {
        std::size_t i = start;
        std::size_t j = 0;
        while (j < needleLen && i < hayLen) {
            const unsigned char hb = static_cast<unsigned char>(hay[i]);
            const unsigned char nb = static_cast<unsigned char>(needle[j]);
            if (hb < 0x80u && nb < 0x80u) {
                if (static_cast<unsigned char>((hb >= 'A' && hb <= 'Z') ? hb + 32u : hb) != nb) {
                    break;
                }
                ++i;
                ++j;
                continue;
            }
            std::size_t hl = 1;
            std::size_t nl = 1;
            const char32_t hc = FoldCodepoint(DecodeUtf8(hay + i, hayLen - i, hl));
            const char32_t nc = DecodeUtf8(needle + j, needleLen - j, nl);
            if (hc != nc) {
                break;
            }
            i += hl;
            j += nl;
        }
        if (j == needleLen) {
            return start;
        }
        std::size_t step = 1;
        DecodeUtf8(hay + start, hayLen - start, step);
        start += step;
    }
    return std::string::npos;
}

inline std::size_t FindFolded(const std::string& hay, const std::string& needleFolded, std::size_t from = 0) {
    return FindFolded(hay.data(), hay.size(), needleFolded, from);
}

/// True when `text` equals `otherFolded` (a FoldCopy'd string) compared through the fold. No allocation.
inline bool EqualsFolded(const char* text, std::size_t len, const std::string& otherFolded) {
    if (text == nullptr) {
        len = 0;
    }
    std::size_t i = 0;
    std::size_t j = 0;
    const std::size_t m = otherFolded.size();
    while (i < len && j < m) {
        std::size_t tl = 1;
        std::size_t ol = 1;
        const char32_t tc = FoldCodepoint(DecodeUtf8(text + i, len - i, tl));
        const char32_t oc = DecodeUtf8(otherFolded.data() + j, m - j, ol);
        if (tc != oc) {
            return false;
        }
        i += tl;
        j += ol;
    }
    return i == len && j == m;
}

inline bool EqualsFolded(const char* text, const std::string& otherFolded) {
    return EqualsFolded(text, text != nullptr ? std::strlen(text) : 0, otherFolded);
}

/// A prepared query: its words, folded, in typed order.
struct Query {
    std::vector<std::u32string> Words;  // folded code points
    std::vector<std::string> WordsUtf8; // the same words as folded UTF-8 (for pre-folded haystacks)
    bool Empty() const { return Words.empty(); }
};

/// Splits `raw` (at most kMaxTextBytes read) on whitespace into at most kMaxQueryWords folded words of at
/// most kMaxWordCodepoints each. Leading, trailing and repeated whitespace never makes a word.
inline Query MakeQuery(const char* raw, std::size_t n) {
    Query q;
    if (raw == nullptr) {
        return q;
    }
    n = (std::min)(n, kMaxTextBytes);
    std::u32string word;
    const auto flush = [&q, &word]() {
        if (!word.empty() && q.Words.size() < kMaxQueryWords) {
            std::string utf8;
            for (const char32_t c : word) {
                AppendUtf8(utf8, c);
            }
            q.Words.push_back(word);
            q.WordsUtf8.push_back(std::move(utf8));
        }
        word.clear();
    };
    std::size_t i = 0;
    while (i < n) {
        std::size_t len = 1;
        const char32_t cp = DecodeUtf8(raw + i, n - i, len);
        i += len;
        if (IsQuerySpace(cp)) {
            flush();
        } else if (word.size() < kMaxWordCodepoints) {
            word.push_back(FoldCodepoint(cp));
        }
    }
    flush();
    return q;
}

inline Query MakeQuery(const char* raw) { return MakeQuery(raw, raw != nullptr ? std::strlen(raw) : 0); }
inline Query MakeQuery(const std::string& raw) { return MakeQuery(raw.data(), raw.size()); }

/// True when every query word occurs in `haystackFolded` (a FoldCopy'd string) as a plain substring. The
/// exact-only check for pre-folded haystacks (settings search tier 1, the audit log). No words: true.
inline bool ContainsAllWordsFolded(const std::string& haystackFolded, const Query& q) {
    for (const std::string& w : q.WordsUtf8) {
        if (haystackFolded.find(w) == std::string::npos) {
            return false;
        }
    }
    return true;
}

/// A matched part of a candidate: bytes [Begin, End).
struct ByteRange {
    std::size_t Begin = 0;
    std::size_t End = 0;
};

enum class Mode {
    Substring, // every word must occur as written
    Fuzzy,     // a word that does not occur may match with its letters in order
};

/// The highest Score a result carries (a sum over words is clamped here, so callers may add to it).
constexpr int kMaxScore = 1 << 22;

struct Result {
    int Score = 0;          // 0 = no match; otherwise > 0, comparable among results of the same query
    bool Substring = false; // every word occurred as written (meaningful only when Matched())
    bool Matched() const { return Score > 0; }
};

/// Added to the RankKey of a result whose words all occurred as written: above every Score.
constexpr long long kExactRankTier = 1LL << 40;

/// The order a list sorts its rows in, best first: a result whose words all occurred as written outranks any
/// letters-in-order one, then the higher score wins. 0 for no match.
inline long long RankKey(const Result& r) {
    if (!r.Matched()) {
        return 0;
    }
    return (r.Substring ? kExactRankTier : 0LL) + static_cast<long long>(r.Score);
}

/// True for a RankKey whose words all occurred as written.
inline bool IsExactRank(long long rankKey) { return rankKey >= kExactRankTier; }

/// The list rule: a row is listed when it matched, and, when the list holds at least one row whose words
/// all occurred as written, only if it is such a row too.
inline bool RowListed(const Result& r, bool anyExactRow) { return r.Matched() && (r.Substring || !anyExactRow); }

/// One searchable part of a candidate (title, id, description...), and how much a match in it weighs.
struct Field {
    const char* Text = nullptr;
    std::size_t Len = 0;
    int Weight = 1;
    bool ExactOnly = false; // ids and opaque keys: a word must occur as written, never letters-in-order
};

namespace detail {

constexpr int kChar = 16;        // every matched letter
constexpr int kWordStart = 12;   // a matched letter that starts a word (after a separator, camelCase, digit run)
constexpr int kTextStart = 8;    // the word's first letter is the candidate's first letter
constexpr int kConsecutive = 10; // a matched letter right after the previous one
constexpr int kWholeWord = 6;    // the word occurred as written and ends where a candidate word ends
constexpr int kWholeText = 10;   // the word occurred as written and is the whole candidate
constexpr int kNoScore = -(1 << 28);

// One candidate, decoded: folded code points, their byte offsets / lengths, and word-start flags.
struct Decoded {
    std::vector<char32_t> Cp;
    std::vector<std::uint32_t> Off;
    std::vector<std::uint8_t> Len;
    std::vector<std::uint8_t> WordStart;
    std::size_t Bytes = 0;
};

inline bool IsAsciiLower(char32_t c) { return c >= 'a' && c <= 'z'; }
inline bool IsAsciiUpper(char32_t c) { return c >= 'A' && c <= 'Z'; }
inline bool IsDigit(char32_t c) { return c >= '0' && c <= '9'; }

inline void DecodeText(const char* text, std::size_t n, Decoded& d) {
    d.Bytes = 0;
    if (text == nullptr) {
        n = 0;
    }
    n = (std::min)(n, kMaxTextBytes);
    // Sized for the worst case (one code point per byte) and trimmed after: writes by index are far cheaper
    // than four push_backs per code point on the per-frame path, and a warm Scratch never reallocates.
    d.Cp.resize(n);
    d.Off.resize(n);
    d.Len.resize(n);
    d.WordStart.resize(n);
    d.Bytes = n;
    char32_t prevRaw = 0;
    bool prevWord = false;
    std::size_t i = 0;
    std::size_t k = 0;
    while (i < n) {
        std::size_t len = 1;
        const unsigned char b = static_cast<unsigned char>(text[i]);
        const char32_t raw = b < 0x80u ? static_cast<char32_t>(b) : DecodeUtf8(text + i, n - i, len);
        const bool word = IsWordChar(raw);
        bool start = false;
        if (word) {
            start = !prevWord || (IsAsciiLower(prevRaw) && IsAsciiUpper(raw)) ||
                    (IsDigit(prevRaw) != IsDigit(raw) && prevRaw < 0x80u && raw < 0x80u);
        }
        d.Cp[k] = b < 0x80u ? (IsAsciiUpper(raw) ? raw + 32u : raw) : FoldCodepoint(raw);
        d.Off[k] = static_cast<std::uint32_t>(i);
        d.Len[k] = static_cast<std::uint8_t>(len);
        d.WordStart[k] = start ? 1u : 0u;
        ++k;
        prevRaw = raw;
        prevWord = word;
        i += len;
    }
    d.Cp.resize(k);
    d.Off.resize(k);
    d.Len.resize(k);
    d.WordStart.resize(k);
}

// True when position `end` (exclusive) closes a word: the end, a non-word character, or a new word.
inline bool EndsWord(const Decoded& d, std::size_t end) {
    return end >= d.Cp.size() || !IsWordChar(d.Cp[end]) || d.WordStart[end] != 0u;
}

inline int LetterScore(const Decoded& d, std::size_t i) { return kChar + (d.WordStart[i] != 0u ? kWordStart : 0); }

// The score of the word occurring as written at code point `at`.
inline int ContiguousScore(const Decoded& d, std::size_t at, std::size_t len) {
    int score = at == 0 ? kTextStart : 0;
    for (std::size_t k = 0; k < len; ++k) {
        score += LetterScore(d, at + k);
        if (k > 0) {
            score += kConsecutive;
        }
    }
    if (EndsWord(d, at + len)) {
        score += kWholeWord;
        if (at == 0 && at + len == d.Cp.size()) {
            score += kWholeText;
        }
    }
    return score;
}

struct WordMatch {
    int Score = kNoScore;
    bool Substring = false;
};

// The best occurrence of `w` as written; its start code point goes to `at`.
inline WordMatch BestOccurrence(const Decoded& d, const std::u32string& w, std::size_t& at) {
    WordMatch best;
    const std::size_t n = d.Cp.size();
    const std::size_t m = w.size();
    if (m == 0 || m > n) {
        return best;
    }
    for (std::size_t s = 0; s + m <= n; ++s) {
        if (d.Cp[s] != w[0] || !std::equal(w.begin(), w.end(), d.Cp.begin() + static_cast<std::ptrdiff_t>(s))) {
            continue;
        }
        const int score = ContiguousScore(d, s, m);
        if (score > best.Score) {
            best.Score = score;
            best.Substring = true;
            at = s;
        }
    }
    return best;
}

// The best letters-in-order alignment of `w` over the first kMaxFuzzyCodepoints of `d`. The table holds, for
// each letter of the word and each code point, the best score that puts that letter there. A letter right
// after the previous one earns kConsecutive, and a gap costs one point per skipped code point. The matched
// code points are appended to `picked`, in word order.
inline WordMatch BestAlignment(const Decoded& d, const std::u32string& w, std::vector<int>& dp, std::vector<int>& from,
                               std::vector<std::size_t>& picked) {
    WordMatch best;
    const std::size_t n = (std::min)(d.Cp.size(), kMaxFuzzyCodepoints);
    const std::size_t m = w.size();
    if (m == 0 || m > n) {
        return best;
    }
    std::size_t k = 0;
    for (std::size_t i = 0; i < n && k < m; ++i) {
        if (d.Cp[i] == w[k]) {
            ++k;
        }
    }
    if (k < m) {
        return best; // the letters do not all occur in order: no table needed
    }
    dp.assign(m * n, kNoScore);
    from.assign(m * n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        if (d.Cp[i] == w[0]) {
            dp[i] = LetterScore(d, i) + (i == 0 ? kTextStart : 0);
        }
    }
    for (k = 1; k < m; ++k) {
        const int* prev = &dp[(k - 1) * n];
        int* cur = &dp[k * n];
        int* curFrom = &from[k * n];
        int gapBest = kNoScore; // max over j <= i - 2 of prev[j] + j
        int gapFrom = -1;
        for (std::size_t i = 1; i < n; ++i) {
            if (i >= 2 && prev[i - 2] > kNoScore) {
                const int cand = prev[i - 2] + static_cast<int>(i - 2);
                if (cand > gapBest) {
                    gapBest = cand;
                    gapFrom = static_cast<int>(i - 2);
                }
            }
            if (d.Cp[i] != w[k]) {
                continue;
            }
            int via = kNoScore;
            int viaFrom = -1;
            if (prev[i - 1] > kNoScore) {
                via = prev[i - 1] + kConsecutive;
                viaFrom = static_cast<int>(i - 1);
            }
            if (gapBest > kNoScore && gapBest - static_cast<int>(i - 1) > via) {
                via = gapBest - static_cast<int>(i - 1);
                viaFrom = gapFrom;
            }
            if (viaFrom >= 0) {
                cur[i] = via + LetterScore(d, i);
                curFrom[i] = viaFrom;
            }
        }
    }
    const int* last = &dp[(m - 1) * n];
    int end = -1;
    for (std::size_t i = 0; i < n; ++i) {
        if (last[i] > best.Score) {
            best.Score = last[i];
            end = static_cast<int>(i);
        }
    }
    if (end < 0) {
        best.Score = kNoScore;
        return best;
    }
    const std::size_t base = picked.size();
    picked.resize(base + m);
    int pos = end;
    for (std::size_t kk = m; kk-- > 0;) {
        picked[base + kk] = static_cast<std::size_t>(pos);
        pos = kk > 0 ? from[kk * n + static_cast<std::size_t>(pos)] : -1;
    }
    return best;
}

// A cheap necessary condition, read straight off the bytes with no decode buffer: every letter of `w` occurs
// in `text` in order (any match, as written or letters-in-order, implies it). Lets MatchFields reject most
// candidates of a filtered list without decoding them.
inline bool LettersOccurInOrder(const std::u32string& w, const char* text, std::size_t n) {
    if (text == nullptr) {
        return w.empty();
    }
    n = (std::min)(n, kMaxTextBytes);
    std::size_t k = 0;
    std::size_t i = 0;
    while (i < n && k < w.size()) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        char32_t c = 0;
        if (b < 0x80u) {
            c = (b >= 'A' && b <= 'Z') ? b + 32u : b;
            ++i;
        } else {
            std::size_t len = 1;
            c = FoldCodepoint(DecodeUtf8(text + i, n - i, len));
            i += len;
        }
        if (c == w[k]) {
            ++k;
        }
    }
    return k == w.size();
}

inline void AppendRange(std::vector<ByteRange>& out, const Decoded& d, std::size_t first, std::size_t count) {
    if (count == 0 || first >= d.Cp.size()) {
        return;
    }
    const std::size_t lastCp = (std::min)(first + count, d.Cp.size()) - 1;
    ByteRange r;
    r.Begin = d.Off[first];
    r.End = static_cast<std::size_t>(d.Off[lastCp]) + d.Len[lastCp];
    out.push_back(r);
}

// Sorts and merges touching / overlapping ranges.
inline void NormalizeRanges(std::vector<ByteRange>& ranges) {
    if (ranges.size() < 2) {
        return;
    }
    std::sort(ranges.begin(), ranges.end(), [](const ByteRange& a, const ByteRange& b) { return a.Begin < b.Begin; });
    std::size_t w = 0;
    for (std::size_t r = 1; r < ranges.size(); ++r) {
        if (ranges[r].Begin <= ranges[w].End) {
            ranges[w].End = (std::max)(ranges[w].End, ranges[r].End);
        } else {
            ranges[++w] = ranges[r];
        }
    }
    ranges.resize(w + 1);
}

} // namespace detail

/// Reusable working memory. Keep one per list (an open dropdown, a palette) and the warm path allocates
/// nothing per candidate.
struct Scratch {
    std::vector<detail::Decoded> Texts;
    std::vector<unsigned char> Possible;  // per word x field: the word's letters occur in the field in order
    std::vector<unsigned char> FieldUsed; // per field: some word may match it (only those are decoded)
    std::vector<int> Dp;
    std::vector<int> From;
    std::vector<std::size_t> Picked;
};

namespace detail {

inline void ClearRanges(std::vector<ByteRange>* ranges, std::size_t count) {
    if (ranges != nullptr) {
        for (std::size_t f = 0; f < count; ++f) {
            ranges[f].clear();
        }
    }
}

// Marks which fields can hold each word at all (its letters in order, read off the bytes) and decodes only the
// fields some word can use. False when a word fits no field: no match, and nothing was decoded.
inline bool PrepareFields(const Query& q, const Field* fields, std::size_t count, Scratch& scratch) {
    const std::size_t wordCount = q.Words.size();
    scratch.Possible.assign(wordCount * count, 0u);
    scratch.FieldUsed.assign(count, 0u);
    for (std::size_t wi = 0; wi < wordCount; ++wi) {
        bool any = false;
        for (std::size_t f = 0; f < count; ++f) {
            const std::size_t len = fields[f].Text != nullptr ? fields[f].Len : 0;
            if (LettersOccurInOrder(q.Words[wi], fields[f].Text, len)) {
                scratch.Possible[wi * count + f] = 1u;
                scratch.FieldUsed[f] = 1u;
                any = true;
            }
        }
        if (!any) {
            return false;
        }
    }
    if (scratch.Texts.size() < count) {
        scratch.Texts.resize(count);
    }
    for (std::size_t f = 0; f < count; ++f) {
        if (scratch.FieldUsed[f] != 0u) {
            DecodeText(fields[f].Text, fields[f].Len, scratch.Texts[f]);
        }
    }
    return true;
}

// Where one word matches best: Field == count when it matches nowhere.
struct FieldPick {
    WordMatch Match;
    long long Weighted = 0;
    std::size_t Field = 0;
    std::size_t At = 0; // first code point of an occurrence as written
    bool Fuzzy = false; // letters in order: the code points are in Scratch::Picked from PickedBase
    std::size_t PickedBase = 0;
};

// An occurrence as written in any field beats letters in order in any other; then weight x score wins.
inline FieldPick BestFieldForWord(const Query& q, std::size_t wi, const Field* fields, std::size_t count, Mode mode,
                                  Scratch& scratch) {
    const std::u32string& w = q.Words[wi];
    FieldPick best;
    best.Field = count;
    scratch.Picked.clear();
    for (std::size_t f = 0; f < count; ++f) {
        if (scratch.Possible[wi * count + f] == 0u) {
            continue; // its letters are not in this field: it cannot match here
        }
        const Decoded& d = scratch.Texts[f];
        FieldPick pick;
        pick.Field = f;
        pick.PickedBase = scratch.Picked.size();
        pick.Match = BestOccurrence(d, w, pick.At);
        if (pick.Match.Score <= kNoScore && mode == Mode::Fuzzy && !fields[f].ExactOnly) {
            pick.Match = BestAlignment(d, w, scratch.Dp, scratch.From, scratch.Picked);
            pick.Fuzzy = pick.Match.Score > kNoScore;
        }
        if (pick.Match.Score <= kNoScore) {
            continue;
        }
        pick.Weighted = static_cast<long long>((std::max)(1, fields[f].Weight)) *
                        static_cast<long long>((std::max)(1, pick.Match.Score));
        const bool better = best.Field == count || (pick.Match.Substring && !best.Match.Substring) ||
                            (pick.Match.Substring == best.Match.Substring && pick.Weighted > best.Weighted);
        if (better) {
            best = pick;
        }
    }
    return best;
}

inline void AppendWordRanges(const FieldPick& pick, std::size_t wordLen, const Scratch& scratch,
                             std::vector<ByteRange>* ranges) {
    if (ranges == nullptr) {
        return;
    }
    const Decoded& d = scratch.Texts[pick.Field];
    if (!pick.Fuzzy) {
        AppendRange(ranges[pick.Field], d, pick.At, wordLen);
        return;
    }
    for (std::size_t k = 0; k < wordLen; ++k) {
        AppendRange(ranges[pick.Field], d, scratch.Picked[pick.PickedBase + k], 1);
    }
}

} // namespace detail

/// Matches `q` against `count` fields: every word must match some field, and each word takes the field
/// where it scores best (an occurrence as written in any field beats letters-in-order in any other; then
/// weight x score). `ranges`, when not null, points at `count` vectors that receive each field's matched
/// bytes (sorted, merged). An empty query matches with Score 1 and no ranges.
inline Result MatchFields(const Query& q, const Field* fields, std::size_t count, Mode mode, Scratch& scratch,
                          std::vector<ByteRange>* ranges = nullptr) {
    Result result;
    detail::ClearRanges(ranges, count);
    if (q.Empty()) {
        result.Score = 1;
        result.Substring = true;
        return result;
    }
    if (fields == nullptr || count == 0 || !detail::PrepareFields(q, fields, count, scratch)) {
        return result;
    }
    long long total = 0;
    bool allSubstring = true;
    for (std::size_t wi = 0; wi < q.Words.size(); ++wi) {
        const detail::FieldPick pick = detail::BestFieldForWord(q, wi, fields, count, mode, scratch);
        if (pick.Field == count) {
            detail::ClearRanges(ranges, count); // no match: no ranges from the words before this one
            return Result();
        }
        allSubstring = allSubstring && pick.Match.Substring;
        total += pick.Weighted;
        detail::AppendWordRanges(pick, q.Words[wi].size(), scratch, ranges);
    }
    if (ranges != nullptr) {
        for (std::size_t f = 0; f < count; ++f) {
            detail::NormalizeRanges(ranges[f]);
        }
    }
    result.Score = static_cast<int>((std::min)(total, static_cast<long long>(kMaxScore)));
    result.Score = (std::max)(result.Score, 1);
    result.Substring = allSubstring;
    return result;
}

/// Matches `q` against one text. `ranges` (optional) receives the matched bytes, sorted and merged.
inline Result Match(const Query& q, const char* text, std::size_t len, Mode mode, Scratch& scratch,
                    std::vector<ByteRange>* ranges = nullptr) {
    Field field;
    field.Text = text;
    field.Len = text != nullptr ? len : 0;
    return MatchFields(q, &field, 1, mode, scratch, ranges);
}

/// Convenience form with its own working memory (allocates; for one-off calls, not per-row loops).
inline Result Match(const Query& q, const std::string& text, Mode mode = Mode::Fuzzy,
                    std::vector<ByteRange>* ranges = nullptr) {
    Scratch scratch;
    return Match(q, text.data(), text.size(), mode, scratch, ranges);
}

} // namespace textmatch
} // namespace smatchet
