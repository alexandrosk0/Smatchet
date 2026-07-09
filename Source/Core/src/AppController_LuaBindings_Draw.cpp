// AppController_LuaBindings_Draw.cpp
// Split from AppController_LuaBindings.cpp (original lines 1484–2590).
// Contains: LuaDrawList method bodies, ReplayCmdList, and the AppController
// methods that replay / manage cached Lua draw lists and run Lua snippets.
//
// Shared entities (LuaDrawList class decl, constants, guards, helpers) come
// from AppController_LuaBindings_detail.h, which also declares the file-scope
// functions defined in AppController_LuaBindings.cpp.

#include "AppController.h"
#include "AppControllerImpl.h" // AppController::Impl — cold sol2/automation member storage (pImpl #19b)
#include "ILuaBindingHost.h"
#include "LuaAutomationHost.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"

#include <algorithm>
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

// Include-list scaffold — dup_audit token-normalizes every include to the same
// `# include LIT` run, so touching this block re-hashes a maximal run that matches
// sibling AppController TUs' include blocks. Idiomatic boilerplate, not shared logic.
// SMATCHET_DEVIATION(rule=duplication; reason=include-block scaffold clone; owner=lua-automation; revisit=2026-12-31)
#include <nlohmann/json.hpp>

#include "Json/BoundedJsonParse.h"

#include "DictationInsertionRouter.h"
#include "imgui.h"
#include "Logger.h"
#include "LuaImagePathPolicyPure.h"
#include "SmatchetFieldIconRender.h"
#include "SmatchetLocalizedImGui.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"
#include "UiPerfMonitor.h"

#include "AppController_LuaBindings_detail.h"

namespace smatchet {
namespace lua {

// See the declaration comment in AppController_LuaTypes.h — blur-time unregister alone
// leaves a dangling router entry when the owning cmd list is rebuilt while the widget
// holds focus. Unregistering a never-registered buffer is a cheap no-op scan.
ImCmd::~ImCmd() {
    if (op == Op::InputText && !textBuf.empty()) {
        g_dictationRouter.UnregisterInputText(textBuf.data());
    }
}

} // namespace lua
} // namespace smatchet

// LuaDrawList method definitions — class declared in AppController_LuaBindings_detail.h.
// See docs/plans/shipped/lua-recorded-cmd-list.md § Crash-safety hardening for per-method
// input bounds; out-of-line so the class is complete-typed before InitLuaUi registers it.
// LuaDrawList is declared at global scope (in the private header), so its method bodies
// must also be at global scope — not inside an anonymous namespace.

void LuaDrawList::RequireActive(const char* method) const {
    if (!active_) {
        // sol2 catches std::exception thrown from C++ inside a Lua pcall frame and surfaces
        // it as a Lua error. The Lua-as-C++ build mode (CMakeLists.txt LANGUAGE CXX) makes
        // this unwinding safe through the recorder's std::vector / std::string temporaries.
        std::string msg = "draw:";
        msg += method;
        msg += " called outside its recording window";
        throw std::runtime_error(msg);
    }
}

smatchet::lua::ImCmd* LuaDrawList::LastInteractive() {
    auto it = std::find_if(cmds_.rbegin(), cmds_.rend(), [](const smatchet::lua::ImCmd& c) {
        return c.op == smatchet::lua::ImCmd::Op::Button || c.op == smatchet::lua::ImCmd::Op::InputText;
    });
    return it == cmds_.rend() ? nullptr : &(*it);
}

