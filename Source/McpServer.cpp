#include "stdafx.h"
#include "McpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>

namespace
{
    constexpr const char* SupportedProtocolVersion = "2025-11-25";
    constexpr const char* CompatProtocolVersion = "2025-06-18";
    constexpr size_t MaxHttpBodyBytes = 16u * 1024u * 1024u;

    std::string ToLowerAscii(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
        return text;
    }

    bool StartsWith(const std::string& text, const std::string& prefix)
    {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    bool ContainsHeaderToken(const std::string& header, const std::string& token)
    {
        const std::string lowerHeader = ToLowerAscii(header);
        const std::string lowerToken = ToLowerAscii(token);
        return lowerHeader.find(lowerToken) != std::string::npos || lowerHeader.find("*/*") != std::string::npos;
    }

    std::string HeaderValue(const std::map<std::string, std::string>& headers, const std::string& name)
    {
        const auto it = headers.find(ToLowerAscii(name));
        return it == headers.end() ? std::string{} : it->second;
    }

    bool IsSupportedProtocolVersion(const std::string& version)
    {
        return version == SupportedProtocolVersion || version == CompatProtocolVersion;
    }

    std::string JsonMemberString(const cld::JsonValue& value, const char* name)
    {
        return cld::JsonStringOr(value, name);
    }

    const cld::JsonValue* JsonMember(const cld::JsonValue& value, const char* name)
    {
        return cld::FindMember(value, name);
    }

    std::string JsonIdToJson(const cld::JsonValue& value)
    {
        const cld::JsonValue* id = JsonMember(value, "id");
        return id ? cld::JsonValueToJson(*id) : "null";
    }

    bool HasJsonId(const cld::JsonValue& value)
    {
        return JsonMember(value, "id") != nullptr;
    }

    std::string MakeResponse(const std::string& idJson, const std::string& resultJson)
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":" + resultJson + "}";
    }

    std::string MakeError(const std::string& idJson, int code, const std::string& message)
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + cld::EscapeJson(message) + "\"}}";
    }

    std::string TextContentJson(const std::string& text)
    {
        return "[{\"type\":\"text\",\"text\":\"" + cld::EscapeJson(text) + "\"}]";
    }

    std::string ToolResultToJson(const mcp::ToolResult& result)
    {
        const std::string content = result.contentJson.empty() ? TextContentJson(result.text) : result.contentJson;
        std::string json = "{\"content\":" + content + ",\"structuredContent\":" + (result.structuredJson.empty() ? "{}" : result.structuredJson);
        if (result.isError)
        {
            json += ",\"isError\":true";
        }
        json += "}";
        return json;
    }

    std::string MakeHttpErrorBody(int code, const std::string& message)
    {
        return MakeError("null", code, message);
    }

    mcp::Server::HttpResponse JsonResponse(int status, const char* reason, const std::string& body)
    {
        mcp::Server::HttpResponse response;
        response.status = status;
        response.reason = reason;
        response.contentType = "application/json";
        response.body = body;
        return response;
    }

    std::string ToolJson(const char* name, const char* title, const char* description, const char* inputSchema)
    {
        return "{\"name\":\"" + std::string(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) +
            "\",\"inputSchema\":" + inputSchema + "}";
    }

    std::string ResourceJson(const char* uri, const char* name, const char* title, const char* description, const char* mimeType)
    {
        return "{\"uri\":\"" + std::string(uri) +
            "\",\"name\":\"" + cld::EscapeJson(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) +
            "\",\"mimeType\":\"" + cld::EscapeJson(mimeType) + "\"}";
    }

    std::string ResourceTemplateJson(const char* uriTemplate, const char* name, const char* title, const char* description, const char* mimeType)
    {
        return "{\"uriTemplate\":\"" + std::string(uriTemplate) +
            "\",\"name\":\"" + cld::EscapeJson(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) +
            "\",\"mimeType\":\"" + cld::EscapeJson(mimeType) + "\"}";
    }

    std::string PromptJson(const char* name, const char* title, const char* description)
    {
        return "{\"name\":\"" + std::string(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) + "\"}";
    }

    std::string PromptTextResult(const char* description, const std::string& text)
    {
        return "{\"description\":\"" + cld::EscapeJson(description) +
            "\",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"" +
            cld::EscapeJson(text) + "\"}}]}";
    }

    std::string EmptyObjectSchema()
    {
        return R"json({"type":"object","properties":{},"additionalProperties":false})json";
    }

    void AppendJoined(std::ostringstream& out, const std::vector<std::string>& items)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }
            out << items[i];
        }
    }

    std::string NewSessionId()
    {
        return mcp::GenerateToken(16);
    }

    std::string PathFromTarget(const std::string& target)
    {
        const size_t query = target.find('?');
        return query == std::string::npos ? target : target.substr(0, query);
    }
}

namespace mcp
{
Server::Server() = default;

Server::~Server()
{
    Stop();
}

bool Server::Start(const ServerSettings& settings, IServerHost* host)
{
    Stop();
    if (!host)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP host was not provided.";
        return false;
    }
    if (settings.token.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP token is empty.";
        return false;
    }

