#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Editor::Mcp {

// Minimal HTTP/1.1 for the editor's loopback MCP endpoint: one request per
// connection, Content-Length bodies only.
struct HttpRequest
{
    std::string method;
    std::string path; // without the query string
    std::map<std::string, std::string> headers; // keys lowercased
    std::string body;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

enum class ParseStatus { Incomplete, Complete, Invalid, TooLarge };

constexpr size_t k_maxBody   = 16u << 20;
constexpr size_t k_maxHeader = 64u << 10;

ParseStatus ParseHttpRequest(std::string_view data, HttpRequest& out);
// Status line, Content-Type (if set), Content-Length, Connection: close,
// extra headers, body.
std::string FormatHttpResponse(const HttpResponse& response);

class HttpServer
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds 127.0.0.1:port (0 = any free port); serves each connection on
    // its own thread. The handler runs on those threads.
    bool Start(uint16_t port, Handler handler, std::string& error);
    // Closes the listen and client sockets and joins all threads.
    void Stop();
    uint16_t Port() const { return m_port; }

private:
    struct Connection
    {
        uintptr_t   socket = 0; // SOCKET; closed under m_connMutex
        std::thread thread;
        bool        done = false;
    };

    void AcceptLoop();
    void Serve(Connection& connection);
    void ReapFinished();

    Handler           m_handler;
    uintptr_t         m_listen = ~uintptr_t(0); // INVALID_SOCKET
    std::thread       m_acceptThread;
    std::atomic<bool> m_stopping{ false };
    bool              m_wsaStarted = false;
    uint16_t          m_port = 0;

    std::mutex                               m_connMutex;
    std::vector<std::unique_ptr<Connection>> m_connections;
};

} // namespace Editor::Mcp
