#ifndef NAVIGATION_HISTORY_H
#define NAVIGATION_HISTORY_H

#include <string>
#include <vector>

struct NavigationEntry {
    std::string Jql;
    // Future: add more fields (project key, board, etc.)
};

class NavigationHistory {
public:
    NavigationHistory();

    void Clear();

    bool CanGoBack() const;
    bool CanGoForward() const;

    // Push a new entry as the "current" one, trimming any forward history.
    void Push(const NavigationEntry& entry);

    // Move back/forward in history. Returns nullptr if movement is not possible.
    const NavigationEntry* GoBack();
    const NavigationEntry* GoForward();

    const NavigationEntry* Current() const;

private:
    std::vector<NavigationEntry> _entries;
    int _index;
};

#endif

