// winsock2.h must precede any windows.h include.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "EditorMcpHttp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Editor::Mcp {

namespace {

constexpr size_t k_maxConnections = 16;
constexpr DWORD  k_receiveTimeoutMs = 10000;

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

const char* Reason(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

std::string WsaErrorText(const char* what, int code)
{
    char message[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, message, sizeof(message), nullptr);
    std::string text = message;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                             text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    char buf[400];
    snprintf(buf, sizeof(buf), "%s failed: %s (WSA %d)", what, text.c_str(), code);
    return buf;
}

void SendAll(SOCKET s, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        const int chunk = static_cast<int>((std::min)(data.size() - sent, size_t(1) << 20));
        const int n = send(s, data.data() + sent, chunk, 0);
        if (n <= 0)
            return;
        sent += static_cast<size_t>(n);
    }
}

HttpResponse TextResponse(int status, const char* text)
{
    HttpResponse r;
    r.status = status;
    r.contentType = "text/plain";
    r.body = text;
    return r;
}

} // namespace

ParseStatus ParseHttpRequest(std::string_view data, HttpRequest& out)
{
    const size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos)
        return data.size() > k_maxHeader ? ParseStatus::TooLarge : ParseStatus::Incomplete;
    if (headerEnd + 4 > k_maxHeader)
        return ParseStatus::TooLarge;

    HttpRequest request;
    std::string_view head = data.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? sp1 : requestLine.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || sp1 == 0)
        return ParseStatus::Invalid;
    request.method = std::string(requestLine.substr(0, sp1));
    const std::string_view target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = requestLine.substr(sp2 + 1);
    if (target.empty() || version.substr(0, 7) != "HTTP/1.")
        return ParseStatus::Invalid;
    request.path = std::string(target.substr(0, target.find('?')));

    while (lineEnd != std::string_view::npos)
    {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string_view line = head.substr(start, lineEnd == std::string_view::npos
                                                             ? std::string_view::npos
                                                             : lineEnd - start);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return ParseStatus::Invalid;
        const std::string name = Lower(Trim(line.substr(0, colon)));
        const std::string value(Trim(line.substr(colon + 1)));
        auto [it, inserted] = request.headers.emplace(name, value);
        if (!inserted)
            it->second += ", " + value;
    }

    if (request.headers.count("transfer-encoding"))
        return ParseStatus::Invalid;

    size_t length = 0;
    const auto contentLength = request.headers.find("content-length");
    if (contentLength != request.headers.end())
    {
        const std::string& v = contentLength->second;
        if (v.empty() || v.size() > 12 ||
            !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return ParseStatus::Invalid;
        const unsigned long long parsed = std::stoull(v);
        if (parsed > k_maxBody)
            return ParseStatus::TooLarge;
        length = static_cast<size_t>(parsed);
    }
    else if (request.method == "POST")
    {
        return ParseStatus::Invalid;
    }

    const size_t bodyStart = headerEnd + 4;
    if (data.size() - bodyStart < length)
        return ParseStatus::Incomplete;
    request.body = std::string(data.substr(bodyStart, length));
    out = std::move(request);
    return ParseStatus::Complete;
}

std::string FormatHttpResponse(const HttpResponse& response)
{
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      Reason(response.status) + "\r\n";
    if (!response.contentType.empty())
        out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    for (const auto& [name, value] : response.headers)
        out += name + ": " + value + "\r\n";
    out += "\r\n";
    out += response.body;
    return out;
}

HttpServer::~HttpServer()
{
    Stop();
}

