#ifndef SMATCHET_UI_GRID_UNDO_PURE_H
#define SMATCHET_UI_GRID_UNDO_PURE_H

// Pure undo history for grid cell writes (docs/plans/shipped/input-ux-w3-grid-spreadsheet.md, slice S3).
// Header-only, no ImGui, C++14. The edit pipeline pushes one Entry per grid write (a single edit) or per bulk job
// (one Entry, one CellChange per row sent), the commit results mark each change Landed or Failed, and an undo
// takes the entry and queues Inverse(entry) as ordinary edits. In memory only, bounded, UI thread only.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace grid {
namespace undo {

// Landed = saved online or queued offline.
enum class ChangeState { Pending, Landed, Failed };

struct CellChange {
    std::string IssueId;
    std::string FieldId;
    std::vector<std::string> Before;
    std::vector<std::string> After;
    std::string BeforeDisplay;
    // AfterDisplay is valid only when Landed; an empty one is a cleared field, not "unknown".
    std::string AfterDisplay;
    ChangeState State = ChangeState::Pending;
};

struct Entry {
    std::uint64_t Id = 0;
    std::string PaneId;
    std::vector<CellChange> Changes;
    std::uint64_t BulkJobId = 0;
};

// The oldest entry is dropped past this.
constexpr std::size_t kMaxEntries = 50;

class History {
  public:
    // Returns the entry id (stamped on the entry; ids start at 1 and never repeat).
    std::uint64_t Push(Entry e) {
        e.Id = ++m_lastId;
        const std::uint64_t id = e.Id;
        m_entries.push_back(std::move(e));
        if (m_entries.size() > kMaxEntries) {
            m_entries.erase(m_entries.begin());
        }
        return id;
    }

    // A commit result for one change of entry `id`: Landed records the display value after the local apply.
    // Unknown entry or change: no-op, false.
    bool MarkLanded(std::uint64_t id, const std::string& issueId, const std::string& fieldId,
                    const std::string& afterDisplay) {
        CellChange* c = FindChange(id, issueId, fieldId);
        if (c == nullptr) {
            return false;
        }
        c->State = ChangeState::Landed;
        c->AfterDisplay = afterDisplay;
        return true;
    }

    // Failed drops that change; an entry left with no change is removed.
    bool MarkFailed(std::uint64_t id, const std::string& issueId, const std::string& fieldId) {
        const std::vector<Entry>::iterator e = FindEntry(id);
        if (e == m_entries.end()) {
            return false;
        }
        for (std::vector<CellChange>::iterator c = e->Changes.begin(); c != e->Changes.end(); ++c) {
            if (c->IssueId == issueId && c->FieldId == fieldId) {
                e->Changes.erase(c);
                if (e->Changes.empty()) {
                    m_entries.erase(e);
                }
                return true;
            }
        }
        return false;
    }

    // Removes one entry (the toast's Undo button).
    bool Take(std::uint64_t id, Entry& out) {
        const std::vector<Entry>::iterator e = FindEntry(id);
        if (e == m_entries.end()) {
            return false;
        }
        out = std::move(*e);
        m_entries.erase(e);
        return true;
    }

    // Grid Ctrl+Z: the pane's newest entry. One that still has a Pending change is NOT taken, so the caller can say
    // "still saving" and keep it (an undo issued before the save lands would invert nothing).
    enum class TakeResult { Taken, StillSaving, Empty };
    TakeResult TakeNewest(const std::string& paneId, Entry& out) {
        for (std::size_t n = m_entries.size(); n > 0; --n) {
            Entry& e = m_entries[n - 1];
            if (e.PaneId != paneId) {
                continue;
            }
            for (std::size_t i = 0; i < e.Changes.size(); ++i) {
                if (e.Changes[i].State == ChangeState::Pending) {
                    return TakeResult::StillSaving;
                }
            }
            out = std::move(e);
            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(n - 1));
            return TakeResult::Taken;
        }
        return TakeResult::Empty;
    }

    // The pane's newest entry, in whatever state its changes are (the context menu names it); null when there is none.
    const Entry* PeekNewest(const std::string& paneId) const {
        for (std::size_t n = m_entries.size(); n > 0; --n) {
            if (m_entries[n - 1].PaneId == paneId) {
                return &m_entries[n - 1];
            }
        }
        return nullptr;
    }

    std::size_t Size() const { return m_entries.size(); }

    // True when `id` is still in the history (the toast's Undo checks before it acts).
    bool Contains(std::uint64_t id) const {
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].Id == id) {
                return true;
            }
        }
        return false;
    }

  private:
    std::vector<Entry>::iterator FindEntry(std::uint64_t id) {
        for (std::vector<Entry>::iterator e = m_entries.begin(); e != m_entries.end(); ++e) {
            if (e->Id == id) {
                return e;
            }
        }
        return m_entries.end();
    }

    CellChange* FindChange(std::uint64_t id, const std::string& issueId, const std::string& fieldId) {
        const std::vector<Entry>::iterator e = FindEntry(id);
        if (e == m_entries.end()) {
            return nullptr;
        }
        for (std::size_t i = 0; i < e->Changes.size(); ++i) {
            if (e->Changes[i].IssueId == issueId && e->Changes[i].FieldId == fieldId) {
                return &e->Changes[i];
            }
        }
        return nullptr;
    }

    std::vector<Entry> m_entries; // oldest first
    std::uint64_t m_lastId = 0;
};

// The inverse edits of an entry, newest change first. Each writes the change's Before values and carries its
// AfterDisplay (the value we wrote) as the ADR-0016 conflict base in BeforeDisplay, so a field someone else changed
// meanwhile reports a conflict instead of being overwritten. Only Landed changes are inverted.
inline std::vector<CellChange> Inverse(const Entry& e) {
    std::vector<CellChange> out;
    for (std::size_t i = e.Changes.size(); i > 0; --i) {
        const CellChange& c = e.Changes[i - 1];
        if (c.State != ChangeState::Landed) {
            continue;
        }
        CellChange inv;
        inv.IssueId = c.IssueId;
        inv.FieldId = c.FieldId;
        inv.Before = c.After;
        inv.After = c.Before;
        inv.BeforeDisplay = c.AfterDisplay;
        inv.AfterDisplay = c.BeforeDisplay;
        out.push_back(std::move(inv));
    }
    return out;
}

} // namespace undo
} // namespace grid
} // namespace smatchet

#endif // SMATCHET_UI_GRID_UNDO_PURE_H