void LuaDrawList::Text(std::string s) {
    RequireActive("text");
    TruncateInPlace(s, kRecorderStringCap);
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::Text;
    c.str = std::move(s);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::TextUnformatted(std::string s) {
    RequireActive("text_unformatted");
    TruncateInPlace(s, kRecorderStringCap);
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::TextUnformatted;
    c.str = std::move(s);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Image(std::string path, float w, float h) {
    RequireActive("image");
    if (path.empty())
        return;
    // SECURITY: a Lua-console script must not be able to drive an arbitrary outbound HTTP GET
    // (SSRF / exfil via crafted URL / tracking pixel). The replay path (DrawImagePathOrUrl ->
    // ResolveFieldIconAssetPath) passes http(s) URLs straight to HttpGetBinary, so explicitly
    // refuse http(s) here at the binding boundary. Only non-http(s) local asset paths are allowed
    // for imgui.image; internal field-icon-map / priority-icon URL fetches are unaffected (they
    // never route through this Lua binding). Refusal is explicit + logged, not a silent drop.
    if (!LuaImagePathPolicyPure::IsAllowedLuaImagePath(path.c_str())) {
        LOG_WARN("Lua imgui.image: refused http(s) URL '%s' — only local asset paths are permitted from scripts.",
                 path.c_str());
        return;
    }
    if (!IsFiniteF(w) || !IsFiniteF(h) || w < 0.0f || h < 0.0f || w > kRecorderSizeCap || h > kRecorderSizeCap)
        return;
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::Image;
    c.str = std::move(path);
    c.f1 = w;
    c.f2 = h;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::ProgressBar(float frac, float w, float h, sol::optional<std::string> overlay) {
    RequireActive("progress_bar");
    if (!IsFiniteF(frac))
        frac = 0.0f;
    frac = ClampFloat(frac, 0.0f, 1.0f);
    if (!IsFiniteF(w) || !IsFiniteF(h))
        return;
    // Negative w / h means "auto"; ImGui handles them at replay.
    if (w < -1.0f || w > kRecorderSizeCap)
        return;
    if (h < -1.0f || h > kRecorderSizeCap)
        return;
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::ProgressBar;
    c.f1 = frac;
    c.f2 = w;
    c.f3 = h;
    if (overlay) {
        std::string ov = overlay.value();
        TruncateInPlace(ov, kRecorderStringCap);
        c.str = std::move(ov);
    }
    cmds_.push_back(std::move(c));
}

void LuaDrawList::SameLine(sol::optional<float> offset, sol::optional<float> spacing) {
    RequireActive("same_line");
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::SameLine;
    const float off = offset ? offset.value() : 0.0f;
    const float sp = spacing ? spacing.value() : -1.0f;
    c.f1 = IsFiniteF(off) ? ClampFloat(off, -kRecorderOffsetCap, kRecorderOffsetCap) : 0.0f;
    c.f2 = IsFiniteF(sp) ? ClampFloat(sp, -kRecorderOffsetCap, kRecorderOffsetCap) : -1.0f;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Separator() {
    RequireActive("separator");
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::Separator;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Dummy(float w, float h) {
    RequireActive("dummy");
    if (!IsFiniteF(w) || !IsFiniteF(h) || w < 0.0f || h < 0.0f || w > kRecorderSizeCap || h > kRecorderSizeCap)
        return;
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::Dummy;
    c.f1 = w;
    c.f2 = h;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::PushColor(int idx, float r, float g, float b, float a) {
    RequireActive("push_color");
    if (idx < 0 || idx >= ImGuiCol_COUNT)
        return;
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::PushColor;
    c.i1 = idx;
    c.f1 = ClampFloat(IsFiniteF(r) ? r : 0.0f, 0.0f, 1.0f);
    c.f2 = ClampFloat(IsFiniteF(g) ? g : 0.0f, 0.0f, 1.0f);
    c.f3 = ClampFloat(IsFiniteF(b) ? b : 0.0f, 0.0f, 1.0f);
    c.f4 = ClampFloat(IsFiniteF(a) ? a : 1.0f, 0.0f, 1.0f);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::PopColor(sol::optional<int> count) {
    RequireActive("pop_color");
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::PopColor;
    c.i1 = count.value_or(1);
    if (c.i1 <= 0)
        c.i1 = 1;
    cmds_.push_back(std::move(c));
}

void LuaDrawList::SetTooltip(std::string s) {
    RequireActive("set_tooltip");
    TruncateInPlace(s, kRecorderStringCap);
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::SetTooltip;
    c.str = std::move(s);
    cmds_.push_back(std::move(c));
}

void LuaDrawList::Button(std::string label, sol::protected_function onClick) {
    RequireActive("button");
    TruncateInPlace(label, kRecorderLabelCap);
    // Auto-disambiguate IDs across multiple interactive widgets in the same recording.
    // The cell / window outer ID scope handles cross-cell / cross-window collisions.
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::Button;
    c.str = std::move(label);
    c.str.append("##b");
    c.str.append(std::to_string(interactiveIndex_++));
    if (onClick.valid()) {
        c.callback = std::move(onClick);
    }
    cmds_.push_back(std::move(c));
}

void LuaDrawList::InputText(std::string label, std::string initial, int maxLen, sol::protected_function onCommit) {
    RequireActive("input_text");
    TruncateInPlace(label, kRecorderLabelCap);
    if (maxLen <= 0)
        maxLen = 256;
    if (maxLen > kRecorderMaxLen)
        maxLen = kRecorderMaxLen;
    if (static_cast<int>(initial.size()) > maxLen)
        initial.resize(maxLen);
    smatchet::lua::ImCmd c;
    c.op = smatchet::lua::ImCmd::Op::InputText;
    c.str = std::move(label);
    c.str.append("##it");
    c.str.append(std::to_string(interactiveIndex_++));
    c.i1 = maxLen;
    c.textBuf.assign(maxLen + 1, '\0');
    if (!initial.empty()) {
        const std::size_t n = std::min<std::size_t>(initial.size(), c.textBuf.size() - 1);
        std::memcpy(c.textBuf.data(), initial.data(), n);
    }
    if (onCommit.valid()) {
        c.callback = std::move(onCommit);
    }
    cmds_.push_back(std::move(c));
}

void LuaDrawList::OnDeactivated(sol::protected_function fn) {
    RequireActive("on_deactivated");
    smatchet::lua::ImCmd* last = LastInteractive();
    if (!last) {
        LOG_WARN("LuaDrawList::on_deactivated called with no prior interactive op; ignoring");
        return;
    }
    if (fn.valid())
        last->onDeactivated = std::move(fn);
}

void LuaDrawList::OnDeactivatedAfterEdit(sol::protected_function fn) {
    RequireActive("on_deactivated_after_edit");
    smatchet::lua::ImCmd* last = LastInteractive();
    if (!last) {
        LOG_WARN("LuaDrawList::on_deactivated_after_edit called with no prior interactive op; ignoring");
        return;
    }
    if (fn.valid())
        last->onDeactivatedAfterEdit = std::move(fn);
}

namespace {

enum class LuaReplayCallbackKind : std::uint8_t {
    None = 0,
    Click = 1u << 0,
    Commit = 1u << 1,
    Deactivated = 1u << 2,
};

void InvokeLuaCallbackSandboxed(sol::state& lua, sol::protected_function& fn, const std::string& cbArg1,
                                const std::string& cbArg2) {
    if (!fn.valid())
        return;
    try {
        LuaHookGuard hook(lua);
        sol::protected_function_result r = fn(cbArg1, cbArg2);
        if (!r.valid()) {
            sol::error e = r;
            LOG_WARN("Lua callback error: %s", e.what());
        }
    } catch (const std::exception& ex) {
        LOG_WARN("Lua callback C++ exception: %s", ex.what());
    } catch (...) {
        LOG_WARN("Lua callback unknown C++ exception");
    }
}

void InvokeLuaCallbackSandboxed3(sol::state& lua, sol::protected_function& fn, const std::string& cbArg1,
                                 const std::string& cbArg2, const std::string& cbArg3) {
    if (!fn.valid())
        return;
    try {
        LuaHookGuard hook(lua);
        sol::protected_function_result r = fn(cbArg1, cbArg2, cbArg3);
        if (!r.valid()) {
            sol::error e = r;
            LOG_WARN("Lua callback error: %s", e.what());
        }
    } catch (const std::exception& ex) {
        LOG_WARN("Lua callback C++ exception: %s", ex.what());
    } catch (...) {
        LOG_WARN("Lua callback unknown C++ exception");
    }
}

// Shared mutable state threaded through one replay pass. `pushed` tracks the running
// style-colour stack depth so the orchestrator can unwind any unbalanced colours at the
// end; `fired` accumulates the LuaReplayCallbackKind bitset returned to the caller.
struct ReplayCtx {
    AppController& app;
    sol::state& lua;
    const std::string& cbArg1;
    const std::string& cbArg2;
    bool isReadOnly;
    int pushed;
    std::uint8_t fired;
};

// on_deactivated / on_deactivated_after_edit fire identically for Button and InputText
// against the same just-submitted ImGui item. Centralised so both handlers stay in lockstep.
void ReplayFireDeactivationCallbacks(ReplayCtx& ctx, smatchet::lua::ImCmd& c) {
    using K = LuaReplayCallbackKind;
    if (c.onDeactivated && ImGui::IsItemDeactivated()) {
        InvokeLuaCallbackSandboxed(ctx.lua, c.onDeactivated, ctx.cbArg1, ctx.cbArg2);
        ctx.fired |= static_cast<std::uint8_t>(K::Deactivated);
    }
    if (c.onDeactivatedAfterEdit && ImGui::IsItemDeactivatedAfterEdit()) {
        InvokeLuaCallbackSandboxed(ctx.lua, c.onDeactivatedAfterEdit, ctx.cbArg1, ctx.cbArg2);
        ctx.fired |= static_cast<std::uint8_t>(K::Deactivated);
    }
}

void ReplayProgressBar(const smatchet::lua::ImCmd& c) {
    ImVec2 sz(c.f2, c.f3);
    if (c.f2 < 0.0f)
        sz.x = ImGui::GetContentRegionAvail().x;
    if (c.f3 <= 0.0f)
        sz.y = ImGui::GetFrameHeight();
    ImGui::ProgressBar(c.f1, sz, c.str.empty() ? nullptr : c.str.c_str());
}

void ReplayPushColor(ReplayCtx& ctx, const smatchet::lua::ImCmd& c) {
    if (c.i1 < 0 || c.i1 >= ImGuiCol_COUNT)
        return;
    ImGui::PushStyleColor(c.i1, ImVec4(c.f1, c.f2, c.f3, c.f4));
    ++ctx.pushed;
}

void ReplayPopColor(ReplayCtx& ctx, const smatchet::lua::ImCmd& c) {
    int n = c.i1 > 0 ? c.i1 : 1;
    if (n > ctx.pushed)
        n = ctx.pushed;
    if (n > 0) {
        ImGui::PopStyleColor(n);
        ctx.pushed -= n;
    }
}

void ReplayButton(ReplayCtx& ctx, smatchet::lua::ImCmd& c) {
    using K = LuaReplayCallbackKind;
    // Read-only cells get visually disabled + no callback firing. Wrap the
    // call in BeginDisabled so click events are suppressed at the ImGui layer
    // (no input routing, no hover-active styling) and we don't have to
    // double-check IsItemDeactivated* — the disabled item still reports
    // deactivation when focus moves on, so explicit isReadOnly guards remain.
    if (ctx.isReadOnly)
        ImGui::BeginDisabled();
    const bool clicked = ImGui::Button(c.str.c_str());
    if (ctx.isReadOnly)
        ImGui::EndDisabled();
    if (!ctx.isReadOnly) {
        if (clicked && c.callback) {
            InvokeLuaCallbackSandboxed(ctx.lua, c.callback, ctx.cbArg1, ctx.cbArg2);
            ctx.fired |= static_cast<std::uint8_t>(K::Click);
        }
        ReplayFireDeactivationCallbacks(ctx, c);
    }
}

void ReplayInputText(ReplayCtx& ctx, smatchet::lua::ImCmd& c) {
    using K = LuaReplayCallbackKind;
    if (c.textBuf.empty())
        return;
    // Encode read_only into the ImGui call directly (input is rejected at the
    // text widget layer, no need to skip-render). Callbacks are still gated:
    // a ReadOnly InputText never fires IsItemDeactivatedAfterEdit-with-commit
    // in practice, but the explicit guard keeps the invariant tight even if
    // a future ImGui revision changes that behaviour.
    const ImGuiInputTextFlags flags = ctx.isReadOnly ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None;
    ImGui::InputText(c.str.c_str(), c.textBuf.data(), c.textBuf.size(), flags);
    if (!ctx.isReadOnly) {
        // Dictation parity with the SmatchetLocalizedImGui wrappers (whisper Phase D):
        // register the replay buffer while the widget is focused, unregister on blur.
        // ReadOnly widgets are skipped — a router splice writes into the buffer
        // directly and would bypass the ReadOnly flag. ~ImCmd unregisters on
        // cmd-list rebuild so the router never holds a freed textBuf pointer.
        SmatchetLocalizedImGui::HookDictationOnLastItem(c.textBuf.data(), c.textBuf.size());
        if (ImGui::IsItemDeactivatedAfterEdit() && c.callback) {
            InvokeLuaCallbackSandboxed3(ctx.lua, c.callback, ctx.cbArg1, ctx.cbArg2, std::string(c.textBuf.data()));
            ctx.fired |= static_cast<std::uint8_t>(K::Commit);
        }
        ReplayFireDeactivationCallbacks(ctx, c);
    }
}

// Dispatch one recorded command to ImGui. Kept as a thin per-Op switch so the orchestrator
// loop stays well under the branch cap; each multi-branch widget delegates to a handler.
void ReplayOne(ReplayCtx& ctx, smatchet::lua::ImCmd& c) {
    using Op = smatchet::lua::ImCmd::Op;
    switch (c.op) {
    case Op::Text:
        ImGui::Text("%s", c.str.c_str());
        break;
    case Op::TextUnformatted:
        ImGui::TextUnformatted(c.str.c_str());
        break;
    case Op::Image:
        SmatchetFieldIconRender::DrawImagePathOrUrl(ctx.app, c.str, c.f1, c.f2);
        break;
    case Op::ProgressBar:
        ReplayProgressBar(c);
        break;
    case Op::SameLine:
        ImGui::SameLine(c.f1, c.f2);
        break;
    case Op::Separator:
        ImGui::Separator();
        break;
    case Op::Dummy:
        ImGui::Dummy(ImVec2(c.f1, c.f2));
        break;
    case Op::PushColor:
        ReplayPushColor(ctx, c);
        break;
    case Op::PopColor:
        ReplayPopColor(ctx, c);
        break;
    case Op::SetTooltip:
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", c.str.c_str());
        break;
    case Op::Button:
        ReplayButton(ctx, c);
        break;
    case Op::InputText:
        ReplayInputText(ctx, c);
        break;
    }
}

std::uint8_t ReplayCmdList(std::vector<smatchet::lua::ImCmd>& cmds, AppController& app, sol::state& lua,
                           const std::string& cbArg1, const std::string& cbArg2, bool isReadOnly) {
    SMATCHET_UI_PERF_SCOPE("LuaDrawList::Replay");
    ReplayCtx ctx{app, lua, cbArg1, cbArg2, isReadOnly, 0, 0};
    try {
        for (smatchet::lua::ImCmd& c : cmds) {
            ReplayOne(ctx, c);
        }
    } catch (const std::exception& ex) {
        LOG_WARN("ReplayCmdList: C++ exception aborted replay: %s", ex.what());
    } catch (...) {
        LOG_WARN("ReplayCmdList: unknown C++ exception aborted replay");
    }
    if (ctx.pushed > 0)
        ImGui::PopStyleColor(ctx.pushed);
    return ctx.fired;
}

// Invoke a cached-field provider on the recorder `rec`, returning call status via out-params.
// `callOk` is false on a C++ exception, `callValid` false on a Lua error; `truthy` is the
// provider's first return value coerced to bool. Mirrors the original inline try-block.
void InvokeCachedFieldProvider(sol::state& lua, sol::protected_function& providerCopy, const std::string& issueId,
                               const std::string& fieldId, const std::string& rawValue, float availWidth,
                               bool isReadOnly, const sol::object& fieldNameObj,
                               const std::shared_ptr<LuaDrawList>& rec, bool& callOk, bool& callValid, bool& truthy,
                               std::string& errMsg) {
    // `res` stays inside the try-block — see DrawLuaWindows above for the
    // -Wmaybe-uninitialized rationale (sol.hpp:12398 dtor reads L/index/popcount).
    // Provider return is also extracted inside the try so the success-branch below
    // doesn't need `res` to be in scope.
    try {
        LuaHookGuard hook(lua);
        LuaImmediateModeGuard imm(false);
        sol::protected_function_result res =
            providerCopy(issueId, fieldId, rawValue, availWidth, isReadOnly, fieldNameObj, rec);
        callValid = res.valid();
        if (callValid) {
            if (res.return_count() >= 1) {
                sol::object ret = res.get<sol::object>(0);
                truthy = LuaTruthy(ret);
            }
        } else {
            sol::error e = res;
            errMsg = e.what();
            LOG_WARN("TryRenderCachedLuaField: Lua error field=%s err=%s", fieldId.c_str(), e.what());
        }
    } catch (const std::exception& ex) {
        callOk = false;
        errMsg = ex.what();
        LOG_WARN("TryRenderCachedLuaField: exception field=%s err=%s", fieldId.c_str(), ex.what());
    } catch (...) {
        callOk = false;
        errMsg = "unknown C++ exception";
        LOG_WARN("TryRenderCachedLuaField: unknown exception field=%s", fieldId.c_str());
    }
}

} // namespace

sol::protected_function* AppController::Impl::ResolveLuaFieldProvider(const std::string& fieldId,
                                                                      const TrackerField* fieldMeta) {
    const auto itId = fieldDisplayCachedProviders_.find(fieldId);
    if (itId != fieldDisplayCachedProviders_.end() && itId->second.valid()) {
        return &itId->second;
    }
    if (fieldMeta && !fieldMeta->Name.empty() && !fieldDisplayCachedProvidersByName_.empty()) {
        const auto itName = fieldDisplayCachedProvidersByName_.find(AsciiLowerCopy(fieldMeta->Name));
        if (itName != fieldDisplayCachedProvidersByName_.end() && itName->second.valid()) {
            return &itName->second;
        }
    }
    return nullptr;
}

void AppController::SurfaceLuaFieldError(const std::string& fieldName, const std::string& issueId,
                                         const std::string& errMsg) {
    const std::string bare = "[LUA cell] field=" + fieldName + " id=" + issueId + ": " + errMsg;
    impl_->LuaLogInfoBind(std::string("[ERROR] ") + bare);
    for (const auto& sink : errorSinks_) {
        sink(bare);
    }
    scriptingWindowOpenRequested_.store(true);
}

bool AppController::TryRenderCachedLuaField(const std::string& fieldId, const CachedTicket& ticket,
                                            const std::string& rawValue, float availWidth,
                                            const TrackerField* fieldMeta, bool allowEdits) {
    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    // 1. Provider lookup — by field id first, then lowercased display name.
    sol::protected_function* providerSlot = impl_->ResolveLuaFieldProvider(fieldId, fieldMeta);
    if (providerSlot == nullptr)
        return false;

    const bool catalogReadOnly = fieldMeta ? fieldMeta->ReadOnly : false;
    const bool editMetaReadOnly = !CanEditFieldForIssue(ticket.id, fieldId, fieldMeta);
    const bool isReadOnly = catalogReadOnly || editMetaReadOnly || !allowEdits;
    const std::string fieldName = fieldMeta ? fieldMeta->Name : std::string();
    const int intAvailWidth = static_cast<int>(std::lround(availWidth));
    const std::uint64_t curProviderGen = luaProviderGen_.load();

    // 2. Cache key + hit check.
    std::string key = ticket.id;
    key.push_back('\0');
    key.append(fieldId);
    auto cit = impl_->luaFieldCache_.find(key);
    if (cit != impl_->luaFieldCache_.end()) {
        smatchet::lua::LuaFieldCacheEntry& entry = cit->second;
        const bool inputsMatch = entry.rawValue == rawValue && entry.fieldName == fieldName &&
                                 entry.intAvailWidth == intAvailWidth && entry.isReadOnly == isReadOnly &&
                                 entry.providerGen == curProviderGen;
        if (inputsMatch) {
            if (!entry.handled)
                return false;
            ReplayCmdList(entry.cmds, *this, lua, ticket.id, fieldId, entry.isReadOnly);
            return true;
        }
    }

    // 3. Miss / stale — invoke provider on a fresh recorder.
    SMATCHET_UI_PERF_SCOPE("LuaDrawList::Record");
    sol::protected_function providerCopy = *providerSlot; // crash-safety §C3 (own a ref)
    auto rec = std::make_shared<LuaDrawList>();           // F6 lifetime: shared with Lua
    sol::object fieldNameObj = fieldMeta ? sol::make_object(lua, fieldMeta->Name) : sol::make_object(lua, sol::nil);
    bool callOk = true;
    bool callValid = false;
    bool truthy = false;
    std::string errMsg;
    InvokeCachedFieldProvider(lua, providerCopy, ticket.id, fieldId, rawValue, availWidth, isReadOnly, fieldNameObj,
                              rec, callOk, callValid, truthy, errMsg);
    rec->Deactivate();

    smatchet::lua::LuaFieldCacheEntry entry;
    entry.rawValue = rawValue;
    entry.fieldName = fieldName;
    entry.intAvailWidth = intAvailWidth;
    entry.isReadOnly = isReadOnly;
    entry.providerGen = curProviderGen;
    if (!callOk || !callValid) {
        // Surface the error in the persistent "Lua Errors" panel + scrolling log + auto-
        // open Scripting window so the user can debug instead of staring at a silent C++
        // fallback. Cache-key is populated below, so this error fires once per cell per
        // (rawValue, fieldName, width, readOnly, providerGen) tuple — never per frame.
        SurfaceLuaFieldError(fieldName, ticket.id, errMsg);
        entry.handled = false;
        entry.cmds.clear();
    } else {
        if (truthy) {
            entry.handled = true;
            entry.cmds = rec->Take();
        } else {
            entry.handled = false;
        }
    }

    const bool handled = entry.handled;
    if (handled) {
        // Replay the freshly-recorded list immediately so this frame paints.
        ReplayCmdList(entry.cmds, *this, lua, ticket.id, fieldId, entry.isReadOnly);
    }
    impl_->luaFieldCache_[key] = std::move(entry);
    return handled;
}

bool AppController::ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                                      std::string& outError) {
    return ScenarioRegisterLuaCachedProvider(fieldId, luaFnName, std::vector<std::string>{}, outError);
}

bool AppController::ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                                      const std::vector<std::string>& extraScripts,
                                                      std::string& outError) {
    namespace fs = ghc::filesystem;
    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    outError.clear();
    if (fieldId.empty() || luaFnName.empty()) {
        outError = "fieldId and luaFnName required";
        return false;
    }

    auto lookup = [&]() -> sol::protected_function {
        sol::object slot = lua[luaFnName];
        if (slot.valid() && slot.get_type() == sol::type::function) {
            return slot.as<sol::protected_function>();
        }
        return sol::protected_function();
    };

    sol::protected_function fn = lookup();

    if (!fn.valid()) {
        // Function may live in a script the ephemeral / headless launcher hasn't loaded yet
        // (userData/Scripts is empty on a fresh install). Probe a candidate-path list and
        // load the first hit in the global env via `lua.script_file` so subsequent `_G[name]`
        // lookup succeeds. Non-`local` `function` declarations become globally visible.
        std::vector<std::string> filenames;
        filenames.emplace_back("SmatchetHooks.lua");
        filenames.insert(filenames.end(), extraScripts.begin(), extraScripts.end());

        std::vector<std::string> roots;
        if (!luaScriptsDirectory_.empty()) {
            roots.push_back(luaScriptsDirectory_);
        }
        roots.emplace_back("Scripts/");
        roots.emplace_back("scripts/");
        roots.emplace_back("../scripts/");
        roots.emplace_back("../../scripts/");
        roots.emplace_back("../../../scripts/");

        std::string loadErr;
        for (const std::string& fname : filenames) {
            for (const std::string& root : roots) {
                const std::string path = root + fname;
                std::error_code ec;
                if (!fs::is_regular_file(fs::path(path), ec)) {
                    continue;
                }
                try {
                    sol::protected_function_result r = lua.script_file(path);
                    if (!r.valid()) {
                        sol::error e = r;
                        loadErr = path + ": " + e.what();
                        continue;
                    }
                    LOG_INFO("ScenarioRegisterLuaCachedProvider: auto-loaded %s", path.c_str());
                    fn = lookup();
                    if (fn.valid()) {
                        break;
                    }
                } catch (const std::exception& ex) {
                    loadErr = path + ": " + ex.what();
                }
            }
            if (fn.valid()) {
                break;
            }
        }

        if (!fn.valid()) {
            outError = "Lua function not found or not callable: " + luaFnName;
            if (!loadErr.empty()) {
                outError += " (auto-load tried: " + loadErr + ")";
            }
            return false;
        }
    }

    // Stash any pre-existing user-side provider so Unregister can restore it. Tracked
    // separately for "had a provider" vs "had nothing" so we never accidentally synthesise
    // an empty entry on restore.
    auto existing = impl_->fieldDisplayCachedProviders_.find(fieldId);
    if (existing != impl_->fieldDisplayCachedProviders_.end()) {
        impl_->scenarioPriorFieldProviders_[fieldId] = existing->second;
        impl_->scenarioPriorEmptyFields_.erase(fieldId);
    } else {
        impl_->scenarioPriorFieldProviders_.erase(fieldId);
        impl_->scenarioPriorEmptyFields_.insert(fieldId);
    }

    impl_->fieldDisplayCachedProviders_[fieldId] = std::move(fn);
    luaProviderGen_.fetch_add(1);
    LOG_INFO("ScenarioRegisterLuaCachedProvider: bound field=%s fn=%s", fieldId.c_str(), luaFnName.c_str());
    return true;
}

void AppController::ScenarioUnregisterLuaCachedProvider(const std::string& fieldId) {
    // Restore the user-side provider that Register displaced, or erase if there was none.
    auto priorIt = impl_->scenarioPriorFieldProviders_.find(fieldId);
    const bool hadPrior = (priorIt != impl_->scenarioPriorFieldProviders_.end());
    const bool hadEmptyPrior = impl_->scenarioPriorEmptyFields_.count(fieldId) > 0;
    if (!hadPrior && !hadEmptyPrior) {
        // Field was never registered through the scenario surface — leave alone.
        return;
    }

    if (hadPrior) {
        impl_->fieldDisplayCachedProviders_[fieldId] = std::move(priorIt->second);
        impl_->scenarioPriorFieldProviders_.erase(priorIt);
    } else {
        impl_->fieldDisplayCachedProviders_.erase(fieldId);
    }
    impl_->scenarioPriorEmptyFields_.erase(fieldId);
    luaProviderGen_.fetch_add(1);

    for (auto it = impl_->luaFieldCache_.begin(); it != impl_->luaFieldCache_.end();) {
        const std::string& key = it->first;
        const std::size_t nul = key.find('\0');
        if (nul != std::string::npos && key.compare(nul + 1, std::string::npos, fieldId) == 0) {
            it = impl_->luaFieldCache_.erase(it);
        } else {
            ++it;
        }
    }
    LOG_INFO("ScenarioUnregisterLuaCachedProvider: field=%s restored=%s", fieldId.c_str(),
             hadPrior ? "user-provider" : "(none)");
}

void AppController::ScenarioInvalidateLuaFieldCache() { impl_->luaFieldCache_.clear(); }

bool AppController::TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField* field,
                                             const std::string& rawValue, std::string& outPathOrUrl) const {
    outPathOrUrl.clear();
    using TrackerFieldValueUtils::ResolveOptionId;
    using TrackerFieldValueUtils::ResolveOptionLabel;

    auto lookupInner = [&](const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& outer,
                           const std::string& outerKey) -> bool {
        const auto oit = outer.find(outerKey);
        if (oit == outer.end()) {
            return false;
        }
        const auto& inner = oit->second;
        std::vector<std::string> keys;
        if (field != nullptr) {
            const std::string optId = ToLowerAsciiCopy(TrimCopyAsciiWhitespace(ResolveOptionId(*field, rawValue)));
            if (!optId.empty()) {
                keys.push_back(optId);
                keys.push_back(ToLowerAsciiCopy(TrimCopyAsciiWhitespace(ResolveOptionLabel(*field, optId))));
            }
        }
        keys.push_back(ToLowerAsciiCopy(TrimCopyAsciiWhitespace(rawValue)));
        if (field != nullptr) {
            keys.push_back(
                ToLowerAsciiCopy(TrimCopyAsciiWhitespace(DisplayValueForTrackerDateField(fieldId, field, rawValue))));
        }
        for (const std::string& k : keys) {
            if (k.empty()) {
                continue;
            }
            const auto it = inner.find(k);
            if (it != inner.end() && !it->second.empty()) {
                outPathOrUrl = it->second;
                return true;
            }
        }
        return false;
    };

    std::lock_guard<std::mutex> lock(impl_->fieldIconMapsMutex_);
    if (lookupInner(impl_->fieldIconMapsByFieldId_, fieldId)) {
        return true;
    }
    if (field != nullptr && !field->Name.empty()) {
        if (lookupInner(impl_->fieldIconMapsByDisplayName_, ToLowerAsciiCopy(field->Name))) {
            return true;
        }
    }
    return false;
}

namespace {
// Shared MCP return-value marshal: first return value as a string verbatim, else
// JSON-encoded; no return -> "{}". Used by all three MCP Lua execution paths.
std::string McpLuaResultToString(sol::protected_function_result& result) {
    if (result.return_count() > 0) {
        sol::object obj = result[0];
        if (obj.is<std::string>()) {
            return obj.as<std::string>();
        }
        return LuaToJson(obj).dump();
    }
    return "{}";
}
} // namespace

std::vector<smatchet::lua::McpToolDefinition> AppController::Impl::GetLuaMcpTools() const {
    std::lock_guard<std::mutex> lock(luaMcpToolsMutex_);
    return luaMcpTools_;
}

std::string AppController::ExecuteLuaMcpTool(const std::string& name, const std::string& paramsJson,
                                             std::string& outError) {
    outError.clear(); // deterministic success contract: never leak caller-provided prior error text
    // Fast reject for genuinely-unknown names against the shared registry, before
    // paying fresh-state setup. (The MCP dispatcher already gates on GetLuaMcpTools,
    // so this is defensive.)
    {
        std::lock_guard<std::mutex> lock(impl_->luaMcpToolsMutex_);
        const bool known = std::any_of(impl_->luaMcpTools_.begin(), impl_->luaMcpTools_.end(),
                                       [&](const smatchet::lua::McpToolDefinition& tool) { return tool.name == name; });
        if (!known) {
            outError = "Tool not found";
            LOG_TRACE("ExecuteLuaMcpTool: not_found name=%s", name.c_str());
            return "";
        }
    }

    LOG_TRACE("ExecuteLuaMcpTool: begin name=%s params_len=%zu", name.c_str(), paramsJson.size());

    // The registered callback is a sol::protected_function bound to the UI-thread `lua`
    // state and cannot be invoked from this httplib worker thread without a data race.
    // Rebuild the tool on a fresh per-call state: replay the setup scripts with a
    // call-local register_tool override that collects definitions into `localTools`
    // (declared AFTER callState so its protected_function members tear down BEFORE the
    // state — the protected_function-before-state invariant). The rebuilt callback is
    // bound to callState; nothing here touches `lua`.
    sol::state callState;
    impl_->PrepareFreshLuaState(callState);
    std::vector<smatchet::lua::McpToolDefinition> localTools;

    sol::table mcpTbl = callState["mcp"];
    if (mcpTbl.valid()) {
        std::vector<smatchet::lua::McpToolDefinition>* sink = &localTools;
        mcpTbl.set_function("register_tool", [sink](sol::table def, sol::function cb) {
            if (!def.valid() || !cb.valid()) {
                return;
            }
            smatchet::lua::McpToolDefinition d;
            AppController::Impl::ParseMcpToolDef(def, d);
            d.callback = sol::protected_function(std::move(cb));
            sink->erase(std::remove_if(sink->begin(), sink->end(),
                                       [&](const smatchet::lua::McpToolDefinition& x) { return x.name == d.name; }),
                        sink->end());
            sink->push_back(std::move(d));
        });
    }

    sol::environment sandbox = CreateSandboxEnvironment(callState);
    impl_->ReplayActiveSetupScripts(callState, sandbox);

    sol::protected_function callback;
    for (auto& t : localTools) {
        if (t.name == name) {
            callback = t.callback;
            break;
        }
    }
    if (!callback.valid()) {
        // Known to the shared registry but not produced by replaying the setup scripts —
        // i.e. it was registered ad-hoc (e.g. by a prior run_lua snippet), so it cannot be
        // rebuilt on a fresh state. Degenerate, already-racy usage; fail cleanly.
        outError = "Tool not available (not defined by a setup script)";
        LOG_TRACE("ExecuteLuaMcpTool: not_rebuildable name=%s", name.c_str());
        return "";
    }

    // `paramsJson` is the MCP tool `arguments.dump()` — ATTACKER-CONTROLLED, and a
    // deeply-nested payload would stack-overflow a bare json::parse before JsonToLua
    // runs (Pillar 3 — Never crash). Parse through the shared depth/node-bounded
    // helper (NO throw across the sol2 boundary). On a depth/node-cap hit reject via
    // the path's nil+error contract (outError + return ""); ordinary shallow-malformed
    // JSON keeps the prior lenient empty-object fallback so valid tool calls are
    // unaffected.
    std::string parseErr;
    nlohmann::json jParams = smatchet::json_safe::ParseBounded(paramsJson, parseErr);
    if (!parseErr.empty()) {
        if (parseErr == smatchet::json_safe::OverflowError() || parseErr == smatchet::json_safe::TooLargeError()) {
            outError = parseErr;
            LOG_WARN("ExecuteLuaMcpTool: params rejected — input too deeply nested or too large; possible hostile "
                     "payload (name=%s)",
                     name.c_str());
            return "";
        }
        jParams = nlohmann::json::object();
    }
    try {
        LOG_TRACE("ExecuteLuaMcpTool: params_json=%s", TruncateForTrace(jParams.dump()).c_str());
    } catch (...) { // catch-all-ok: trace dump of untrusted params JSON
        LOG_TRACE("ExecuteLuaMcpTool: params (dump failed)");
    }

    std::string ret;
    {
        LuaHookGuard hook(callState);
        FieldEditAuditSource::ScopedOverride mcpSource(FieldEditAuditSource::kMcp);
        sol::protected_function_result result = callback(JsonToLua(callState, jParams));
        if (!result.valid()) {
            sol::error e = result;
            outError = e.what();
            LOG_TRACE("ExecuteLuaMcpTool: error name=%s err=%s", name.c_str(), TruncateForTrace(outError).c_str());
            return "";
        }
        ret = McpLuaResultToString(result);
    }
    LOG_TRACE("ExecuteLuaMcpTool: ok name=%s result_len=%zu", name.c_str(), ret.size());
    return ret;
}

std::string AppController::ExecuteLuaSnippetForMcp(const std::string& code, const nlohmann::json& args,
                                                   std::string& outError) {
    outError.clear(); // deterministic success contract: never leak caller-provided prior error text
    if (code.empty()) {
        outError = "Missing snippet code";
        return "";
    }

    try {
        LOG_TRACE("ExecuteLuaSnippetForMcp: begin code_len=%zu args=%s", code.size(),
                  TruncateForTrace(args.dump()).c_str());
    } catch (...) { // catch-all-ok: trace dump of untrusted args JSON
        LOG_TRACE("ExecuteLuaSnippetForMcp: begin code_len=%zu (args dump failed)", code.size());
    }

    // Fresh per-call state: this runs on an httplib worker thread, while the main
    // `lua` state is driven by the UI thread. Never share a lua_State across threads.
    sol::state callState;
    impl_->PrepareFreshLuaState(callState);
    sol::environment sandbox = CreateSandboxEnvironment(callState);
    sandbox["args"] = JsonToLua(callState, args);

    sol::load_result script = callState.load(code, "mcp.run_lua.snippet");
    if (!script.valid()) {
        sol::error err = script;
        outError = err.what();
        LOG_TRACE("ExecuteLuaSnippetForMcp: load_error %s", TruncateForTrace(outError).c_str());
        return "";
    }

    sol::protected_function func = script;
    sandbox.set_on(func);

    std::string ret;
    {
        LuaHookGuard hook(callState);
        FieldEditAuditSource::ScopedOverride mcpSource(FieldEditAuditSource::kMcp);
        sol::protected_function_result result = func();
        if (!result.valid()) {
            sol::error e = result;
            outError = e.what();
            LOG_TRACE("ExecuteLuaSnippetForMcp: runtime_error %s", TruncateForTrace(outError).c_str());
            return "";
        }
        ret = McpLuaResultToString(result);
    }
    LOG_TRACE("ExecuteLuaSnippetForMcp: ok result_len=%zu", ret.size());
    return ret;
}

bool AppController::ExecuteLuaConsoleSnippet(const std::string& code, std::string& outError,
                                             std::string& outResultSummary) {
    outError.clear();
    outResultSummary.clear();
    if (code.empty()) {
        outError = "No code to run";
        return false;
    }
    constexpr size_t kMaxConsoleSnippetBytes = 512u * 1024u;
    if (code.size() > kMaxConsoleSnippetBytes) {
        outError = "Code exceeds maximum size (512 KB)";
        return false;
    }

    LOG_TRACE("ExecuteLuaConsoleSnippet: begin code_len=%zu", code.size());

    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sol::load_result script = lua.load(code, "lua_console.oneshot");
    if (!script.valid()) {
        sol::error err = script;
        outError = err.what();
        LOG_TRACE("ExecuteLuaConsoleSnippet: load_error %s", TruncateForTrace(outError).c_str());
        impl_->LuaLogInfoBind(std::string("[ERROR] ") + outError);
        for (const auto& sink : errorSinks_) {
            sink(outError);
        }
        scriptingWindowOpenRequested_.store(true);
        return false;
    }

    sol::protected_function func = script;
    sandbox.set_on(func);

    lua_sethook(
        lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) { luaL_error(L, "Script execution timeout exceeded."); },
        LUA_MASKCOUNT, 100000);

    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);
    sol::protected_function_result result = func();
    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!result.valid()) {
        sol::error e = result;
        outError = e.what();
        LOG_TRACE("ExecuteLuaConsoleSnippet: runtime_error %s", TruncateForTrace(outError).c_str());
        // Red scrolling-log entry + persistent error panel + auto-open window.
        impl_->LuaLogInfoBind(std::string("[ERROR] ") + outError);
        for (const auto& sink : errorSinks_) {
            sink(outError);
        }
        scriptingWindowOpenRequested_.store(true);
        return false;
    }

    if (result.return_count() > 0) {
        try {
            const sol::object obj = result[0];
            if (obj.valid() && obj.get_type() != sol::type::lua_nil) {
                if (obj.is<std::string>()) {
                    outResultSummary = obj.as<std::string>();
                } else {
                    outResultSummary = LuaToJson(obj).dump();
                }
            }
        } catch (const std::exception& e) {
            outResultSummary = std::string("(return stringify failed: ") + e.what() + ")";
        } catch (...) { // catch-all-ok: stringify on Lua return value for logging
            outResultSummary = "(return stringify failed)";
        }
        constexpr size_t kMaxSummary = 800;
        if (outResultSummary.size() > kMaxSummary) {
            outResultSummary.resize(kMaxSummary);
            outResultSummary += "...";
        }
    }

    LOG_TRACE("ExecuteLuaConsoleSnippet: ok summary_len=%zu", outResultSummary.size());
    return true;
}

std::string AppController::ExecuteLuaScriptForMcp(const std::string& scriptName, const nlohmann::json& args,
                                                  std::string& outError) {
    outError.clear(); // deterministic success contract: never leak caller-provided prior error text
    const std::string path = ResolveLuaScriptPath(scriptName);
    if (path.empty()) {
        outError = "Invalid script path";
        LOG_TRACE("ExecuteLuaScriptForMcp: invalid scriptName=%s", scriptName.c_str());
        return "";
    }

    try {
        LOG_TRACE("ExecuteLuaScriptForMcp: begin path=%s scriptName=%s args=%s", path.c_str(), scriptName.c_str(),
                  TruncateForTrace(args.dump()).c_str());
    } catch (...) { // catch-all-ok: trace dump of untrusted args JSON
        LOG_TRACE("ExecuteLuaScriptForMcp: begin path=%s (args dump failed)", path.c_str());
    }

    // Fresh per-call state: runs on an httplib worker thread; the main `lua` state
    // is the UI thread's. Never share a lua_State across threads.
    sol::state callState;
    impl_->PrepareFreshLuaState(callState);
    sol::environment sandbox = CreateSandboxEnvironment(callState);
    sandbox["args"] = JsonToLua(callState, args);

    sol::load_result script = callState.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        outError = err.what();
        LOG_TRACE("ExecuteLuaScriptForMcp: load_error path=%s %s", path.c_str(), TruncateForTrace(outError).c_str());
        return "";
    }

    sol::protected_function func = script;
    sandbox.set_on(func);

    std::string ret;
    {
        LuaHookGuard hook(callState);
        FieldEditAuditSource::ScopedOverride mcpSource(FieldEditAuditSource::kMcp);
        sol::protected_function_result result = func();
        if (!result.valid()) {
            sol::error e = result;
            outError = e.what();
            LOG_TRACE("ExecuteLuaScriptForMcp: runtime_error path=%s %s", path.c_str(),
                      TruncateForTrace(outError).c_str());
            return "";
        }
        ret = McpLuaResultToString(result);
    }
    LOG_TRACE("ExecuteLuaScriptForMcp: ok path=%s result_len=%zu", path.c_str(), ret.size());
    return ret;
}

