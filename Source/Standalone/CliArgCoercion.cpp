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

bool IsValidMcpPort(long long port) { return port >= 1 && port <= 65535; }

int ClampScenarioFrames(long long frames) {
    if (frames < 0) {
        return 0;
    }
    // The downstream wait-ms arithmetic must stay within int; cap well below the
    // overflow point. Two million frames is about 33 million ms, and an absurdly
    // long run of roughly 9 hours at 60 fps.
    constexpr long long kMaxScenarioFrames = 2000000;
    if (frames > kMaxScenarioFrames) {
        return static_cast<int>(kMaxScenarioFrames);
    }
    return static_cast<int>(frames);
}

int ScenarioFramesFromJson(const nlohmann::json& framesValue, int defaultFrames) {
    if (framesValue.is_number_integer()) {
        return ClampScenarioFrames(framesValue.get<long long>());
    }
    if (framesValue.is_number_float()) {
        return ClampScenarioFrames(static_cast<long long>(framesValue.get<double>()));
    }
    if (framesValue.is_string()) {
        try {
            return ClampScenarioFrames(std::stoll(framesValue.get<std::string>()));
        } catch (...) { // catch-all-ok: a malformed frames string falls back to the default
            return defaultFrames;
        }
    }
    return defaultFrames;
}

int ScenarioWaitMs(int timeoutMs, int frames) {
    return (timeoutMs > 0) ? timeoutMs : ((frames / 60 + 30) * 1000);
}

int ResolveCliTimeoutBudgetMs(int flagMs, int envMs) {
    if (flagMs >= 0) {
        return flagMs;
    }
    return (envMs > 0) ? envMs : 0;
}

} // namespace cli
} // namespace smatchet
