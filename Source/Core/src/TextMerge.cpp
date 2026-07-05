#include "TextMerge.h"

#include <algorithm>
#include <iterator>
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
        if (i > 0)
            out += '\n';
        out += lines[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// LCS — O(n*m) DP; sufficient for description-sized texts (< ~1000 lines).
// Returns matched (indexA, indexB) pairs in forward order.
// ---------------------------------------------------------------------------
struct LcsPair {
    int a;
    int b;
};

std::vector<LcsPair> ComputeLcs(const Lines& A, const Lines& B) {
    const int m = static_cast<int>(A.size());
    const int n = static_cast<int>(B.size());
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            dp[i][j] = (A[i - 1] == B[j - 1]) ? dp[i - 1][j - 1] + 1 : (std::max)(dp[i - 1][j], dp[i][j - 1]);
        }
    }
    std::vector<LcsPair> seq;
    int i = m, j = n;
    while (i > 0 && j > 0) {
        if (A[i - 1] == B[j - 1]) {
            seq.push_back({i - 1, j - 1});
            --i;
            --j;
        } else if (dp[i - 1][j] > dp[i][j - 1])
            --i;
        else
            --j;
    }
    std::reverse(seq.begin(), seq.end());
    return seq;
}

// ---------------------------------------------------------------------------
// Hunk: a changed region expressed as [baseStart, baseEnd) deleted from base
// and `changed` lines inserted from the other version.
// ---------------------------------------------------------------------------
struct Hunk {
    int baseStart = 0;
    int baseEnd = 0;
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
            h.baseEnd = p.a;
            for (int k = oi; k < p.b; ++k)
                h.changed.push_back(other[k]);
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
        h.baseEnd = bEnd;
        for (int k = oi; k < oEnd; ++k)
            h.changed.push_back(other[k]);
        if (!(h.baseStart == h.baseEnd && h.changed.empty()))
            hunks.push_back(std::move(h));
    }
    return hunks;
}

// ComputeLcs's backing `dp` matrix is O(n*m) ints — two ~30k-line texts (a pasted
// log, or a hostile/compromised tracker response reached via OfflineQueueService's
// 3-way merge on server-controlled `theirs`) allocate ~3.6 GB and run O(n*m) time.
// CPP_CODE_AUDIT.md #5. 4M cells caps the matrix at ~16 MB / a few ms.
constexpr size_t kMaxLcsCells = 4000000;

bool ExceedsLcsCellBudget(size_t a, size_t b) {
    if (a == 0 || b == 0) {
        return false;
    }
    return a > kMaxLcsCells / b; // overflow-safe form of `a * b > kMaxLcsCells`
}

// Whole-document conflict markers — the bail-out result when ExceedsLcsCellBudget trips
// (no line-level diff was computed, so there are no hunks to build partial output from).
std::string WholeDocumentConflictText(const std::string& mine, const std::string& theirs) {
    std::string conflictText = "<<<<<<< mine\n" + mine;
    if (!mine.empty() && mine.back() != '\n') {
        conflictText += '\n';
    }
    conflictText += "=======\n" + theirs;
    if (!theirs.empty() && theirs.back() != '\n') {
        conflictText += '\n';
    }
    conflictText += ">>>>>>> theirs";
    return conflictText;
}

