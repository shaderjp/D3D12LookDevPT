#pragma once

#include "McpDispatcher.h"
#include "SimpleJson.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mcp
{
struct ServerSettings
{
    uint16_t port = 8777;
    int requestTimeoutSeconds = 120;
    AccessMode accessMode = AccessMode::ConfirmMutations;
    std::string token;
};

struct ServerStatus
{
    bool running = false;
    uint16_t port = 0;
    std::string endpoint;
    std::string lastError;
    size_t activeSessions = 0;
    size_t activeRequests = 0;
    std::vector<std::string> recentRequests;
};

struct ToolResult
{
    bool ok = false;
    bool isError = false;
    std::string text;
    std::string structuredJson = "{}";
    std::string contentJson;
};

struct ResourceResult
{
    bool ok = false;
    std::string uri;
    std::string mimeType = "application/json";
    std::string text;
    std::string blob;
    std::string error;
};

class IServerHost
{
public:
    virtual ~IServerHost() = default;
    virtual ToolResult CallMcpTool(const std::string& name, const cld::JsonValue& arguments, int timeoutMs) = 0;
    virtual ResourceResult ReadMcpResource(const std::string& uri) = 0;
    virtual size_t PendingMcpCommandCount() const = 0;
};

class Server
{
public:
    Server();
    ~Server();

    bool Start(const ServerSettings& settings, IServerHost* host);
    void Stop();
    bool IsRunning() const;
    ServerStatus GetStatus() const;

    struct HttpRequest
    {
        std::string method;
        std::string target;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string reason = "OK";
        std::string contentType = "application/json";
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };

private:
    struct Session
    {
        std::string protocolVersion;
        bool initialized = false;
    };

    void Run();
    void HandleClient(uintptr_t socketValue);
    bool ReadHttpRequest(uintptr_t socketValue, HttpRequest& request, std::string& error) const;
    void SendHttpResponse(uintptr_t socketValue, const HttpResponse& response) const;
    HttpResponse HandleHttpRequest(const HttpRequest& request);
    HttpResponse HandleJsonRpc(const HttpRequest& request, const cld::JsonValue& rpc);
    HttpResponse HandleInitialize(const cld::JsonValue& rpc);
    HttpResponse HandlePromptGet(const std::string& idJson, const cld::JsonValue& rpc);
    HttpResponse HandleDeleteSession(const HttpRequest& request);
    bool ValidateOrigin(const HttpRequest& request) const;
    bool ValidateAuthorization(const HttpRequest& request) const;
    bool ValidateProtocolHeader(const HttpRequest& request, std::string& diagnostics) const;
    bool ResolveSession(const HttpRequest& request, Session& session, std::string& diagnostics) const;
    void MarkSessionInitialized(const HttpRequest& request);
    void AppendLog(const std::string& message);

    ServerSettings m_settings;
    IServerHost* m_host = nullptr;
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_stopRequested = false;
    std::atomic<size_t> m_activeRequests = 0;
    uintptr_t m_listenSocket = 0;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::vector<std::thread> m_workers;
    std::map<std::string, Session> m_sessions;
    std::vector<std::string> m_recentRequests;
    std::string m_lastError;
};

std::string GenerateToken(size_t byteCount = 24);
std::string AccessModeName(AccessMode mode);
AccessMode AccessModeFromName(const std::string& name, AccessMode fallback);
std::string BuildActionsSchemaJson();
std::string BuildToolsListJson();
std::string BuildResourcesListJson();
std::string BuildResourceTemplatesListJson();
std::string BuildPromptsListJson();
std::string BuildPromptGetResultJson(const std::string& name, const cld::JsonValue* arguments, bool& found);
}