void AppController::RecordLuaWindow(smatchet::lua::LuaWindowEntry& w, std::uint64_t curDataGen,
                                    std::uint64_t curProviderGen) {
    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    auto rec = std::make_shared<LuaDrawList>();
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);
    sol::protected_function drawFnCopy = w.drawFn; // Crash-safety §C3
    bool callOk = true;
    bool callValid = false;
    std::string callErr;
    // `res` stays inside the try-block; on the exception path it never
    // exists, so its uninit-on-throw state can't leak past the catch.
    // Silences sol2's -Wmaybe-uninitialized through `~protected_function_result()`
    // (stack::remove of L/index/popcount at sol.hpp:12398).
    try {
        SMATCHET_UI_PERF_SCOPE("LuaWindow::Record");
        LuaHookGuard hook(lua);
        LuaImmediateModeGuard imm(false);
        sol::protected_function_result res = drawFnCopy(rec);
        callValid = res.valid();
        if (!callValid) {
            sol::error e = res;
            callErr = e.what();
        }
    } catch (const std::exception& ex) {
        callOk = false;
        callErr = ex.what();
        LOG_WARN("DrawLuaWindows: C++ exception window=%s %s", w.name.c_str(), ex.what());
    } catch (...) {
        callOk = false;
        callErr = "unknown C++ exception";
        LOG_WARN("DrawLuaWindows: unknown C++ exception window=%s", w.name.c_str());
    }
    rec->Deactivate();
    if (callOk && callValid) {
        w.cmds = rec->Take();
        w.cachedDataGen = curDataGen;
        w.cachedProviderGen = curProviderGen;
        w.dirty = false;
        w.hasError = false;
        w.errorMessage.clear();
    } else {
        LOG_TRACE("DrawLuaWindows: error window=%s %s", w.name.c_str(), callErr.c_str());
        // Surface in the persistent "Lua Errors" panel + scrolling log +
        // auto-open Scripting window. Negative-cache below means this fires
        // once per record-event per window, not per frame.
        const std::string bare = "[LUA window] " + w.name + ": " + callErr;
        impl_->LuaLogInfoBind(std::string("[ERROR] ") + bare);
        for (const auto& sink : errorSinks_) {
            sink(bare);
        }
        scriptingWindowOpenRequested_.store(true);
        w.cmds.clear();
        w.hasError = true;
        w.errorMessage = std::move(callErr);
        w.cachedDataGen = curDataGen;
        w.cachedProviderGen = curProviderGen;
        w.dirty = false;
    }
}

