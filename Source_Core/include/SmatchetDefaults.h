#pragma once

namespace SmatchetDefaults {
constexpr char kDefaultDbPath[] = "Smatchet_LocalCache.sqlite";
constexpr char kDefaultBackendType[] = "Jira";

namespace Mcp {
constexpr int kDefaultPort = 42360;
constexpr char kBindLocalhost[] = "127.0.0.1";
constexpr char kBindAny[] = "0.0.0.0";
constexpr char kSsePath[] = "/mcp/sse";
}
} // namespace SmatchetDefaults
