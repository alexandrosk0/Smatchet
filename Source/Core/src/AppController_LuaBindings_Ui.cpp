#include "AppControllerImpl.h" // AppController::Impl — cold sol2/automation member storage (pImpl #19b)
#include "ILuaBindingHost.h"
#include "LuaAutomationHost.h"
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls app_.Cache-> methods.

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AiAssistantController.h"
#include "AiLuaPromptRateLimit.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"
#include "LuaAutomationHookPolicyPure.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <exception>
#include <future>
#include <ghc/filesystem.hpp>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <iterator>

#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared AppController_LuaBindings-TU include prologue is grandfathered across the god-file-split siblings (AppController_LuaBindings.cpp / _Ui / _Ai / _Tickets) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared AppController_LuaBindings TU prologue header is introduced)
// clang-format on

#include "Json/BoundedJsonParse.h"

#include "imgui.h"
#include "Logger.h"
#include "SmatchetFieldIconRender.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"
#include "UiPerfMonitor.h"

#include "AppController_LuaBindings_detail.h"

namespace smatchet_lua_init_detail {

// imgui.* glue: each entry rejects when called inside a cached provider / window recording.
// `g_luaImmediateModeAllowed` is flipped false by LuaImmediateModeGuard around the
// `TryRenderCachedLuaField` provider call and the `DrawLuaWindows` record path; only
// event-time callbacks (`register_ticket_action`, `register_global_action`, MCP tools)
// leave it true. luaL_error unwinds via C++ exceptions thanks to the Lua-as-C++ build mode.
void ImGuiSameLineGlue(sol::this_state L) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    ImGui::SameLine();
}

void ImGuiSeparatorGlue(sol::this_state L) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    ImGui::Separator();
}

void ImGuiProgressBarGlue(sol::this_state L, float fraction, float width, float height) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    ImVec2 sz(width, height);
    if (width < 0.0f) {
        sz.x = ImGui::GetContentRegionAvail().x;
    }
    if (height <= 0.0f) {
        sz.y = ImGui::GetFrameHeight();
    }
    ImGui::ProgressBar(fraction, sz);
}

std::tuple<float, float> ImGuiGetContentRegionAvailGlue(sol::this_state L) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return std::make_tuple(0.0f, 0.0f);
    }
    const ImVec2 v = ImGui::GetContentRegionAvail();
    return std::make_tuple(v.x, v.y);
}

bool ImGuiButtonGlue(sol::this_state L, const std::string& label) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return false;
    }
    return ImGui::Button(label.c_str());
}

void LuaRegisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId, sol::function fn) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldDisplayCachedBind(fieldId, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedGlue(sol::this_state L, const std::string& fieldId) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldDisplayCachedBind(fieldId);
}

void LuaRegisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName, sol::function fn) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldDisplayCachedByNameBind(displayName, std::move(fn));
}

void LuaUnregisterFieldDisplayCachedByNameGlue(sol::this_state L, const std::string& displayName) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldDisplayCachedByNameBind(displayName);
}

void LuaUiInvalidateWindowGlue(sol::this_state L, const std::string& name) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->app_.LuaUiInvalidateWindowBind(name);
}

void LuaUiInvalidateFieldCacheGlue(sol::this_state L, sol::optional<std::string> ticketId,
                                   sol::optional<std::string> fieldId) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiInvalidateFieldCacheBind(ticketId, fieldId);
}

void LuaUiUnregisterWindowGlue(sol::this_state L, const std::string& name) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiUnregisterWindowBind(name);
}

void LuaRegisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::table map,
                                 sol::optional<bool> byName) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaRegisterFieldIconMapBind(fieldKey, std::move(map), byName);
}

void LuaUnregisterFieldIconMapGlue(sol::this_state L, const std::string& fieldKey, sol::optional<bool> byName) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUnregisterFieldIconMapBind(fieldKey, byName);
}

void LuaImGuiTextGlue(sol::this_state L, const std::string& s) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaImGuiTextBind(s);
}

void LuaImGuiTextUnformattedGlue(sol::this_state L, const std::string& s) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return;
    }
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaImGuiTextUnformattedBind(s);
}

bool LuaImGuiImageGlue(sol::this_state L, const std::string& path, float w, float h) {
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_luaImmediateModeAllowed) {
        luaL_error(L, kImmediateModeErrorMsg);
        return false;
    }
    AppController::Impl* app = ResolveApp(L);
    return app ? app->LuaImGuiImageBind(path, w, h) : false;
}

void LuaUiRegisterWindowGlue(sol::this_state L, const std::string& name, sol::function drawFn) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterWindowBind(name, std::move(drawFn));
}

void LuaUiRegisterTicketActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterTicketActionBind(name, cb);
}

void LuaUiRegisterGlobalActionGlue(sol::this_state L, const std::string& name, const std::string& cb) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->LuaUiRegisterGlobalActionBind(name, cb);
}

} // namespace smatchet_lua_init_detail

