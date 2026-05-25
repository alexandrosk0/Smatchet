#include "LabelEditDiffPure.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace smatchet {
namespace github {

LabelEditDiff ComputeLabelEditDiff(const std::vector<std::string>& current,
                                   const std::vector<std::string>& intended) {
    // De-dupe both inputs into sorted sets — diffing two sets has O((n+m) log n)
    // complexity but the label-edit hot path is tiny (typical issue has < 10
    // labels). std::set keeps the sort + uniqueness invariants in one type.
    const std::set<std::string> cur(current.begin(), current.end());
    const std::set<std::string> ins(intended.begin(), intended.end());

    LabelEditDiff out;
    std::set_difference(ins.begin(), ins.end(), cur.begin(), cur.end(), std::back_inserter(out.ToAdd));
    std::set_difference(cur.begin(), cur.end(), ins.begin(), ins.end(), std::back_inserter(out.ToRemove));
    return out;
}

} // namespace github
} // namespace smatchet