    WSADATA wsaData{};
    const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "WSAStartup failed: " + std::to_string(wsaResult);
        return false;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        const int error = WSAGetLastError();
        WSACleanup();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP socket creation failed: " + std::to_string(error);
        return false;
    }

    BOOL exclusive = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(settings.port);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(listenSocket);
        WSACleanup();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP bind failed on 127.0.0.1:" + std::to_string(settings.port) + " (" + std::to_string(error) + ")";
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(listenSocket);
        WSACleanup();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP listen failed: " + std::to_string(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = settings;
        m_host = host;
        m_listenSocket = static_cast<uintptr_t>(listenSocket);
        m_sessions.clear();
        m_recentRequests.clear();
        m_lastError.clear();
    }
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&Server::Run, this);
    AppendLog("MCP server started on 127.0.0.1:" + std::to_string(settings.port));
    return true;
}

void Server::Stop()
{
    if (!m_running && !m_thread.joinable())
    {
        return;
    }

    m_stopRequested = true;
    uintptr_t socketValue = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        socketValue = m_listenSocket;
        m_listenSocket = 0;
    }
    if (socketValue != 0 && static_cast<SOCKET>(socketValue) != INVALID_SOCKET)
    {
        shutdown(static_cast<SOCKET>(socketValue), SD_BOTH);
        closesocket(static_cast<SOCKET>(socketValue));
    }

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        workers.swap(m_workers);
        m_sessions.clear();
    }
    for (std::thread& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    m_running = false;
    WSACleanup();
}

bool Server::IsRunning() const
{
    return m_running;
}

ServerStatus Server::GetStatus() const
{
    ServerStatus status;
    std::lock_guard<std::mutex> lock(m_mutex);
    status.running = m_running;
    status.port = m_settings.port;
    status.endpoint = "http://127.0.0.1:" + std::to_string(m_settings.port) + "/mcp";
    status.lastError = m_lastError;
    status.activeSessions = m_sessions.size();
    status.activeRequests = m_activeRequests.load();
    status.recentRequests = m_recentRequests;
    return status;
}

void Server::Run()
{
    while (!m_stopRequested)
    {
        SOCKET listenSocket = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            listenSocket = static_cast<SOCKET>(m_listenSocket);
        }
        if (listenSocket == INVALID_SOCKET || listenSocket == 0)
        {
            break;
        }

        SOCKET client = accept(listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            if (!m_stopRequested)
            {
                const int error = WSAGetLastError();
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastError = "MCP accept failed: " + std::to_string(error);
            }
            break;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_workers.emplace_back(&Server::HandleClient, this, static_cast<uintptr_t>(client));
        }
    }
}

void Server::HandleClient(uintptr_t socketValue)
{
    SOCKET client = static_cast<SOCKET>(socketValue);
    DWORD timeoutMs = 30000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    HttpRequest request;
    std::string error;
    HttpResponse response;
    if (!ReadHttpRequest(socketValue, request, error))
    {
        response = JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32700, error));
    }
    else
    {
        response = HandleHttpRequest(request);
    }

    SendHttpResponse(socketValue, response);
    shutdown(client, SD_BOTH);
    closesocket(client);
}

bool Server::ReadHttpRequest(uintptr_t socketValue, HttpRequest& request, std::string& error) const
{
    SOCKET client = static_cast<SOCKET>(socketValue);
    std::string buffer;
    char chunk[4096];
    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos)
    {
        const int received = recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0)
        {
            error = "HTTP request read failed.";
            return false;
        }
        buffer.append(chunk, chunk + received);
        if (buffer.size() > MaxHttpBodyBytes)
        {
            error = "HTTP request is too large.";
            return false;
        }
        headerEnd = buffer.find("\r\n\r\n");
    }

    const std::string headerText = buffer.substr(0, headerEnd);
    std::istringstream headerStream(headerText);
    std::string requestLine;
    if (!std::getline(headerStream, requestLine))
    {
        error = "HTTP request line missing.";
        return false;
    }
    if (!requestLine.empty() && requestLine.back() == '\r')
    {
        requestLine.pop_back();
    }

    std::istringstream requestLineStream(requestLine);
    std::string version;
    requestLineStream >> request.method >> request.target >> version;
    if (request.method.empty() || request.target.empty())
    {
        error = "HTTP request line is invalid.";
        return false;
    }
    request.path = PathFromTarget(request.target);

    std::string line;
    while (std::getline(headerStream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        std::string name = ToLowerAscii(cld::TrimAscii(line.substr(0, colon)));
        std::string value = cld::TrimAscii(line.substr(colon + 1));
        request.headers[name] = value;
    }

    size_t contentLength = 0;
    const std::string contentLengthText = HeaderValue(request.headers, "content-length");
    if (!contentLengthText.empty())
    {
        try
        {
            contentLength = static_cast<size_t>(std::stoull(contentLengthText));
        }
        catch (...)
        {
            error = "Content-Length is invalid.";
            return false;
        }
        if (contentLength > MaxHttpBodyBytes)
        {
            error = "HTTP body is too large.";
            return false;
        }
    }

    const size_t bodyStart = headerEnd + 4;
    request.body = buffer.substr(bodyStart);
    while (request.body.size() < contentLength)
    {
        const int received = recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0)
        {
            error = "HTTP body read failed.";
            return false;
        }
        request.body.append(chunk, chunk + received);
    }
    if (request.body.size() > contentLength)
    {
        request.body.resize(contentLength);
    }
    return true;
}