void AppController::Impl::LuaRegisterFieldDisplayCachedBind(const std::string& fieldId, sol::function fn) {
    if (fieldId.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayCachedProviders_[fieldId] = sol::protected_function(std::move(fn));
    // Bump invalidates every cached entry that holds a (possibly-stale) provider ref. Per-entry
    // input comparison in TryRenderCachedLuaField handles ordinary value changes; this is the
    // explicit "registration churn happened" channel.
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaUnregisterFieldDisplayCachedBind(const std::string& fieldId) {
    fieldDisplayCachedProviders_.erase(fieldId);
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaRegisterFieldDisplayCachedByNameBind(const std::string& displayName, sol::function fn) {
    if (displayName.empty() || !fn.valid()) {
        return;
    }
    fieldDisplayCachedProvidersByName_[AsciiLowerCopy(displayName)] = sol::protected_function(std::move(fn));
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaUnregisterFieldDisplayCachedByNameBind(const std::string& displayName) {
    fieldDisplayCachedProvidersByName_.erase(AsciiLowerCopy(displayName));
    app_.luaProviderGen_.fetch_add(1);
}

void AppController::Impl::LuaRegisterFieldIconMapBind(const std::string& fieldKey, sol::table map,
                                                      sol::optional<bool> byName) {
    if (fieldKey.empty() || !map.valid()) {
        return;
    }
    std::unordered_map<std::string, std::string> inner;
    map.for_each([&](sol::object kObj, sol::object vObj) {
        if (kObj.is<std::string>() && vObj.is<std::string>()) {
            inner[ToLowerAsciiCopy(kObj.as<std::string>())] = vObj.as<std::string>();
        }
    });
    std::lock_guard<std::mutex> lock(fieldIconMapsMutex_);
    if (byName.value_or(false)) {
        fieldIconMapsByDisplayName_[ToLowerAsciiCopy(fieldKey)] = std::move(inner);
    } else {
        fieldIconMapsByFieldId_[fieldKey] = std::move(inner);
    }
}

void AppController::Impl::LuaUnregisterFieldIconMapBind(const std::string& fieldKey, sol::optional<bool> byName) {
    std::lock_guard<std::mutex> lock(fieldIconMapsMutex_);
    if (byName.value_or(false)) {
        fieldIconMapsByDisplayName_.erase(ToLowerAsciiCopy(fieldKey));
    } else {
        fieldIconMapsByFieldId_.erase(fieldKey);
    }
}

void AppController::Impl::LuaImGuiTextBind(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

void AppController::Impl::LuaImGuiTextUnformattedBind(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

bool AppController::Impl::LuaImGuiImageBind(const std::string& path, float w, float h) {
    return SmatchetFieldIconRender::DrawImagePathOrUrl(app_, path, w, h);
}

void AppController::ApplyOrQueueLuaWindowOp(smatchet::lua::PendingLuaWindowOp op) {
    // UI-thread-only. See plan §Explicit window invalidation (F1).
    if (inDrawLuaWindows_) {
        impl_->pendingLuaWindowOps_.push_back(std::move(op));
        return;
    }
    switch (op.kind) {
    case smatchet::lua::PendingLuaWindowOp::Kind::Invalidate: {
        auto it = std::find_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                               [&](const smatchet::lua::LuaWindowEntry& w) { return w.name == op.name; });
        if (it != impl_->luaWindows_.end()) {
            it->dirty = true;
            it->hasError = false;
            it->errorMessage.clear();
            return;
        }
        break;
    }
    case smatchet::lua::PendingLuaWindowOp::Kind::Register: {
        auto it = std::find_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                               [&](const smatchet::lua::LuaWindowEntry& w) { return w.name == op.name; });
        if (it != impl_->luaWindows_.end())
            impl_->luaWindows_.erase(it);
        smatchet::lua::LuaWindowEntry e;
        e.name = op.name;
        e.drawFn = std::move(op.drawFn);
        e.dirty = true;
        impl_->luaWindows_.push_back(std::move(e));
        break;
    }
    case smatchet::lua::PendingLuaWindowOp::Kind::Unregister:
        impl_->luaWindows_.erase(
            std::remove_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                           [&](const smatchet::lua::LuaWindowEntry& w) { return w.name == op.name; }),
            impl_->luaWindows_.end());
        break;
    }
}

void AppController::Impl::LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn) {
    smatchet::lua::PendingLuaWindowOp op;
    op.kind = smatchet::lua::PendingLuaWindowOp::Kind::Register;
    op.name = name;
    if (drawFn.valid()) {
        op.drawFn = sol::protected_function(std::move(drawFn));
    }
    // F1 timing: on-UI uses the in-frame queue so a callback fired inside DrawLuaWindows
    // takes effect this frame, not the next. Off-UI hops the dispatcher first.
    if (app_.IsOnUiThread()) {
        app_.ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = &app_;
        app_.mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::Impl::LuaUiUnregisterWindowBind(const std::string& name) {
    smatchet::lua::PendingLuaWindowOp op;
    op.kind = smatchet::lua::PendingLuaWindowOp::Kind::Unregister;
    op.name = name;
    if (app_.IsOnUiThread()) {
        app_.ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = &app_;
        app_.mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::LuaUiInvalidateWindowBind(const std::string& name) {
    smatchet::lua::PendingLuaWindowOp op;
    op.kind = smatchet::lua::PendingLuaWindowOp::Kind::Invalidate;
    op.name = name;
    if (IsOnUiThread()) {
        ApplyOrQueueLuaWindowOp(std::move(op));
    } else {
        AppController* self = this;
        mainThreadDispatcher.PostToMainThread(
            [self, opCap = std::move(op)]() mutable { self->ApplyOrQueueLuaWindowOp(std::move(opCap)); });
    }
}

void AppController::Impl::LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId,
                                                        sol::optional<std::string> fieldId) {
    const std::string tid = ticketId.value_or(std::string());
    const std::string fid = fieldId.value_or(std::string());
    const bool hasTicket = static_cast<bool>(ticketId);
    const bool hasField = static_cast<bool>(fieldId);
    auto apply = [this, hasTicket, hasField, tid, fid]() {
        if (!hasTicket) {
            luaFieldCache_.clear();
            return;
        }
        if (!hasField) {
            for (auto it = luaFieldCache_.begin(); it != luaFieldCache_.end();) {
                const std::string& key = it->first;
                const std::size_t nul = key.find('\0');
                if (nul != std::string::npos && key.compare(0, nul, tid) == 0)
                    it = luaFieldCache_.erase(it);
                else
                    ++it;
            }
            return;
        }
        std::string key = tid;
        key.push_back('\0');
        key.append(fid);
        luaFieldCache_.erase(key);
    };
    if (app_.IsOnUiThread()) {
        apply();
    } else {
        app_.mainThreadDispatcher.PostToMainThread(std::move(apply));
    }
}

void AppController::NotifyLuaTicketDataChanged() { luaWindowDataGen_.fetch_add(1); }

void AppController::Impl::LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName) {
    std::lock_guard<std::mutex> lock(luaActionsMutex_);
    luaTicketActions_.erase(
        std::remove_if(luaTicketActions_.begin(), luaTicketActions_.end(),
                       [&](const std::pair<std::string, std::string>& p) { return p.first == name; }),
        luaTicketActions_.end());
    if (!callbackFuncName.empty()) {
        luaTicketActions_.push_back({name, callbackFuncName});
    }
}

void AppController::Impl::LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName) {
    {
        std::lock_guard<std::mutex> lock(luaActionsMutex_);
        luaGlobalActions_.erase(
            std::remove_if(luaGlobalActions_.begin(), luaGlobalActions_.end(),
                           [&](const std::pair<std::string, std::string>& p) { return p.first == name; }),
            luaGlobalActions_.end());
        if (!callbackFuncName.empty()) {
            luaGlobalActions_.push_back({name, callbackFuncName});
        }
    }
    // commandRegistry_ has its own internal locking; do NOT hold luaActionsMutex_ across this
    // call. ExecuteLuaGlobalAction's handler closure does not re-enter luaActionsMutex_.

    // Mirror into the unified command registry as a `lua.<name>` command so it is
    // discoverable via CLI / MCP / Palette without extra registration. See plan §Lua.
    if (app_.commandRegistry_) {
        const std::string cmdName = "lua." + name;
        // De-dup: if the action was already registered (e.g. script reloaded), remove the old one
        // from the registry. There is no `Unregister` API (registrations are permanent for safety),
        // so we skip re-registration when the exact name is already present.
        if (!app_.commandRegistry_->HasExact(cmdName) && !name.empty() && !callbackFuncName.empty()) {
            smatchet::cmd::Command c;
            c.Name = cmdName;
            c.Category = "lua";
            c.Summary = "(Lua) " + name;
            c.Description = "Lua global action registered via ui.register_global_action(\"" + name + "\", ...).";
            c.Idempotent = false; // Lua actions may mutate state
            c.AsyncSafe = true;
            // Capture by value so the handler owns a copy of the callback name string.
            const std::string cbName = callbackFuncName;
            AppController* appPtr = &app_;
            c.Handler = [appPtr, cbName](const nlohmann::json& /*args*/, const smatchet::cmd::CommandContext& /*ctx*/) {
                appPtr->ExecuteLuaGlobalAction(cbName);
                return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
            };
            try {
                app_.commandRegistry_->Register(std::move(c));
            } catch (const std::exception& ex) {
                LOG_WARN("LuaUiRegisterGlobalActionBind: could not register '%s' in registry: %s", cmdName.c_str(),
                         ex.what());
            }
        }
    }
}