// True when either base/mine or base/theirs would exceed ExceedsLcsCellBudget — the caller
// should return a whole-document conflict instead of running DiffHunks at all. Folds both
// budget checks + the OR into one call so ThreeWayMerge only spends a single branch on this.
bool ShouldBailOnLcsBudget(const Lines& bLines, const Lines& mLines, const Lines& tLines) {
    return ExceedsLcsCellBudget(bLines.size(), mLines.size()) || ExceedsLcsCellBudget(bLines.size(), tLines.size());
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
MergeResult ThreeWayMerge(const std::string& base, const std::string& mine, const std::string& theirs) {
    // Trivial fast-paths.
    if (mine == theirs)
        return {true, mine};
    if (mine == base)
        return {true, theirs};
    if (theirs == base)
        return {true, mine};

    const Lines bLines = SplitLines(base);
    const Lines mLines = SplitLines(mine);
    const Lines tLines = SplitLines(theirs);

    // Bail out as a conflict rather than running an O(n*m) LCS whose backing matrix
    // would blow up memory/time on large inputs — see ExceedsLcsCellBudget. Cheap to
    // check before either DiffHunks call actually allocates.
    if (ShouldBailOnLcsBudget(bLines, mLines, tLines)) {
        return {false, WholeDocumentConflictText(mine, theirs)};
    }

    const auto mHunks = DiffHunks(bLines, mLines);
    const auto tHunks = DiffHunks(bLines, tLines);

    Lines out;
    bool isClean = true;
    int cursor = 0;
    size_t mi = 0, ti = 0;
    const int bSize = static_cast<int>(bLines.size());

    const auto flushBase = [&](int upTo) {
        const int end = (std::min)(upTo, bSize);
        for (int k = cursor; k < end; ++k)
            out.push_back(bLines[k]);
        if (upTo > cursor)
            cursor = (std::min)(upTo, bSize);
    };

    while (mi < mHunks.size() || ti < tHunks.size()) {
        // Discard hunks whose base range was already consumed by a previous hunk.
        // These represent overlapping changes from different start positions — flag as conflict
        // but don't try to apply them (the previously-applied hunk already replaced that region).
        while (mi < mHunks.size() && mHunks[mi].baseEnd <= cursor)
            ++mi;
        while (ti < tHunks.size() && tHunks[ti].baseEnd <= cursor)
            ++ti;
        // If a hunk starts BEFORE cursor but ends after, the region partially overlaps — conflict.
        if (mi < mHunks.size() && mHunks[mi].baseStart < cursor) {
            isClean = false;
            ++mi;
            continue;
        }
        if (ti < tHunks.size() && tHunks[ti].baseStart < cursor) {
            isClean = false;
            ++ti;
            continue;
        }

        const bool mHas = mi < mHunks.size();
        const bool tHas = ti < tHunks.size();
        if (!mHas && !tHas)
            break;
        int nextBase = bSize;
        if (mHas)
            nextBase = (std::min)(nextBase, mHunks[mi].baseStart);
        if (tHas)
            nextBase = (std::min)(nextBase, tHunks[ti].baseStart);

        flushBase(nextBase);

        const Hunk* mh = (mHas && mHunks[mi].baseStart == nextBase) ? &mHunks[mi] : nullptr;
        const Hunk* th = (tHas && tHunks[ti].baseStart == nextBase) ? &tHunks[ti] : nullptr;

        if (mh && th) {
            if (mh->baseEnd == th->baseEnd && mh->changed == th->changed) {
                // Identical change on both sides — apply once.
                std::copy(mh->changed.begin(), mh->changed.end(), std::back_inserter(out));
                cursor = mh->baseEnd;
            } else {
                // True conflict — emit markers.
                isClean = false;
                out.push_back("<<<<<<< mine");
                std::copy(mh->changed.begin(), mh->changed.end(), std::back_inserter(out));
                out.push_back("=======");
                std::copy(th->changed.begin(), th->changed.end(), std::back_inserter(out));
                out.push_back(">>>>>>> theirs");
                cursor = (std::max)(mh->baseEnd, th->baseEnd);
            }
            ++mi;
            ++ti;
        } else if (mh) {
            std::copy(mh->changed.begin(), mh->changed.end(), std::back_inserter(out));
            cursor = mh->baseEnd;
            ++mi;
        } else if (th) {
            std::copy(th->changed.begin(), th->changed.end(), std::back_inserter(out));
            cursor = th->baseEnd;
            ++ti;
        }
    }
    flushBase(bSize);

    return {isClean, JoinLines(out)};
}

} // namespace TextMerge