void Server::SendHttpResponse(uintptr_t socketValue, const HttpResponse& response) const
{
    SOCKET client = static_cast<SOCKET>(socketValue);
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << response.reason << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    if (!response.contentType.empty())
    {
        out << "Content-Type: " << response.contentType << "\r\n";
    }
    out << "Connection: close\r\n";
    for (const auto& [name, value] : response.headers)
    {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    out << response.body;
    const std::string text = out.str();
    send(client, text.data(), static_cast<int>(text.size()), 0);
}

Server::HttpResponse Server::HandleHttpRequest(const HttpRequest& request)
{
    if (request.path != "/mcp")
    {
        return JsonResponse(404, "Not Found", MakeHttpErrorBody(-32601, "MCP endpoint is /mcp."));
    }
    if (!ValidateOrigin(request))
    {
        return JsonResponse(403, "Forbidden", MakeHttpErrorBody(-32000, "Origin is not allowed."));
    }
    if (!ValidateAuthorization(request))
    {
        return JsonResponse(401, "Unauthorized", MakeHttpErrorBody(-32001, "Authorization bearer token is required."));
    }
    if (request.method == "GET")
    {
        HttpResponse response = JsonResponse(405, "Method Not Allowed", MakeHttpErrorBody(-32000, "SSE is not implemented by this server."));
        response.headers.push_back({ "Allow", "POST, DELETE" });
        return response;
    }
    if (request.method == "DELETE")
    {
        return HandleDeleteSession(request);
    }
    if (request.method != "POST")
    {
        HttpResponse response = JsonResponse(405, "Method Not Allowed", MakeHttpErrorBody(-32000, "Only POST, GET, and DELETE are supported."));
        response.headers.push_back({ "Allow", "POST, GET, DELETE" });
        return response;
    }

    const std::string accept = HeaderValue(request.headers, "accept");
    if (!accept.empty() && !ContainsHeaderToken(accept, "application/json") && !ContainsHeaderToken(accept, "text/event-stream"))
    {
        return JsonResponse(406, "Not Acceptable", MakeHttpErrorBody(-32000, "Accept header must allow application/json or text/event-stream."));
    }

    std::string protocolDiagnostics;
    if (!ValidateProtocolHeader(request, protocolDiagnostics))
    {
        return JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32000, protocolDiagnostics));
    }

    cld::JsonValue rpc;
    try
    {
        rpc = cld::JsonParser(request.body).Parse();
    }
    catch (const std::exception& ex)
    {
        return JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32700, ex.what()));
    }
    return HandleJsonRpc(request, rpc);
}

