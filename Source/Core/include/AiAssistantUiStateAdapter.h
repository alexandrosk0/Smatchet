#pragma once

#include "IAiAssistantUiState.h"

// GetGlobalAiAssistantUiState() — the process-lifetime IAiAssistantUiState that forwards to the
// global `g_ui` (UiDrawSession). Production wiring for AiAssistantController; the returned
// reference is to a function-local static, so a dispatcher task that drains AFTER the controller
// is destroyed stays safe (see IAiAssistantUiState.h § LIFETIME CONTRACT). This is the ONLY
// production binding — the headless ThreadSanitizer test uses a fake instead, which is why the
// controller never links the ImGui-bound UiDrawSession.
//
// Only defined under SMATCHET_WITH_AI (the only caller, AppController's controller construction,
// is itself AI-gated); the AI-off translation unit is empty.
IAiAssistantUiState& GetGlobalAiAssistantUiState();
