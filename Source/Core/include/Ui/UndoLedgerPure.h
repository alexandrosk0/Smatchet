#pragma once

// UndoLedgerPure.h — pending undoable actions behind the toast's Undo button
// (docs/plans/input-ux-w2-drafts-undo.md; INPUT_UX_AUDIT.md § 4.3 / #90). STL only, no ImGui, so the
// settle rules are unit-tested without a UI session.
//
// An action that can be undone is done at once and leaves an entry here. The entry settles exactly
// once: Undo runs its undo callback, and every other ending (the toast expired, was dismissed or was
// pushed out, the app is closing) runs its commit callback. A callback runs after its entry is removed,
// so it may add, undo or commit other entries. UI thread only.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace smatchet {
namespace undo {

class UndoLedger {
  public:
    /// Entries kept at once; adding past this commits the oldest first.
    /// (A function, not a static constexpr member: C++14 would need an out-of-line definition for a
    /// member a test binds by reference.)
    static std::size_t MaxEntries() { return 32; }

    /// Records an action. Either callback may be empty. Returns its id (never 0).
    std::uint64_t Add(std::function<void()> undo, std::function<void()> commit) {
        while (entries_.size() >= MaxEntries()) {
            Commit(entries_.front().Id);
        }
        Entry e;
        e.Id = ++lastId_;
        e.UndoFn = std::move(undo);
        e.CommitFn = std::move(commit);
        entries_.push_back(std::move(e));
        return lastId_;
    }

    /// Runs the undo of `id` and forgets it. False when `id` already settled.
    bool Undo(std::uint64_t id) {
        Entry e;
        if (!Take(id, e)) {
            return false;
        }
        if (e.UndoFn) {
            e.UndoFn();
        }
        return true;
    }

    /// Runs the commit of `id` and forgets it. False when `id` already settled.
    bool Commit(std::uint64_t id) {
        Entry e;
        if (!Take(id, e)) {
            return false;
        }
        if (e.CommitFn) {
            e.CommitFn();
        }
        return true;
    }

    /// Commits everything still pending, oldest first (shutdown).
    void CommitAll() {
        while (!entries_.empty()) {
            Commit(entries_.front().Id);
        }
    }

    bool Contains(std::uint64_t id) const {
        for (const Entry& e : entries_) {
            if (e.Id == id) {
                return true;
            }
        }
        return false;
    }

    std::size_t Size() const { return entries_.size(); }

  private:
    struct Entry {
        std::uint64_t Id = 0;
        std::function<void()> UndoFn;
        std::function<void()> CommitFn;
    };

    bool Take(std::uint64_t id, Entry& out) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->Id == id) {
                out = std::move(*it);
                entries_.erase(it);
                return true;
            }
        }
        return false;
    }

    std::vector<Entry> entries_;
    std::uint64_t lastId_ = 0;
};

} // namespace undo
} // namespace smatchet
