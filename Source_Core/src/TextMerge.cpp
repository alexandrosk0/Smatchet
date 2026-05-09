#include "TextMerge.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace TextMerge {

namespace {

using Lines = std::vector<std::string>;

Lines SplitLines(const std::string& text) {
    Lines out;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        out.push_back(std::move(line));
    }
    return out;
}

std::string JoinLines(const Lines& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// LCS — O(n*m) DP; sufficient for description-sized texts (< ~1000 lines).
// Returns matched (indexA, indexB) pairs in forward order.
// ---------------------------------------------------------------------------
struct LcsPair { int a; int b; };

std::vector<LcsPair> ComputeLcs(const Lines& A, const Lines& B) {
    const int m = static_cast<int>(A.size());
    const int n = static_cast<int>(B.size());
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            dp[i][j] = (A[i-1] == B[j-1])
                ? dp[i-1][j-1] + 1
                : (std::max)(dp[i-1][j], dp[i][j-1]);
        }
    }
    std::vector<LcsPair> seq;
    int i = m, j = n;
    while (i > 0 && j > 0) {
        if (A[i-1] == B[j-1]) { seq.push_back({i-1, j-1}); --i; --j; }
        else if (dp[i-1][j] > dp[i][j-1]) --i;
        else --j;
    }
    std::reverse(seq.begin(), seq.end());
    return seq;
}

// ---------------------------------------------------------------------------
// Hunk: a changed region expressed as [baseStart, baseEnd) deleted from base
// and `changed` lines inserted from the other version.
// ---------------------------------------------------------------------------
struct Hunk {
    int   baseStart = 0;
    int   baseEnd   = 0;
    Lines changed;
};

std::vector<Hunk> DiffHunks(const Lines& base, const Lines& other) {
    const auto lcs = ComputeLcs(base, other);
    std::vector<Hunk> hunks;
    int bi = 0, oi = 0;
    for (const auto& p : lcs) {
        if (p.a > bi || p.b > oi) {
            Hunk h;
            h.baseStart = bi;
            h.baseEnd   = p.a;
            for (int k = oi; k < p.b; ++k) h.changed.push_back(other[k]);
            hunks.push_back(std::move(h));
        }
        bi = p.a + 1;
        oi = p.b + 1;
    }
    const int bEnd = static_cast<int>(base.size());
    const int oEnd = static_cast<int>(other.size());
    if (bi < bEnd || oi < oEnd) {
        Hunk h;
        h.baseStart = bi;
        h.baseEnd   = bEnd;
        for (int k = oi; k < oEnd; ++k) h.changed.push_back(other[k]);
        if (!(h.baseStart == h.baseEnd && h.changed.empty()))
            hunks.push_back(std::move(h));
    }
    return hunks;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
MergeResult ThreeWayMerge(const std::string& base, const std::string& mine, const std::string& theirs) {
    // Trivial fast-paths.
    if (mine == theirs)   return {true, mine};
    if (mine == base)     return {true, theirs};
    if (theirs == base)   return {true, mine};

    const Lines bLines = SplitLines(base);
    const Lines mLines = SplitLines(mine);
    const Lines tLines = SplitLines(theirs);

    const auto mHunks = DiffHunks(bLines, mLines);
    const auto tHunks = DiffHunks(bLines, tLines);

    Lines out;
    bool isClean = true;
    int cursor = 0;
    size_t mi = 0, ti = 0;
    const int bSize = static_cast<int>(bLines.size());

    const auto flushBase = [&](int upTo) {
        const int end = (std::min)(upTo, bSize);
        for (int k = cursor; k < end; ++k) out.push_back(bLines[k]);
        if (upTo > cursor) cursor = (std::min)(upTo, bSize);
    };

    while (mi < mHunks.size() || ti < tHunks.size()) {
        // Discard hunks whose base range was already consumed by a previous hunk.
        // These represent overlapping changes from different start positions — flag as conflict
        // but don't try to apply them (the previously-applied hunk already replaced that region).
        while (mi < mHunks.size() && mHunks[mi].baseEnd <= cursor) ++mi;
        while (ti < tHunks.size() && tHunks[ti].baseEnd <= cursor) ++ti;
        // If a hunk starts BEFORE cursor but ends after, the region partially overlaps — conflict.
        if (mi < mHunks.size() && mHunks[mi].baseStart < cursor) { isClean = false; ++mi; continue; }
        if (ti < tHunks.size() && tHunks[ti].baseStart < cursor) { isClean = false; ++ti; continue; }

        const bool mHas = mi < mHunks.size();
        const bool tHas = ti < tHunks.size();
        if (!mHas && !tHas) break;
        int nextBase = bSize;
        if (mHas) nextBase = (std::min)(nextBase, mHunks[mi].baseStart);
        if (tHas) nextBase = (std::min)(nextBase, tHunks[ti].baseStart);

        flushBase(nextBase);

        const Hunk* mh = (mHas && mHunks[mi].baseStart == nextBase) ? &mHunks[mi] : nullptr;
        const Hunk* th = (tHas && tHunks[ti].baseStart == nextBase) ? &tHunks[ti] : nullptr;

        if (mh && th) {
            if (mh->baseEnd == th->baseEnd && mh->changed == th->changed) {
                // Identical change on both sides — apply once.
                for (const auto& l : mh->changed) out.push_back(l);
                cursor = mh->baseEnd;
            } else {
                // True conflict — emit markers.
                isClean = false;
                out.push_back("<<<<<<< mine");
                for (const auto& l : mh->changed) out.push_back(l);
                out.push_back("=======");
                for (const auto& l : th->changed) out.push_back(l);
                out.push_back(">>>>>>> theirs");
                cursor = (std::max)(mh->baseEnd, th->baseEnd);
            }
            ++mi; ++ti;
        } else if (mh) {
            for (const auto& l : mh->changed) out.push_back(l);
            cursor = mh->baseEnd;
            ++mi;
        } else if (th) {
            for (const auto& l : th->changed) out.push_back(l);
            cursor = th->baseEnd;
            ++ti;
        }
    }
    flushBase(bSize);

    return {isClean, JoinLines(out)};
}

} // namespace TextMerge