void AppController::DrawLuaWindows() {
    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    // Recorded-cmd-list draw path. The Lua draw fn runs only on a dirty / gen-mismatch
    // frame; otherwise we replay the cached recording. See plan §Window draw.
    inDrawLuaWindows_ = true;
    const std::uint64_t curDataGen = luaWindowDataGen_.load();
    const std::uint64_t curProviderGen = luaProviderGen_.load();
    for (smatchet::lua::LuaWindowEntry& w : impl_->luaWindows_) {
        if (!w.drawFn.valid())
            continue;
        bool open = true;
        if (ImGui::Begin(w.name.c_str(), &open)) {
            const bool needRecord = w.dirty || w.cachedDataGen != curDataGen || w.cachedProviderGen != curProviderGen;
            if (needRecord) {
                RecordLuaWindow(w, curDataGen, curProviderGen);
            }
            if (w.hasError) {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Lua Error: %s", w.errorMessage.c_str());
            } else {
                using K = LuaReplayCallbackKind;
                std::uint8_t fired;
                {
                    SMATCHET_UI_PERF_SCOPE("LuaWindow::Replay");
                    fired = ReplayCmdList(w.cmds, *this, lua, w.name, std::string(),
                                          /*isReadOnly=*/false);
                }
                const std::uint8_t dirtyMask =
                    static_cast<std::uint8_t>(K::Click) | static_cast<std::uint8_t>(K::Commit);
                // Per plan Q6: only Click / Commit auto-dirty; on_deactivated* does not.
                if (fired & dirtyMask) {
                    w.dirty = true;
                }
            }
        }
        ImGui::End();
        if (!open)
            w.drawFn = sol::lua_nil;
    }
    inDrawLuaWindows_ = false;

    // Drain ops enqueued mid-iteration (button callbacks that called register/unregister/
    // invalidate). ApplyOrQueueLuaWindowOp now hits the direct-mutation path because the
    // flag flipped to false.
    if (!impl_->pendingLuaWindowOps_.empty()) {
        std::vector<smatchet::lua::PendingLuaWindowOp> drained;
        drained.swap(impl_->pendingLuaWindowOps_);
        for (smatchet::lua::PendingLuaWindowOp& op : drained) {
            ApplyOrQueueLuaWindowOp(std::move(op));
        }
    }

    impl_->luaWindows_.erase(std::remove_if(impl_->luaWindows_.begin(), impl_->luaWindows_.end(),
                                            [](const smatchet::lua::LuaWindowEntry& w) { return !w.drawFn.valid(); }),
                             impl_->luaWindows_.end());
}

