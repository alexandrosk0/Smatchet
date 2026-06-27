#pragma once

#include "IAiAssistantUiState.h"

// Production IAiAssistantUiState bound to the global g_ui via a process-lifetime function-local
// static (safe for dispatcher tasks draining after the controller dies — see § LIFETIME CONTRACT
// in IAiAssistantUiState.h). Defined only under SMATCHET_WITH_AI; the test uses a fake instead.
IAiAssistantUiState& GetGlobalAiAssistantUiState();