bool HttpServer::Start(uint16_t port, Handler handler, std::string& error)
{
    Stop();
    WSADATA wsa;
    const int startup = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (startup != 0)
    {
        error = WsaErrorText("WSAStartup", startup);
        return false;
    }
    m_wsaStarted = true;

    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        error = WsaErrorText("socket", WSAGetLastError());
        Stop();
        return false;
    }
    // Refuse to share the port with another process (a second editor).
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(s, SOMAXCONN) == SOCKET_ERROR)
    {
        char what[64];
        snprintf(what, sizeof(what), "listening on 127.0.0.1:%u", port);
        error = WsaErrorText(what, WSAGetLastError());
        closesocket(s);
        Stop();
        return false;
    }
    int addrLen = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    m_port = ntohs(addr.sin_port);

    m_listen = static_cast<uintptr_t>(s);
    m_handler = std::move(handler);
    m_stopping = false;
    m_acceptThread = std::thread([this] { AcceptLoop(); });
    return true;
}

void HttpServer::Stop()
{
    m_stopping = true;
    if (m_listen != static_cast<uintptr_t>(INVALID_SOCKET))
    {
        closesocket(static_cast<SOCKET>(m_listen)); // unblocks accept
        m_listen = static_cast<uintptr_t>(INVALID_SOCKET);
    }
    if (m_acceptThread.joinable())
        m_acceptThread.join();
    {
        std::lock_guard lock(m_connMutex);
        for (const auto& c : m_connections)
            if (c->socket != static_cast<uintptr_t>(INVALID_SOCKET))
                shutdown(static_cast<SOCKET>(c->socket), SD_BOTH);
    }
    for (const auto& c : m_connections)
        if (c->thread.joinable())
            c->thread.join();
    m_connections.clear();
    if (m_wsaStarted)
    {
        WSACleanup();
        m_wsaStarted = false;
    }
    m_port = 0;
}

void HttpServer::ReapFinished()
{
    std::lock_guard lock(m_connMutex);
    auto finished = [](const std::unique_ptr<Connection>& c) {
        if (!c->done)
            return false;
        c->thread.join(); // the thread only returns after setting done
        return true;
    };
    m_connections.erase(std::remove_if(m_connections.begin(), m_connections.end(), finished),
                        m_connections.end());
}

void HttpServer::AcceptLoop()
{
    const SOCKET listener = static_cast<SOCKET>(m_listen);
    while (!m_stopping)
    {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            if (m_stopping)
                break;
            Sleep(10); // transient failure (e.g. reset before accept)
            continue;
        }
        ReapFinished();

        std::lock_guard lock(m_connMutex);
        if (m_stopping)
        {
            closesocket(client);
            break;
        }
        if (m_connections.size() >= k_maxConnections)
        {
            SendAll(client, FormatHttpResponse(TextResponse(503, "too many connections")));
            shutdown(client, SD_SEND);
            closesocket(client);
            continue;
        }
        const DWORD timeout = k_receiveTimeoutMs;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        auto connection = std::make_unique<Connection>();
        connection->socket = static_cast<uintptr_t>(client);
        Connection* raw = connection.get();
        m_connections.push_back(std::move(connection));
        raw->thread = std::thread([this, raw] { Serve(*raw); });
    }
}

void HttpServer::Serve(Connection& connection)
{
    const SOCKET s = static_cast<SOCKET>(connection.socket);
    std::string data;
    HttpRequest request;
    ParseStatus status = ParseStatus::Incomplete;
    char buf[64 * 1024];
    while ((status = ParseHttpRequest(data, request)) == ParseStatus::Incomplete)
    {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0)
            break; // closed, timed out or shut down by Stop
        data.append(buf, static_cast<size_t>(n));
    }

    if (status != ParseStatus::Incomplete)
    {
        HttpResponse response;
        if (status == ParseStatus::Invalid)
            response = TextResponse(400, "malformed or unsupported HTTP request");
        else if (status == ParseStatus::TooLarge)
            response = TextResponse(413, "request too large");
        else
        {
            try
            {
                response = m_handler(request);
            }
            catch (...)
            {
                response = TextResponse(500, "internal error");
            }
        }
        SendAll(s, FormatHttpResponse(response));
        shutdown(s, SD_SEND);
    }

    std::lock_guard lock(m_connMutex);
    closesocket(s);
    connection.socket = static_cast<uintptr_t>(INVALID_SOCKET);
    connection.done = true;
}

} // namespace Editor::Mcp
