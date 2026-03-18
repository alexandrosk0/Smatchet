#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <iostream>
#include "AppController.h"

class McpServer {
public:
    McpServer(AppController& controller) : app(controller), running(false) {}

    void Start(int port = 8080) {
        if (running) return;
        running = true;

        serverThread = std::thread([this, port]() {
            try {
                httplib::Server svr;

                // MCP Endpoint: List all tickets for the AI
                svr.Get("/mcp/list_tickets", [this](const httplib::Request&, httplib::Response& res) {
                    const auto& tickets = app.GetActiveTickets();
                    nlohmann::json j = nlohmann::json::array();
                    for (const auto& t : tickets) {
                        j.push_back({{"id", t.id}, {"fields", t.fieldValues}});
                    }
                    res.set_content(j.dump(), "application/json");
                });

                // MCP Endpoint: Search tickets (The AI can call this with ?q=Engine)
                svr.Get("/mcp/search", [this](const httplib::Request& req, httplib::Response& res) {
                    std::string query = req.get_param_value("q");
                    const auto& tickets = app.GetActiveTickets();
                    nlohmann::json j = nlohmann::json::array();
                    for (const auto& t : tickets) {
                        bool matches = (t.id.find(query) != std::string::npos);
                        if (!matches) {
                            for (const auto& kv : t.fieldValues) {
                                if (kv.first.find(query) != std::string::npos || kv.second.find(query) != std::string::npos) {
                                    matches = true;
                                    break;
                                }
                            }
                        }
                        if (matches) {
                            j.push_back({{"id", t.id}, {"fields", t.fieldValues}});
                        }
                    }
                    res.set_content(j.dump(), "application/json");
                });

                svr.listen("0.0.0.0", port);
            } catch (const std::exception& e) {
                std::cerr << "MCP server thread error: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "MCP server thread unknown error" << std::endl;
            }
        });
        serverThread.detach(); // Run in background
    }

private:
    AppController& app;
    std::thread serverThread;
    std::atomic<bool> running;
};

#endif