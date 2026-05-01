#ifndef AI_CONTROLLER_H
#define AI_CONTROLLER_H

#include <string>

class AiController {
  public:
    struct AiResult {
        bool Success;
        std::string Response;
    };

    // Generic function to ask the AI about a specific ticket
    static AiResult AnalyzeTicket(const std::string& key, const std::string& summary, const std::string& apiKey,
                                  const std::string& model, const std::string& baseUrl);
};

#endif