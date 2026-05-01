#include "NavigationHistory.h"

NavigationHistory::NavigationHistory() : _entries(), _index(-1) {}

void NavigationHistory::Clear() {
    _entries.clear();
    _index = -1;
}

bool NavigationHistory::CanGoBack() const { return _index > 0; }

bool NavigationHistory::CanGoForward() const { return _index + 1 < static_cast<int>(_entries.size()); }

void NavigationHistory::Push(const NavigationEntry& entry) {
    if (_index >= 0 && _index < static_cast<int>(_entries.size()) && _entries[_index].Jql == entry.Jql) {
        return;
    }

    if (_index + 1 < static_cast<int>(_entries.size())) {
        _entries.erase(_entries.begin() + _index + 1, _entries.end());
    }

    _entries.push_back(entry);
    _index = static_cast<int>(_entries.size()) - 1;
}

const NavigationEntry* NavigationHistory::GoBack() {
    if (!CanGoBack())
        return nullptr;
    --_index;
    return &_entries[_index];
}

const NavigationEntry* NavigationHistory::GoForward() {
    if (!CanGoForward())
        return nullptr;
    ++_index;
    return &_entries[_index];
}

const NavigationEntry* NavigationHistory::Current() const {
    if (_index < 0 || _index >= static_cast<int>(_entries.size())) {
        return nullptr;
    }
    return &_entries[_index];
}
