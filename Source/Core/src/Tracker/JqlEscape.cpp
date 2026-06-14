#include "Tracker/JqlEscape.h"

namespace tracker_jql {

std::string QuoteLiteral(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

} // namespace tracker_jql
