#include "AiController.h"

AiController::AiResult AiController::AnalyzeTicket(const std::string& /*key*/, const std::string& /*summary*/,
                                                   const std::string& /*apiKey*/, const std::string& /*model*/,
                                                   const std::string& /*baseUrl*/) {
    AiResult r;
    r.Success = false;
    r.Response = "AI features were not included in this build (SMATCHET_WITH_AI is off).";
    return r;
}