Server::HttpResponse Server::HandleJsonRpc(const HttpRequest& request, const cld::JsonValue& rpc)
{
    if (rpc.type != cld::JsonValue::Type::Object)
    {
        return JsonResponse(400, "Bad Request", MakeError("null", -32600, "JSON-RPC message must be an object."));
    }

    const std::string method = JsonMemberString(rpc, "method");
    const bool hasId = HasJsonId(rpc);
    if (method.empty())
    {
        return JsonResponse(202, "Accepted", "");
    }

    if (method == "initialize")
    {
        if (!hasId)
        {
            return JsonResponse(202, "Accepted", "");
        }
        return HandleInitialize(rpc);
    }

    Session session;
    std::string sessionDiagnostics;
    if (!ResolveSession(request, session, sessionDiagnostics))
    {
        return JsonResponse(sessionDiagnostics == "Unknown MCP session." ? 404 : 400, sessionDiagnostics == "Unknown MCP session." ? "Not Found" : "Bad Request", MakeError(JsonIdToJson(rpc), -32002, sessionDiagnostics));
    }

    if (!hasId)
    {
        if (method == "notifications/initialized")
        {
            MarkSessionInitialized(request);
            AppendLog("notifications/initialized");
        }
        return JsonResponse(202, "Accepted", "");
    }

    m_activeRequests.fetch_add(1);
    auto requestGuard = std::unique_ptr<void, std::function<void(void*)>>(reinterpret_cast<void*>(1), [this](void*)
    {
        m_activeRequests.fetch_sub(1);
    });

    const std::string idJson = JsonIdToJson(rpc);
    AppendLog(method);

    if (method == "ping")
    {
        return JsonResponse(200, "OK", MakeResponse(idJson, "{}"));
    }
    if (method == "tools/list")
    {
        return JsonResponse(200, "OK", MakeResponse(idJson, BuildToolsListJson()));
    }
    if (method == "resources/list")
    {
        return JsonResponse(200, "OK", MakeResponse(idJson, BuildResourcesListJson()));
    }
    if (method == "resources/templates/list")
    {
        return JsonResponse(200, "OK", MakeResponse(idJson, BuildResourceTemplatesListJson()));
    }
    if (method == "prompts/list")
    {
        return JsonResponse(200, "OK", MakeResponse(idJson, BuildPromptsListJson()));
    }
    if (method == "prompts/get")
    {
        return HandlePromptGet(idJson, rpc);
    }
    if (method == "resources/read")
    {
        const cld::JsonValue* params = JsonMember(rpc, "params");
        if (!params || params->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "resources/read requires params."));
        }
        const std::string uri = cld::JsonStringOr(*params, "uri");
        if (uri.empty())
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "resources/read requires uri."));
        }
        ResourceResult resource = m_host->ReadMcpResource(uri);
        if (!resource.ok)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32004, resource.error.empty() ? "Resource read failed." : resource.error));
        }
        std::string content = "{\"uri\":\"" + cld::EscapeJson(resource.uri.empty() ? uri : resource.uri) + "\",\"mimeType\":\"" + cld::EscapeJson(resource.mimeType) + "\"";
        if (!resource.blob.empty())
        {
            content += ",\"blob\":\"" + resource.blob + "\"";
        }
        else
        {
            content += ",\"text\":\"" + cld::EscapeJson(resource.text) + "\"";
        }
        content += "}";
        return JsonResponse(200, "OK", MakeResponse(idJson, "{\"contents\":[" + content + "]}"));
    }
    if (method == "tools/call")
    {
        const cld::JsonValue* params = JsonMember(rpc, "params");
        if (!params || params->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "tools/call requires params."));
        }
        const std::string name = cld::JsonStringOr(*params, "name");
        if (name.empty())
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "tools/call requires name."));
        }
        const cld::JsonValue* arguments = JsonMember(*params, "arguments");
        cld::JsonValue emptyArguments;
        emptyArguments.type = cld::JsonValue::Type::Object;
        if (!arguments)
        {
            arguments = &emptyArguments;
        }
        if (arguments->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "tools/call arguments must be an object."));
        }

        ToolResult toolResult = m_host->CallMcpTool(name, *arguments, (std::max)(1, m_settings.requestTimeoutSeconds) * 1000);
        return JsonResponse(200, "OK", MakeResponse(idJson, ToolResultToJson(toolResult)));
    }

    return JsonResponse(200, "OK", MakeError(idJson, -32601, "Unsupported MCP method."));
}

Server::HttpResponse Server::HandleInitialize(const cld::JsonValue& rpc)
{
    std::string requestedVersion = SupportedProtocolVersion;
    if (const cld::JsonValue* params = JsonMember(rpc, "params"); params && params->type == cld::JsonValue::Type::Object)
    {
        requestedVersion = cld::JsonStringOr(*params, "protocolVersion", SupportedProtocolVersion);
    }
    const std::string negotiatedVersion = IsSupportedProtocolVersion(requestedVersion) ? requestedVersion : SupportedProtocolVersion;
    const std::string sessionId = NewSessionId();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[sessionId] = Session{ negotiatedVersion, false };
    }

    std::string result = "{\"protocolVersion\":\"" + negotiatedVersion + "\","
        "\"capabilities\":{\"tools\":{\"listChanged\":false},\"resources\":{\"listChanged\":false},\"prompts\":{\"listChanged\":false}},"
        "\"serverInfo\":{\"name\":\"d3d12lookdevpt\",\"title\":\"D3D12LookDevPT\",\"version\":\"0.1.0\"},"
        "\"instructions\":\"Use lookdevpt.* tools to inspect and control the local D3D12LookDevPT session.\"}";
    HttpResponse response = JsonResponse(200, "OK", MakeResponse(JsonIdToJson(rpc), result));
    response.headers.push_back({ "MCP-Session-Id", sessionId });
    AppendLog("initialize");
    return response;
}

Server::HttpResponse Server::HandlePromptGet(const std::string& idJson, const cld::JsonValue& rpc)
{
    const cld::JsonValue* params = JsonMember(rpc, "params");
    if (!params || params->type != cld::JsonValue::Type::Object)
    {
        return JsonResponse(200, "OK", MakeError(idJson, -32602, "prompts/get requires params."));
    }
    const std::string name = cld::JsonStringOr(*params, "name");
    if (name.empty())
    {
        return JsonResponse(200, "OK", MakeError(idJson, -32602, "prompts/get requires name."));
    }
    const cld::JsonValue* arguments = JsonMember(*params, "arguments");
    bool found = false;
    const std::string result = BuildPromptGetResultJson(name, arguments, found);
    if (!found)
    {
        return JsonResponse(200, "OK", MakeError(idJson, -32004, "Unknown prompt."));
    }
    return JsonResponse(200, "OK", MakeResponse(idJson, result));
}

