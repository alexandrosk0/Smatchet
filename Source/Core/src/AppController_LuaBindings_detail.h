// AppController_LuaBindings_detail.h
// Private shared header between AppController_LuaBindings.cpp and
// AppController_LuaBindings_Draw.cpp. Not part of the public API — do not
// include from other TUs.
//
// All entities here have external linkage (constexpr, inline, or extern).
// They were previously in an anonymous namespace in the original monolithic TU;
// the split requires them to be visible across two TUs.

#pragma once

// AppControllerImpl.h pulls AppController.h + ILuaBindingHost.h (sol2) + AppController_LuaTypes.h
// (smatchet::lua::ImCmd etc.). After hardening #19c, sol2 + the recorder value types no longer
// live on AppController.h, so include the Impl header to get the full Lua surface.
#include "AppControllerImpl.h"
#include <nlohmann/json.hpp>
#include "Json/LuaJsonConvert.h" // JsonToLua / LuaToJson — shared inline leaf
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time constants
// ---------------------------------------------------------------------------

constexpr const char* kImmediateModeErrorMsg =
    "imgui.* not allowed inside cached provider / window — use draw:* instead";

// Recorder-method input-bounds caps per plan §Crash-safety hardening.
constexpr std::size_t kRecorderStringCap = 64u * 1024u;
constexpr std::size_t kRecorderLabelCap = 256u;
constexpr int kRecorderMaxLen = 65535;
constexpr float kRecorderSizeCap = 8192.0f;
constexpr float kRecorderOffsetCap = 4096.0f;

// ---------------------------------------------------------------------------
// Inline helpers
// ---------------------------------------------------------------------------

inline float ClampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool IsFiniteF(float v) { return std::isfinite(v); }

inline void TruncateInPlace(std::string& s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
}

// ---------------------------------------------------------------------------
// Thread-local immediate-mode gate
// Defined at file scope in AppController_LuaBindings.cpp (not anon namespace)
// so File 2 can reach it via this extern declaration.
// ---------------------------------------------------------------------------
extern thread_local bool g_luaImmediateModeAllowed;

// ---------------------------------------------------------------------------
// RAII guards
// ---------------------------------------------------------------------------

struct LuaImmediateModeGuard {
    bool prev;
    explicit LuaImmediateModeGuard(bool allow) : prev(g_luaImmediateModeAllowed) { g_luaImmediateModeAllowed = allow; }
    ~LuaImmediateModeGuard() { g_luaImmediateModeAllowed = prev; }
    LuaImmediateModeGuard(const LuaImmediateModeGuard&) = delete;
    LuaImmediateModeGuard& operator=(const LuaImmediateModeGuard&) = delete;
};

// RAII Lua instruction-count hook. Uniform count = 100000 per plan §LuaHookGuard (Q7).
struct LuaHookGuard {
    lua_State* L;
    explicit LuaHookGuard(sol::state& lua, int count = 100000) : L(lua.lua_state()) {
        lua_sethook(
            L, [](lua_State* s, lua_Debug*) { luaL_error(s, "Script execution timeout exceeded."); }, LUA_MASKCOUNT,
            count);
    }
    ~LuaHookGuard() { lua_sethook(L, nullptr, 0, 0); }
    LuaHookGuard(const LuaHookGuard&) = delete;
    LuaHookGuard& operator=(const LuaHookGuard&) = delete;
};

// ---------------------------------------------------------------------------
// LuaDrawList class declaration
// Method bodies live in AppController_LuaBindings_Draw.cpp's anonymous namespace.
// ---------------------------------------------------------------------------
class LuaDrawList {
  public:
    LuaDrawList() = default;
    LuaDrawList(const LuaDrawList&) = delete;
    LuaDrawList& operator=(const LuaDrawList&) = delete;
    LuaDrawList(LuaDrawList&&) = delete;
    LuaDrawList& operator=(LuaDrawList&&) = delete;

    void Text(std::string s);
    void TextUnformatted(std::string s);
    void Image(std::string path, float w, float h);
    void ProgressBar(float frac, float w, float h, sol::optional<std::string> overlay);
    void SameLine(sol::optional<float> offset, sol::optional<float> spacing);
    void Separator();
    void Dummy(float w, float h);
    void PushColor(int idx, float r, float g, float b, float a);
    void PopColor(sol::optional<int> count);
    void SetTooltip(std::string s);
    void Button(std::string label, sol::protected_function onClick);
    void InputText(std::string label, std::string initial, int maxLen, sol::protected_function onCommit);
    void OnDeactivated(sol::protected_function fn);
    void OnDeactivatedAfterEdit(sol::protected_function fn);

    void Deactivate() { active_ = false; }
    std::vector<smatchet::lua::ImCmd> Take() { return std::move(cmds_); }

  private:
    void RequireActive(const char* method) const;
    smatchet::lua::ImCmd* LastInteractive();

    std::vector<smatchet::lua::ImCmd> cmds_;
    int interactiveIndex_ = 0;
    bool active_ = true;
};

// ---------------------------------------------------------------------------
// Shared free functions (defined at file scope in AppController_LuaBindings.cpp)
// ---------------------------------------------------------------------------

bool LuaTruthy(const sol::object& o);
std::string AsciiLowerCopy(std::string s);
std::string TruncateForTrace(const std::string& s, std::size_t maxLen = 480);
// JsonToLua / LuaToJson now live in Json/LuaJsonConvert.h (included above).
sol::environment CreateSandboxEnvironment(sol::state& lua);
