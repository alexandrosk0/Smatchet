#include "CliArgCoercion.h"

namespace smatchet {
namespace cli {

namespace {

bool IsStrictInt(const std::string& s) {
    if (s.empty())
        return false;
    size_t i = 0;
    if (s[0] == '-') {
        if (s.size() == 1)
            return false;
        i = 1;
    }
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

bool IsStrictFloat(const std::string& s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-')
        ++i;
    const size_t intStart = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    if (i == intStart)
        return false;
    if (i >= s.size() || s[i] != '.')
        return false;
    ++i;
    const size_t fracStart = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    if (i == fracStart)
        return false;
    if (i < s.size()) {
        if (s[i] != 'e' && s[i] != 'E')
            return false;
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            ++i;
        const size_t expStart = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
            ++i;
        if (i == expStart)
            return false;
    }
    return i == s.size();
}

} // namespace

nlohmann::json CoerceCliArgValue(const std::string& raw) {
    if (raw == "true")
        return nlohmann::json(true);
    if (raw == "false")
        return nlohmann::json(false);

    if (IsStrictInt(raw)) {
        try {
            const long long n = std::stoll(raw);
            return nlohmann::json(n);
        } catch (...) {
            // Out of range — fall back to string so the user's literal survives.
            return nlohmann::json(raw);
        }
    }

    if (IsStrictFloat(raw)) {
        try {
            const double d = std::stod(raw);
            return nlohmann::json(d);
        } catch (...) {
            return nlohmann::json(raw);
        }
    }

    return nlohmann::json(raw);
}

} // namespace cli
} // namespace smatchet
