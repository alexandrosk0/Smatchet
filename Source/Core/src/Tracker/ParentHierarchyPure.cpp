#include "ParentHierarchyPure.h"

#include "StringUtil.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace ParentHierarchyPure {

namespace {

using IndexById = std::unordered_map<std::string, std::size_t>;

IndexById BuildIndexById(const std::vector<CachedTicket>& tickets) {
    IndexById byId;
    byId.reserve(tickets.size());
    for (std::size_t i = 0; i < tickets.size(); ++i) {
        if (!tickets[i].id.empty()) {
            byId.emplace(tickets[i].id, i); // first occurrence wins on a duplicate id
        }
    }
    return byId;
}

/// Index of the PRESENT parent of `tickets[index]`, or `tickets.size()` when none.
std::size_t PresentParentIndex(const std::vector<CachedTicket>& tickets, const IndexById& byId, std::size_t index) {
    const std::string parentKey = ParentKeyOf(tickets[index]);
    if (parentKey.empty() || parentKey == tickets[index].id) {
        return tickets.size();
    }
    const auto it = byId.find(parentKey);
    return it == byId.end() ? tickets.size() : it->second;
}

/// Walk PRESENT ancestors of `index`, invoking `onAncestor(parentIndex)` nearest-first.
/// Stops at a root, a cycle, or `kMaxHierarchyDepth` steps.
template <typename Fn>
void WalkAncestors(const std::vector<CachedTicket>& tickets, const IndexById& byId, std::size_t index, Fn onAncestor) {
    std::unordered_set<std::size_t> visited;
    visited.insert(index);
    std::size_t current = index;
    for (int depth = 0; depth < kMaxHierarchyDepth; ++depth) {
        const std::size_t parentIndex = PresentParentIndex(tickets, byId, current);
        if (parentIndex >= tickets.size() || !visited.insert(parentIndex).second) {
            return; // root reached, or a cycle closed
        }
        onAncestor(parentIndex);
        current = parentIndex;
    }
}

} // namespace

std::string ParentKeyFromFieldValue(const std::string& raw) {
    std::string key = TrimCopy(raw);
    const std::size_t sep = key.find(" - ");
    if (sep != std::string::npos) {
        key.resize(sep);
        key = TrimCopy(key);
    }
    return key;
}

std::string ParentKeyOf(const CachedTicket& ticket) {
    return ParentKeyFromFieldValue(ticket.GetFieldValueRef("parent"));
}

std::vector<std::string> ReferencedParentIds(const std::vector<CachedTicket>& tickets) {
    std::vector<std::string> referenced;
    std::unordered_set<std::string> seen;
    for (const CachedTicket& ticket : tickets) {
        const std::string parentKey = ParentKeyOf(ticket);
        if (!parentKey.empty() && seen.insert(parentKey).second) {
            referenced.push_back(parentKey);
        }
    }
    return referenced;
}

std::vector<std::string> MissingParentKeys(const std::vector<CachedTicket>& tickets) {
    std::unordered_set<std::string> present;
    present.reserve(tickets.size());
    for (const CachedTicket& ticket : tickets) {
        present.insert(ticket.id);
    }
    std::vector<std::string> missing = ReferencedParentIds(tickets);
    missing.erase(std::remove_if(missing.begin(), missing.end(),
                                 [&present](const std::string& key) { return present.count(key) != 0; }),
                  missing.end());
    return missing;
}

std::unordered_set<std::string> PresentParentIds(const std::vector<CachedTicket>& tickets) {
    const IndexById byId = BuildIndexById(tickets);
    std::unordered_set<std::string> parents;
    for (std::size_t i = 0; i < tickets.size(); ++i) {
        const std::size_t parentIndex = PresentParentIndex(tickets, byId, i);
        if (parentIndex < tickets.size()) {
            parents.insert(tickets[parentIndex].id);
        }
    }
    return parents;
}

std::vector<std::size_t> AncestorChain(const std::vector<CachedTicket>& tickets, std::size_t index) {
    std::vector<std::size_t> chain;
    if (index >= tickets.size()) {
        return chain;
    }
    const IndexById byId = BuildIndexById(tickets);
    WalkAncestors(tickets, byId, index, [&chain](std::size_t parentIndex) { chain.push_back(parentIndex); });
    return chain;
}

std::vector<int> ComputeDepths(const std::vector<CachedTicket>& tickets) {
    const IndexById byId = BuildIndexById(tickets);
    std::vector<int> depths(tickets.size(), 0);
    for (std::size_t i = 0; i < tickets.size(); ++i) {
        int depth = 0;
        WalkAncestors(tickets, byId, i, [&depth](std::size_t) { ++depth; });
        depths[i] = depth;
    }
    return depths;
}

std::vector<std::size_t> StoryGroupOrder(const std::vector<CachedTicket>& tickets,
                                         const std::vector<std::size_t>& order) {
    const IndexById byId = BuildIndexById(tickets);
    std::vector<char> inOrder(tickets.size(), 0);
    for (std::size_t index : order) {
        if (index < tickets.size()) {
            inOrder[index] = 1;
        }
    }
    // children[p] = child indices of p in incoming (column-sorted) order; roots likewise.
    std::unordered_map<std::size_t, std::vector<std::size_t>> children;
    std::vector<std::size_t> roots;
    for (std::size_t index : order) {
        if (index >= tickets.size()) {
            continue;
        }
        const std::size_t parentIndex = PresentParentIndex(tickets, byId, index);
        if (parentIndex < tickets.size() && inOrder[parentIndex]) {
            children[parentIndex].push_back(index);
        } else {
            roots.push_back(index);
        }
    }

    std::vector<std::size_t> result;
    result.reserve(order.size());
    std::vector<char> emitted(tickets.size(), 0);
    // Explicit DFS stack (no recursion): frame = (index, next-child position).
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    auto emitTree = [&](std::size_t root) {
        if (emitted[root]) {
            return;
        }
        emitted[root] = 1;
        result.push_back(root);
        stack.clear();
        stack.emplace_back(root, 0);
        while (!stack.empty()) {
            std::pair<std::size_t, std::size_t>& frame = stack.back();
            const auto kidsIt = children.find(frame.first);
            if (kidsIt == children.end() || frame.second >= kidsIt->second.size() ||
                stack.size() > static_cast<std::size_t>(kMaxHierarchyDepth)) {
                stack.pop_back();
                continue;
            }
            const std::size_t child = kidsIt->second[frame.second++];
            if (!emitted[child]) {
                emitted[child] = 1;
                result.push_back(child);
                stack.emplace_back(child, 0);
            }
        }
    };
    for (std::size_t root : roots) {
        emitTree(root);
    }
    // Cycle members never reach a root; surface them (and their subtrees) in incoming order.
    for (std::size_t index : order) {
        if (index < tickets.size() && !emitted[index]) {
            emitTree(index);
        }
    }
    return result;
}

} // namespace ParentHierarchyPure
