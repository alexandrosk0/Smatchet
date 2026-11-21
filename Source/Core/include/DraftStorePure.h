#pragma once

// DraftStorePure.h — the STL + nlohmann core of the draft store (docs/plans/input-ux-w2-drafts-undo.md):
// a bounded map of what the user has typed and not yet sent, keyed by (surface, scope), its JSON file
// format, and the decision of when the worker writes it. No threads, no file I/O and no ImGui, so it
// is unit-tested on every rig. DraftStore.h layers the shared store and its file worker on top.
//
// Surface names a text box ("comments", "longtext", ...); scope names what it is about (an issue, and
// the tracker site where the surface knows it). Base is what the field held when the draft started,
// for surfaces that can tell the user the saved text changed since.

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace smatchet {
namespace drafts {

struct DraftEntry {
    std::string Text;
    std::string Base;
    std::int64_t UpdatedUnixMs = 0;
};

/// Bounds on what the store keeps. An entry is its text, base and key bytes together.
struct DraftLimits {
    std::size_t MaxEntries = 200;
    std::size_t MaxEntryBytes = 160u * 1024u; // a full long-text buffer (64 KiB) plus its base, with room
    std::size_t MaxTotalBytes = 4u * 1024u * 1024u;
    std::size_t MaxSurfaceBytes = 64;
    std::size_t MaxScopeBytes = 512;
};

/// The file format version this build reads and writes.
constexpr int kDraftFileVersion = 1;
/// Largest drafts file read or parsed: the total cap plus room for JSON escaping.
constexpr std::size_t kMaxDraftFileBytes = 16u * 1024u * 1024u;

enum class PutResult : unsigned char {
    Stored,    ///< the entry was added or changed
    Unchanged, ///< the same text and base were already stored
    Erased,    ///< empty text: the entry was removed (or there was none)
    Rejected   ///< over a bound: not stored, and any older entry under the key was removed
};

/// What the user typed, per (surface, scope), within DraftLimits. Not thread-safe: the owner locks.
class DraftMap {
  public:
    explicit DraftMap(const DraftLimits& limits = DraftLimits()) : limits_(limits) {}

    const DraftLimits& Limits() const { return limits_; }

    /// Stores `text` for the key. Empty text erases. A key or entry over its bound is rejected, and an
    /// older entry under the key goes too: a shorter, stale draft must not come back later. Over the
    /// count or total bound, the least recently updated other entries are dropped first.
    PutResult Put(const std::string& surface, const std::string& scope, const std::string& text,
                  const std::string& base, std::int64_t nowUnixMs) {
        const Key key(surface, scope);
        if (text.empty()) {
            EraseKey(key);
            return PutResult::Erased;
        }
        if (!KeyFits(surface, scope) || EntryBytes(key, text, base) > limits_.MaxEntryBytes) {
            EraseKey(key);
            return PutResult::Rejected;
        }
        const auto it = entries_.find(key);
        if (it != entries_.end() && it->second.Text == text && it->second.Base == base) {
            return PutResult::Unchanged;
        }
        EraseKey(key);
        DraftEntry entry;
        entry.Text = text;
        entry.Base = base;
        entry.UpdatedUnixMs = nowUnixMs;
        totalBytes_ += EntryBytes(key, entry.Text, entry.Base);
        entries_.emplace(key, std::move(entry));
        EvictOver(key);
        return PutResult::Stored;
    }

    bool Get(const std::string& surface, const std::string& scope, DraftEntry& out) const {
        const auto it = entries_.find(Key(surface, scope));
        if (it == entries_.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    bool Contains(const std::string& surface, const std::string& scope) const {
        return entries_.count(Key(surface, scope)) != 0;
    }

    bool Erase(const std::string& surface, const std::string& scope) { return EraseKey(Key(surface, scope)); }

    /// Erases only while the stored text is still `text`: a post that comes back after the user typed
    /// something new must not take the new draft with it.
    bool EraseIfText(const std::string& surface, const std::string& scope, const std::string& text) {
        const auto it = entries_.find(Key(surface, scope));
        if (it == entries_.end() || it->second.Text != text) {
            return false;
        }
        return EraseKey(it->first);
    }

    std::size_t EraseSurface(const std::string& surface) {
        std::size_t erased = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.first == surface) {
                totalBytes_ -= EntryBytes(it->first, it->second.Text, it->second.Base);
                it = entries_.erase(it);
                ++erased;
            } else {
                ++it;
            }
        }
        return erased;
    }

    void Clear() {
        entries_.clear();
        totalBytes_ = 0;
    }

    std::size_t Size() const { return entries_.size(); }
    std::size_t TotalBytes() const { return totalBytes_; }

    /// Adds every entry of `loaded` whose key is not here yet. A draft typed in this session wins over
    /// the copy read from disk at start.
    void MergeUnder(const DraftMap& loaded) {
        for (const auto& kv : loaded.entries_) {
            if (entries_.count(kv.first) == 0) {
                Put(kv.first.first, kv.first.second, kv.second.Text, kv.second.Base, kv.second.UpdatedUnixMs);
            }
        }
    }

    /// The file image. Invalid UTF-8 in a draft is written as U+FFFD instead of throwing.
    std::string ToJson() const {
        nlohmann::json drafts = nlohmann::json::array();
        for (const auto& kv : entries_) {
            nlohmann::json row = nlohmann::json::object();
            row["surface"] = kv.first.first;
            row["scope"] = kv.first.second;
            row["text"] = kv.second.Text;
            if (!kv.second.Base.empty()) {
                row["base"] = kv.second.Base;
            }
            row["updated"] = kv.second.UpdatedUnixMs;
            drafts.push_back(std::move(row));
        }
        nlohmann::json root = nlohmann::json::object();
        root["version"] = kDraftFileVersion;
        root["drafts"] = std::move(drafts);
        return root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    /// Reads a file image into `out` (cleared first), through the same bounds as Put. False, with
    /// `err` set, when the text is not a drafts file this build reads; rows that are not well formed
    /// are skipped. Untrusted input: bounded parse, never throws.
    static bool FromJson(const std::string& text, const DraftLimits& limits, DraftMap& out, std::string& err) {
        out = DraftMap(limits);
        err.clear();
        std::string parseErr;
        const nlohmann::json root = json_safe::ParseBounded(text, parseErr, kMaxDraftFileBytes);
        if (!parseErr.empty()) {
            err = parseErr;
            return false;
        }
        if (!root.is_object() || !root.contains("version") || !root["version"].is_number_integer() ||
            root["version"].get<std::int64_t>() != kDraftFileVersion) {
            err = "not a version 1 drafts file";
            return false;
        }
        const auto drafts = root.find("drafts");
        if (drafts == root.end() || !drafts->is_array()) {
            err = "no drafts array";
            return false;
        }
        for (const nlohmann::json& row : *drafts) {
            ReadRow(row, out);
        }
        return true;
    }

  private:
    using Key = std::pair<std::string, std::string>;

    bool KeyFits(const std::string& surface, const std::string& scope) const {
        return !surface.empty() && surface.size() <= limits_.MaxSurfaceBytes && scope.size() <= limits_.MaxScopeBytes;
    }

    static std::size_t EntryBytes(const Key& key, const std::string& text, const std::string& base) {
        return key.first.size() + key.second.size() + text.size() + base.size();
    }

    bool EraseKey(const Key& key) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return false;
        }
        totalBytes_ -= EntryBytes(it->first, it->second.Text, it->second.Base);
        entries_.erase(it);
        return true;
    }

    /// Drops the least recently updated entries other than `keep` until both bounds hold.
    void EvictOver(const Key& keep) {
        while ((entries_.size() > limits_.MaxEntries || totalBytes_ > limits_.MaxTotalBytes) && entries_.size() > 1) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->first != keep &&
                    (oldest == entries_.end() || it->second.UpdatedUnixMs < oldest->second.UpdatedUnixMs)) {
                    oldest = it;
                }
            }
            if (oldest == entries_.end()) {
                return;
            }
            EraseKey(oldest->first);
        }
    }

    static bool IsString(const nlohmann::json& row, const char* name) {
        const auto it = row.find(name);
        return it != row.end() && it->is_string();
    }

    static void ReadRow(const nlohmann::json& row, DraftMap& out) {
        if (!row.is_object() || !IsString(row, "surface") || !IsString(row, "scope") || !IsString(row, "text")) {
            return;
        }
        const auto updated = row.find("updated");
        const std::int64_t updatedMs =
            (updated != row.end() && updated->is_number_integer()) ? updated->get<std::int64_t>() : 0;
        const std::string base = IsString(row, "base") ? row["base"].get<std::string>() : std::string();
        out.Put(row["surface"].get<std::string>(), row["scope"].get<std::string>(), row["text"].get<std::string>(),
                base, updatedMs);
    }

    DraftLimits limits_;
    std::map<Key, DraftEntry> entries_;
    std::size_t totalBytes_ = 0;
};

/// The worker writes the file once typing has paused for kDraftWriteQuiet, or once a change has waited
/// kDraftWriteMaxWait unsaved, so a long burst of typing still reaches the disk.
constexpr std::chrono::milliseconds kDraftWriteQuiet(750);
constexpr std::chrono::milliseconds kDraftWriteMaxWait(2000);

/// When a pending change becomes due (see DraftWriteDue).
inline std::chrono::steady_clock::time_point DraftWriteDueAt(std::chrono::steady_clock::time_point lastChange,
                                                             std::chrono::steady_clock::time_point firstUnsavedChange) {
    const std::chrono::steady_clock::time_point quietAt = lastChange + kDraftWriteQuiet;
    const std::chrono::steady_clock::time_point capAt = firstUnsavedChange + kDraftWriteMaxWait;
    return capAt < quietAt ? capAt : quietAt;
}

inline bool DraftWriteDue(bool dirty, std::chrono::steady_clock::time_point lastChange,
                          std::chrono::steady_clock::time_point firstUnsavedChange,
                          std::chrono::steady_clock::time_point now) {
    return dirty && now >= DraftWriteDueAt(lastChange, firstUnsavedChange);
}

} // namespace drafts
} // namespace smatchet
