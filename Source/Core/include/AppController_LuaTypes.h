#ifndef APP_CONTROLLER_LUA_TYPES_H
#define APP_CONTROLLER_LUA_TYPES_H

// Lua recorder + replay value types extracted out of AppController.h to keep
// that header under ~900 LOC. Types stay accessible as `AppController::ImCmd` /
// `AppController::LuaFieldCacheEntry` / `AppController::LuaWindowEntry` /
// `AppController::PendingLuaWindowOp` through `using` aliases declared inside
// the AppController class body. See docs/plans/shipped/large-files-and-phase-2.md § A5
// and docs/plans/shipped/lua-recorded-cmd-list.md.

#if defined(SMATCHET_WITH_LUA_AUTOMATION)

// sol2 must already be included by the consumer. AppController.h pulls in
// <sol/sol.hpp> before including this header (both guarded by the same
// SMATCHET_WITH_LUA_AUTOMATION macro). This header does not include sol2 itself
// to keep the include order documented and explicit at the call site.

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace lua {

/// One recorded ImGui draw command for cached Lua field/window replay. Built by
/// `LuaDrawList` in `AppController_LuaBindings.cpp`; replayed by `ReplayCmdList`.
struct ImCmd {
    enum class Op : std::uint8_t {
        Text,
        TextUnformatted,
        Image,
        ProgressBar,
        SameLine,
        Separator,
        Dummy,
        PushColor,
        PopColor,
        SetTooltip,
        Button,
        InputText
    };
    Op op;
    std::string str; // text / image path / tooltip / overlay / suffixed label
    float f1, f2, f3, f4;
    int i1; // pop count / input max_len
    sol::protected_function callback;
    // on_deactivated* attach in-place to the last interactive op at record time — replay
    // needs IsItemDeactivated* against the same ImGui item, so they can't live as
    // separate ImCmd entries.
    sol::protected_function onDeactivated;
    sol::protected_function onDeactivatedAfterEdit;
    std::vector<char> textBuf;
    ImCmd() : op(Op::Separator), f1(0), f2(0), f3(0), f4(0), i1(0) {}
    // Unregisters textBuf from the dictation router (CPP_CODE_AUDIT.md #21: non-static
    // registered buffers must unregister in their owner's destructor, not just on blur —
    // a cmd-list rebuild mid-focus would otherwise leave the router holding a freed
    // pointer). Defined in AppController_LuaBindings_Draw.cpp to keep the router include
    // out of this header. Copy/move stay defaulted: a moved-from textBuf is empty, so
    // vector reallocation never unregisters a live buffer.
    ~ImCmd();
    ImCmd(const ImCmd&) = default;
    ImCmd& operator=(const ImCmd&) = default;
    ImCmd(ImCmd&&) = default;
    ImCmd& operator=(ImCmd&&) = default;
};

struct LuaFieldCacheEntry {
    std::vector<ImCmd> cmds;
    std::string rawValue;
    std::string fieldName;
    int intAvailWidth;
    bool isReadOnly;
    std::uint64_t providerGen;
    bool handled;
    LuaFieldCacheEntry() : intAvailWidth(0), isReadOnly(false), providerGen(0), handled(false) {}
};

struct LuaWindowEntry {
    std::string name;
    sol::protected_function drawFn;
    std::vector<ImCmd> cmds;
    std::uint64_t cachedDataGen;
    std::uint64_t cachedProviderGen;
    bool dirty;
    bool hasError;
    std::string errorMessage;
    LuaWindowEntry() : cachedDataGen(0), cachedProviderGen(0), dirty(true), hasError(false) {}
};

// Mid-iteration safety: ops arriving during DrawLuaWindows are queued and drained after.
struct PendingLuaWindowOp {
    enum class Kind { Register, Unregister, Invalidate };
    Kind kind;
    std::string name;
    sol::protected_function drawFn;
};

} // namespace lua
} // namespace smatchet

#endif // SMATCHET_WITH_LUA_AUTOMATION

#endif // APP_CONTROLLER_LUA_TYPES_H