Server::HttpResponse Server::HandleDeleteSession(const HttpRequest& request)
{
    const std::string sessionId = HeaderValue(request.headers, "mcp-session-id");
    if (sessionId.empty())
    {
        return JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32002, "MCP-Session-Id is required."));
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end())
        {
            return JsonResponse(404, "Not Found", MakeHttpErrorBody(-32002, "Unknown MCP session."));
        }
        m_sessions.erase(it);
    }
    AppendLog("DELETE session");
    return JsonResponse(202, "Accepted", "");
}

bool Server::ValidateOrigin(const HttpRequest& request) const
{
    const std::string origin = HeaderValue(request.headers, "origin");
    if (origin.empty() || origin == "null")
    {
        return true;
    }
    return StartsWith(origin, "http://127.0.0.1:") || StartsWith(origin, "http://localhost:");
}

bool Server::ValidateAuthorization(const HttpRequest& request) const
{
    const std::string authorization = HeaderValue(request.headers, "authorization");
    return authorization == "Bearer " + m_settings.token;
}

bool Server::ValidateProtocolHeader(const HttpRequest& request, std::string& diagnostics) const
{
    const std::string version = HeaderValue(request.headers, "mcp-protocol-version");
    if (version.empty())
    {
        return true;
    }
    if (!IsSupportedProtocolVersion(version))
    {
        diagnostics = "Unsupported MCP-Protocol-Version.";
        return false;
    }
    return true;
}

bool Server::ResolveSession(const HttpRequest& request, Session& session, std::string& diagnostics) const
{
    const std::string sessionId = HeaderValue(request.headers, "mcp-session-id");
    if (sessionId.empty())
    {
        diagnostics = "MCP-Session-Id is required.";
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end())
    {
        diagnostics = "Unknown MCP session.";
        return false;
    }
    session = it->second;
    return true;
}

void Server::MarkSessionInitialized(const HttpRequest& request)
{
    const std::string sessionId = HeaderValue(request.headers, "mcp-session-id");
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sessions.find(sessionId);
    if (it != m_sessions.end())
    {
        it->second.initialized = true;
    }
}

void Server::AppendLog(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recentRequests.push_back(message);
    if (m_recentRequests.size() > 12)
    {
        m_recentRequests.erase(m_recentRequests.begin(), m_recentRequests.begin() + (m_recentRequests.size() - 12));
    }
}

std::string GenerateToken(size_t byteCount)
{
    std::random_device randomDevice;
    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (size_t i = 0; i < byteCount; ++i)
    {
        token << std::setw(2) << (randomDevice() & 0xffu);
    }
    return token.str();
}

std::string AccessModeName(AccessMode mode)
{
    switch (mode)
    {
    case AccessMode::ReadOnly:
        return "read_only";
    case AccessMode::AllowMutations:
        return "allow_mutations";
    case AccessMode::ConfirmMutations:
    default:
        return "confirm_mutations";
    }
}

AccessMode AccessModeFromName(const std::string& name, AccessMode fallback)
{
    if (name == "read_only")
    {
        return AccessMode::ReadOnly;
    }
    if (name == "allow_mutations")
    {
        return AccessMode::AllowMutations;
    }
    if (name == "confirm_mutations")
    {
        return AccessMode::ConfirmMutations;
    }
    return fallback;
}