std::vector<std::string> AppController::GetLuaTicketActionNames() const {
    std::lock_guard<std::mutex> lock(impl_->luaActionsMutex_);
    std::vector<std::string> names;
    names.reserve(impl_->luaTicketActions_.size());
    std::transform(impl_->luaTicketActions_.begin(), impl_->luaTicketActions_.end(), std::back_inserter(names),
                   [](const auto& pair) { return pair.first; });
    return names;
}

std::vector<std::string> AppController::GetLuaGlobalActionNames() const {
    std::lock_guard<std::mutex> lock(impl_->luaActionsMutex_);
    std::vector<std::string> names;
    names.reserve(impl_->luaGlobalActions_.size());
    std::transform(impl_->luaGlobalActions_.begin(), impl_->luaGlobalActions_.end(), std::back_inserter(names),
                   [](const auto& pair) { return pair.first; });
    return names;
}

void AppController::ExecuteLuaTicketAction(const std::string& name, const std::string& issueId) {
    std::string callbackFuncName;
    {
        std::lock_guard<std::mutex> lock(impl_->luaActionsMutex_);
        const auto it =
            std::find_if(impl_->luaTicketActions_.begin(), impl_->luaTicketActions_.end(),
                         [&](const std::pair<std::string, std::string>& pair) { return pair.first == name; });
        if (it != impl_->luaTicketActions_.end()) {
            callbackFuncName = it->second;
        }
    }
    if (callbackFuncName.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
        impl_->automationJobs_.push_back({AutomationJob::Type::TicketAction, callbackFuncName, {}, issueId});
    }
    impl_->automationJobCv_.notify_one();
}

void AppController::ExecuteLuaGlobalAction(const std::string& name) {
    std::string callbackFuncName;
    {
        std::lock_guard<std::mutex> lock(impl_->luaActionsMutex_);
        const auto it =
            std::find_if(impl_->luaGlobalActions_.begin(), impl_->luaGlobalActions_.end(),
                         [&](const std::pair<std::string, std::string>& pair) { return pair.first == name; });
        if (it != impl_->luaGlobalActions_.end()) {
            callbackFuncName = it->second;
        }
    }
    if (callbackFuncName.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
        impl_->automationJobs_.push_back({AutomationJob::Type::GlobalAction, callbackFuncName, {}, ""});
    }
    impl_->automationJobCv_.notify_one();
}
