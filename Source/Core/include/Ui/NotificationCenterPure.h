#pragma once

// NotificationCenterPure — the STL-only formatting core of the Notification Center
// (ticket-change-monitor plan, S3 / § Files-to-modify #13a). The center window draws one row
// per ToastHistoryEntry; these helpers turn an entry's type + CreatedAt into the row's tag and
// timestamp text. Kept free of ImGui so the formatting is unit-testable without a UI session,
// mirroring ToastHistoryPure.h's split of the history ring's pure core from SmatchetToast.cpp.

#include <cstdint>
#include <string>

#include "ToastHistoryPure.h" // ToastType

namespace smatchet {

/// Short uppercase tag shown in the Notification Center's type column for a toast severity.
inline const char* ToastTypeShortLabel(ToastType type) {
    switch (type) {
    case ToastType::Success:
        return "OK";
    case ToastType::Warning:
        return "WARN";
    case ToastType::Error:
        return "ERR";
    case ToastType::Info:
    default:
        return "INFO";
    }
}

/// UTC wall-clock "HH:MM:SS" for `unixSeconds`. Deterministic + timezone-proof (no localtime),
/// so the row-timestamp formatting is unit-testable; negative / pre-epoch inputs wrap into the
/// day via floored modulo rather than producing a malformed string.
inline std::string FormatClockHMS(std::int64_t unixSeconds) {
    const std::int64_t secOfDay = ((unixSeconds % 86400) + 86400) % 86400;
    const int h = static_cast<int>(secOfDay / 3600);
    const int m = static_cast<int>((secOfDay % 3600) / 60);
    const int s = static_cast<int>(secOfDay % 60);
    char buf[9];
    buf[0] = static_cast<char>('0' + h / 10);
    buf[1] = static_cast<char>('0' + h % 10);
    buf[2] = ':';
    buf[3] = static_cast<char>('0' + m / 10);
    buf[4] = static_cast<char>('0' + m % 10);
    buf[5] = ':';
    buf[6] = static_cast<char>('0' + s / 10);
    buf[7] = static_cast<char>('0' + s % 10);
    buf[8] = '\0';
    return std::string(buf);
}

} // namespace smatchet