std::string BuildActionsSchemaJson()
{
    return R"json({
  "actions": [
    {"method":"set_scene","description":"Load a scene and optional environment map.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"scenePath":{"type":"string"},"environmentPath":{"type":"string"}},"additionalProperties":false}},
    {"method":"set_camera","description":"Set camera position and yaw/pitch in radians.","inputSchema":{"type":"object","properties":{"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"yaw":{"type":"number"},"pitch":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_material","description":"Override one material by index or name, including factors and texture slot overrides.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"baseColor":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"emissive":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"roughness":{"type":"number"},"metallic":{"type":"number"},"occlusionStrength":{"type":"number"},"normalStrength":{"type":"number"},"alphaCutoff":{"type":"number"},"alphaMasked":{"type":"boolean"},"packedORM":{"type":"boolean"},"textures":{"type":"object","properties":{"baseColor":{"type":"string"},"normal":{"type":"string"},"roughness":{"type":"string"},"metallic":{"type":"string"},"occlusion":{"type":"string"},"emissive":{"type":"string"}},"additionalProperties":false},"clearTextures":{"type":"array","items":{"oneOf":[{"type":"integer"},{"type":"string"}]}},"resetToSource":{"type":"boolean"}},"additionalProperties":false}},
    {"method":"set_material_texture","description":"Set, clear, or reset one material texture slot.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"slot":{"oneOf":[{"type":"integer"},{"type":"string"}]},"path":{"type":"string"},"clear":{"type":"boolean"},"resetToSource":{"type":"boolean"}},"required":["slot"],"additionalProperties":false}},
    {"method":"reset_material","description":"Reset one material back to imported source values and textures.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"}},"additionalProperties":false}},
    {"method":"save_material_variant","description":"Save or replace a named per-material variant snapshot.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"variant":{"type":"string"},"variantName":{"type":"string"}},"additionalProperties":false}},
    {"method":"apply_material_variant","description":"Apply a named or indexed material variant.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"variant":{"type":"string"},"variantName":{"type":"string"},"variantIndex":{"type":"integer"}},"additionalProperties":false}},
    {"method":"delete_material_variant","description":"Delete a named or indexed material variant.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"variant":{"type":"string"},"variantName":{"type":"string"},"variantIndex":{"type":"integer"}},"additionalProperties":false}},
    {"method":"set_material_view","description":"Set selected material and material focus display mode.","inputSchema":{"type":"object","properties":{"selectedMaterial":{"type":"integer"},"focusMode":{"type":"string","enum":["normal","isolate","dim"]}},"additionalProperties":false}},
    {"method":"set_color_management","description":"Set final view exposure, gamma, and tone mapper.","inputSchema":{"type":"object","properties":{"exposure":{"type":"number"},"gamma":{"type":"number"},"toneMapper":{"type":"string","enum":["none","reinhard","aces"]}},"additionalProperties":false}},
    {"method":"set_lighting","description":"Set direct, sky, environment, and light sampling controls.","inputSchema":{"type":"object","properties":{"direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"intensity":{"type":"number"},"rayTMin":{"type":"number"},"skyEnabled":{"type":"boolean"},"skyColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyHorizonColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyZenithColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyGroundColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyIntensity":{"type":"number"},"sunIntensity":{"type":"number"},"sunSize":{"type":"number"},"environmentEnabled":{"type":"boolean"},"environmentIntensity":{"type":"number"},"environmentRotation":{"type":"number"},"sunNEE":{"type":"boolean"},"skyNEE":{"type":"boolean"},"emissiveTriangleLights":{"type":"boolean"},"emissiveIntensity":{"type":"number"},"proceduralAreaLights":{"type":"boolean"},"areaLightIntensity":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_path_tracing","description":"Set path tracing mode and sampling controls.","inputSchema":{"type":"object","properties":{"mode":{"type":"string","enum":["baseline","path_tracing","restir_gi","restir_di","restir_gi_di","combined"]},"samplesPerFrame":{"type":"integer"},"maxBounces":{"type":"integer"},"minBounces":{"type":"integer"},"radianceClamp":{"type":"number"},"temporalClamp":{"type":"number"},"maxAccumSamples":{"type":"integer"},"freezeAccumulation":{"type":"boolean"},"adaptiveSampling":{"type":"boolean"},"maxAdaptiveSPP":{"type":"integer"},"varianceThreshold":{"type":"number"},"disocclusionBoost":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_restir","description":"Set GI/DI ReSTIR reuse and reservoir controls.","inputSchema":{"type":"object","properties":{"temporalReuse":{"type":"boolean"},"spatialReusePasses":{"type":"integer"},"spatialRadius":{"type":"integer"},"candidateSamples":{"type":"integer"},"mClamp":{"type":"number"},"diTemporalReuse":{"type":"boolean"},"diSpatialReusePasses":{"type":"integer"},"diCandidateSamples":{"type":"integer"},"diMClamp":{"type":"number"},"reservoirReprojection":{"type":"boolean"},"reservoirValidation":{"type":"boolean"},"giValidationRay":{"type":"boolean"},"reservoirMaxAge":{"type":"integer"}},"additionalProperties":false}},
    {"method":"set_denoise","description":"Set denoiser backend, DLSS mode, temporal stability, jitter, and realtime reconstruction controls.","inputSchema":{"type":"object","properties":{"preset":{"type":"string","enum":["interactive_stable","sharp_preview","still_capture"]},"backend":{"type":"string","enum":["internal","dlss_rr","off"]},"dlssMode":{"type":"string","enum":["quality","balanced","performance","ultra_performance"]},"dlssEnabledWhenAvailable":{"type":"boolean"},"resetDlss":{"type":"boolean"},"enabled":{"type":"boolean"},"splitSignalDenoise":{"type":"boolean"},"realtimeReconstruction":{"type":"boolean"},"cameraJitter":{"type":"boolean"},"temporalStability":{"type":"boolean"},"jitterMode":{"type":"string","enum":["stable16","halton","off"]},"movingJitterScale":{"type":"number"},"resetHistory":{"type":"boolean"},"maxHistoryFrames":{"type":"integer"},"temporalAlphaMin":{"type":"number"},"temporalAlphaMax":{"type":"number"},"historyClampSigma":{"type":"number"},"reactiveThreshold":{"type":"number"},"specularHistoryScale":{"type":"number"},"spatialIterations":{"type":"integer"},"atrousPasses":{"type":"integer"},"diffuseFilterStrength":{"type":"number"},"specularFilterStrength":{"type":"number"},"varianceScale":{"type":"number"},"normalSigma":{"type":"number"},"depthSigma":{"type":"number"},"luminanceSigma":{"type":"number"},"albedoSigma":{"type":"number"},"strength":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_view","description":"Set active debug view and display toggles.","inputSchema":{"type":"object","properties":{"debugView":{"type":"integer"},"normalMapYFlip":{"type":"boolean"},"environmentEnabled":{"type":"boolean"}},"additionalProperties":false}}
  ]
})json";
}

std::string BuildToolsListJson()
{
    const char* actionSchema = R"json({"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method","params"],"additionalProperties":false})json";
    const char* setCameraSpeedSchema = R"json({"type":"object","properties":{"baseMoveSpeed":{"type":"number"},"fastMoveSpeed":{"type":"number"}},"additionalProperties":false})json";
    const char* fitCameraSchema = R"json({"type":"object","properties":{"padding":{"type":"number"},"preserveOrientation":{"type":"boolean"},"yaw":{"type":"number"},"pitch":{"type":"number"}},"additionalProperties":false})json";
    const char* displayResolutionSchema = R"json({"type":"object","properties":{"preset":{"type":"string","enum":["720p","1080p","4k"]},"width":{"type":"integer"},"height":{"type":"integer"}},"additionalProperties":false})json";
    const char* projectPathSchema = R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})json";
    const char* runActionsSchema = R"json({"type":"object","properties":{"actions":{"type":"array","items":{"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method","params"],"additionalProperties":false},"minItems":1,"maxItems":16},"validateOnly":{"type":"boolean"},"stopOnError":{"type":"boolean"}},"required":["actions"],"additionalProperties":false})json";
    const char* captureDebugPackSchema = R"json({"type":"object","properties":{"views":{"type":"array","items":{"oneOf":[{"type":"integer"},{"type":"string"}]},"maxItems":8},"restoreView":{"type":"boolean"}},"additionalProperties":false})json";
    std::vector<std::string> tools;
    tools.push_back(ToolJson("lookdevpt.get_stats", "Get renderer stats", "Return DXR, scene, renderer, ReSTIR, and denoiser stats.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.get_state", "Get renderer state", "Return current scene, camera, lighting, path tracing, ReSTIR, denoise, and view state.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.list_materials", "List materials", "Return material names, usage counts, editable factors, and texture slot state.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.list_debug_views", "List debug views", "Return debug view ids, labels, and keys.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.list_render_modes", "List render modes", "Return path tracing mode labels and action values.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.get_diagnostics", "Get diagnostics", "Return scene, project, capture, and MCP diagnostics.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.capture_viewport", "Capture viewport", "Capture the current path-traced viewport as PNG.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.capture_debug_pack", "Capture debug pack", "Capture up to eight debug views as PNG resources.", captureDebugPackSchema));
    tools.push_back(ToolJson("lookdevpt.validate_action", "Validate action", "Validate an action without applying it.", actionSchema));
    tools.push_back(ToolJson("lookdevpt.run_actions", "Run actions", "Validate and run multiple action-layer mutations as one MCP request.", runActionsSchema));
    tools.push_back(ToolJson("lookdevpt.reset_accumulation", "Reset accumulation", "Reset progressive accumulation.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.reset_denoise_history", "Reset denoise history", "Invalidate denoise temporal history.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.reset_reservoirs", "Reset reservoirs", "Reset accumulation, ReSTIR reservoirs, and denoise history.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.reset_camera_view", "Reset camera view", "Return the camera to the default scene view.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.set_camera_speed", "Set camera speed", "Set WASD base and fast camera speeds.", setCameraSpeedSchema));
    tools.push_back(ToolJson("lookdevpt.fit_camera_to_scene", "Fit camera to scene", "Move the camera so the current scene bounds fit in view.", fitCameraSchema));
    tools.push_back(ToolJson("lookdevpt.set_display_resolution", "Set display resolution", "Resize the application viewport/window to a preset or custom resolution.", displayResolutionSchema));
    tools.push_back(ToolJson("lookdevpt.load_project", "Load project", "Load a local .lookdevpt.json project file.", projectPathSchema));
    tools.push_back(ToolJson("lookdevpt.save_project", "Save project", "Save the current project path.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.save_project_as", "Save project as", "Save the project to a local .lookdevpt.json path.", projectPathSchema));

    const std::string actions = BuildActionsSchemaJson();
    cld::JsonValue root = cld::JsonParser(actions).Parse();
    const cld::JsonValue* actionArray = cld::FindMember(root, "actions");
    if (actionArray && actionArray->type == cld::JsonValue::Type::Array)
    {
        for (const cld::JsonValue& action : actionArray->array)
        {
            const std::string method = cld::JsonStringOr(action, "method");
            const cld::JsonValue* inputSchema = cld::FindMember(action, "inputSchema");
            const std::string description = cld::JsonStringOr(action, "description");
            if (!method.empty() && inputSchema)
            {
                const std::string toolName = "lookdevpt." + method;
                tools.push_back(ToolJson(toolName.c_str(), method.c_str(), description.c_str(), cld::JsonValueToJson(*inputSchema).c_str()));
            }
        }
    }

    std::ostringstream out;
    out << "{\"tools\":[";
    AppendJoined(out, tools);
    out << "]}";
    return out.str();
}

std::string BuildResourcesListJson()
{
    std::vector<std::string> resources;
    resources.push_back(ResourceJson("lookdevpt://state", "state", "D3D12LookDevPT State", "Current renderer state as JSON.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://stats", "stats", "D3D12LookDevPT Stats", "Current renderer stats as JSON.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://diagnostics", "diagnostics", "Diagnostics", "Scene, project, capture, and MCP diagnostics.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://materials", "materials", "Materials", "Scene material list as JSON.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://material-variants", "material_variants", "Material Variants", "Saved per-material variant snapshots.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://material-presets", "material_presets", "Material Presets", "Built-in and user material presets.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://debug-views", "debug_views", "Debug Views", "Available debug view ids and labels.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://render-modes", "render_modes", "Render Modes", "Available render modes and action values.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://project", "project", "Project", "Current project path and dirty state.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://scene/summary", "scene_summary", "Scene Summary", "Loaded scene counts, bounds, lights, and asset paths.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://actions/schema", "actions_schema", "Action Schema", "Action names and input schemas.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://captures/index", "captures_index", "Capture Index", "In-memory MCP capture history.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://captures/latest.png", "latest_capture", "Latest Viewport Capture", "Most recent viewport capture.", "image/png"));
    std::ostringstream out;
    out << "{\"resources\":[";
    AppendJoined(out, resources);
    out << "]}";
    return out.str();
}

std::string BuildResourceTemplatesListJson()
{
    std::vector<std::string> templates;
    templates.push_back(ResourceTemplateJson("lookdevpt://captures/{id}.png", "capture_by_id", "Capture By Id", "Read an in-memory PNG capture by id.", "image/png"));
    templates.push_back(ResourceTemplateJson("lookdevpt://materials/{index}", "material_by_index", "Material By Index", "Read one material JSON object by material index.", "application/json"));
    templates.push_back(ResourceTemplateJson("lookdevpt://materials/{index}/textures", "material_textures_by_index", "Material Textures By Index", "Read texture slots and override state for one material.", "application/json"));
    std::ostringstream out;
    out << "{\"resourceTemplates\":[";
    AppendJoined(out, templates);
    out << "]}";
    return out.str();
}

std::string BuildPromptsListJson()
{
    std::vector<std::string> prompts;
    prompts.push_back(PromptJson("lookdevpt.inspect_scene", "Inspect Scene", "Inspect the current renderer state, stats, materials, and diagnostics."));
    prompts.push_back(PromptJson("lookdevpt.tune_denoise", "Tune Denoise", "Tune temporal denoise settings for a stable LookDev viewport."));
    prompts.push_back(PromptJson("lookdevpt.setup_camera_shot", "Setup Camera Shot", "Create or refine a camera shot using scene bounds and current state."));
    prompts.push_back(PromptJson("lookdevpt.capture_debug_review", "Capture Debug Review", "Capture key debug views and summarize rendering issues."));
    std::ostringstream out;
    out << "{\"prompts\":[";
    AppendJoined(out, prompts);
    out << "]}";
    return out.str();
}

std::string BuildPromptGetResultJson(const std::string& name, const cld::JsonValue* arguments, bool& found)
{
    (void)arguments;
    found = true;
    if (name == "lookdevpt.inspect_scene")
    {
        return PromptTextResult("Inspect the active D3D12LookDevPT scene.",
            "Inspect the current D3D12LookDevPT session. First call lookdevpt.get_state, lookdevpt.get_stats, lookdevpt.list_materials, and lookdevpt.get_diagnostics. Then summarize scene scale, camera position, render mode, denoise state, material count, active lights, and any obvious setup issues. Do not mutate state.");
    }
    if (name == "lookdevpt.tune_denoise")
    {
        return PromptTextResult("Tune denoise settings for stable LookDev.",
            "Tune the D3D12LookDevPT denoise settings for a stable interactive viewport. Read state/stats first, validate the proposed set_denoise parameters with lookdevpt.validate_action, then apply lookdevpt.set_denoise only if mutation access is allowed. Prefer interactive_stable for camera work and still_capture for static review. Confirm the result with lookdevpt.get_state.");
    }
    if (name == "lookdevpt.setup_camera_shot")
    {
        return PromptTextResult("Set up a camera shot.",
            "Set up a camera shot in D3D12LookDevPT. Read lookdevpt://scene/summary and lookdevpt.get_state, optionally call lookdevpt.fit_camera_to_scene, then use lookdevpt.set_camera for final position/yaw/pitch. Confirm with lookdevpt.get_state and capture the viewport if requested.");
    }
    if (name == "lookdevpt.capture_debug_review")
    {
        return PromptTextResult("Capture debug views for review.",
            "Capture a debug review pack from D3D12LookDevPT. Call lookdevpt.capture_debug_pack with Final, Base Color, World Normal, Roughness, Metallic, Direct Signal, Indirect Signal, and History Confidence. Then read lookdevpt://captures/index and summarize visible render/debug issues.");
    }
    found = false;
    return "{}";
}
}
