// winsock2.h must precede windows.h (pulled in by engine headers).
#include <winsock2.h>
#include <ws2tcpip.h>

#include "EditorMcpHttp.h"
#include "EditorMcpProtocol.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using nlohmann::json;
using namespace Editor::Mcp;

namespace {

void Require(bool value, const char* message)
{
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

constexpr uint16_t k_port = 47800;

HttpRequest Post(const std::string& body, const std::string& path = "/mcp")
{
    HttpRequest r;
    r.method = "POST";
    r.path = path;
    r.headers["host"] = "127.0.0.1:47800";
    r.body = body;
    return r;
}

Protocol MakeProtocol()
{
    std::vector<ToolSpec> tools = {
        { "echo", "Returns its arguments", { { "type", "object" } }, false },
        { "fail", "Always fails", { { "type", "object" } }, true },
    };
    return Protocol(std::move(tools), [](const std::string& name, const json& args) {
        return name == "echo" ? JsonResult(args) : ErrorResult("boom");
    });
}

json Rpc(const Protocol& protocol, const json& message, const std::string& version = "2025-11-25",
         int expectStatus = 200)
{
    const HttpResponse r = protocol.HandlePost(message.dump(), version);
    Require(r.status == expectStatus, "unexpected HTTP status");
    return json::parse(r.body);
}

void TestParse()
{
    const std::string full =
        "POST /mcp?x=1 HTTP/1.1\r\nHost: 127.0.0.1:47800\r\nContent-Type: application/json\r\n"
        "Content-Length: 11\r\n\r\n{\"a\":12345}";
    HttpRequest request;
    Require(ParseHttpRequest(full.substr(0, 60), request) == ParseStatus::Incomplete,
            "partial headers are incomplete");
    Require(ParseHttpRequest(full.substr(0, full.size() - 3), request) == ParseStatus::Incomplete,
            "partial body is incomplete");
    Require(ParseHttpRequest(full, request) == ParseStatus::Complete, "full request parses");
    Require(request.method == "POST" && request.path == "/mcp", "request line");
    Require(request.body == "{\"a\":12345}", "exact body");
    Require(request.headers.at("content-type") == "application/json", "lowercased header keys");

    Require(ParseHttpRequest("POST /mcp HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
                             request) == ParseStatus::Invalid, "chunked is invalid");
    Require(ParseHttpRequest("POST /mcp HTTP/1.1\r\nHost: x\r\n\r\n", request) == ParseStatus::Invalid,
            "POST without Content-Length is invalid");
    Require(ParseHttpRequest("POST /mcp HTTP/1.1\r\nContent-Length: " + std::to_string(k_maxBody + 1) +
                             "\r\n\r\n", request) == ParseStatus::TooLarge, "oversized body");
    Require(ParseHttpRequest("GET /mcp HTTP/1.1\r\nHost: x\r\n\r\n", request) == ParseStatus::Complete,
            "GET without body parses");
}

void TestRouting()
{
    const Protocol protocol = MakeProtocol();
    const std::string ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

    HttpRequest evilHost = Post(ping);
    evilHost.headers["host"] = "evil.com:47800";
    Require(RouteMcpRequest(evilHost, protocol, k_port).status == 403, "foreign Host is refused");

    HttpRequest evilOrigin = Post(ping);
    evilOrigin.headers["origin"] = "http://evil.com";
    Require(RouteMcpRequest(evilOrigin, protocol, k_port).status == 403, "foreign Origin is refused");

    HttpRequest localOrigin = Post(ping);
    localOrigin.headers["origin"] = "http://localhost:47800";
    Require(RouteMcpRequest(localOrigin, protocol, k_port).status == 200, "local Origin is accepted");

    HttpRequest get = Post("");
    get.method = "GET";
    const HttpResponse getResponse = RouteMcpRequest(get, protocol, k_port);
    bool allow = false;
    for (const auto& [name, value] : getResponse.headers)
        allow |= name == "Allow" && value == "POST";
    Require(getResponse.status == 405 && allow, "GET is 405 with Allow: POST");

    Require(RouteMcpRequest(Post(ping, "/x"), protocol, k_port).status == 404, "other paths are 404");
}

void TestProtocol()
{
    const Protocol protocol = MakeProtocol();

    json r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" },
                             { "params", { { "protocolVersion", "2025-06-18" } } } }, "");
    Require(r["result"]["protocolVersion"] == "2025-06-18", "supported version is echoed");
    Require(r["result"]["serverInfo"]["name"] == "FoxEditor", "server name");

    r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "initialize" },
                        { "params", { { "protocolVersion", "2099-01-01" } } } }, "2099-01-01");
    Require(r["result"]["protocolVersion"] == "2025-11-25", "unknown version gets the latest");

    HttpResponse raw = protocol.HandlePost(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                                           "2025-11-25");
    Require(raw.status == 202 && raw.body.empty(), "notifications are 202 without body");

    raw = protocol.HandlePost(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})", "2026-07-28");
    Require(raw.status == 400 && raw.body.empty(), "newer protocol revision is 400 with empty body");

    r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", 4 }, { "method", "tools/list" } });
    const json& tools = r["result"]["tools"];
    Require(tools.size() == 2 && tools[0]["name"] == "echo", "tools are listed");
    Require(!tools[0].contains("annotations"), "mutating tool has no readOnlyHint");
    Require(tools[1]["annotations"]["readOnlyHint"] == true, "read tool has readOnlyHint");

    r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", "abc" }, { "method", "tools/call" },
                        { "params", { { "name", "echo" }, { "arguments", { { "a", 1 } } } } } });
    Require(r["id"] == "abc", "string id is echoed");
    Require(r["result"]["isError"] == false, "echo succeeds");
    Require(json::parse(r["result"]["content"][0]["text"].get<std::string>()) == json{ { "a", 1 } },
            "echo returns its arguments as JSON text");

    r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", 5 }, { "method", "tools/call" },
                        { "params", { { "name", "fail" } } } });
    Require(r["result"]["isError"] == true && r["result"]["content"][0]["text"] == "boom",
            "tool failure is an isError result");

    r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", 6 }, { "method", "tools/call" },
                        { "params", { { "name", "nope" } } } });
    Require(r["error"]["code"] == -32602, "unknown tool is -32602");

    r = Rpc(protocol, { { "jsonrpc", "2.0" }, { "id", 7 }, { "method", "resources/list" } });
    Require(r["error"]["code"] == -32601, "unknown method is -32601");

    raw = protocol.HandlePost("{", "");
    Require(raw.status == 400 && json::parse(raw.body)["error"]["code"] == -32700, "parse error");

    raw = protocol.HandlePost("[]", "");
    Require(raw.status == 200 && json::parse(raw.body)["error"]["code"] == -32600, "batches refused");

    Require(Base64Encode(reinterpret_cast<const uint8_t*>("foobar"), 6) == "Zm9vYmFy" &&
            Base64Encode(reinterpret_cast<const uint8_t*>("fooba"), 5) == "Zm9vYmE=" &&
            Base64Encode(reinterpret_cast<const uint8_t*>("foob"), 4) == "Zm9vYg==",
            "base64 padding");
}

SOCKET Connect(uint16_t port)
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0, "connect");
    return s;
}

void TestSocketRoundTrip()
{
    WSADATA wsa;
    Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup");
    const Protocol protocol = MakeProtocol();
    HttpServer server;
    std::string error;
    uint16_t port = 0;
    Require(server.Start(0, [&protocol, &port](const HttpRequest& request) {
                return RouteMcpRequest(request, protocol, port);
            }, error), "server starts on a free port");
    port = server.Port();
    Require(port != 0, "server reports its port");

    const std::string body =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})";
    const std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                "\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
    const SOCKET client = Connect(port);
    Require(send(client, request.data(), static_cast<int>(request.size()), 0) ==
            static_cast<int>(request.size()), "send request");
    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(client, buf, sizeof(buf), 0)) > 0)
        response.append(buf, static_cast<size_t>(n));
    closesocket(client);
    Require(response.rfind("HTTP/1.1 200", 0) == 0, "200 response");
    const size_t split = response.find("\r\n\r\n");
    Require(split != std::string::npos, "response has a header block");
    Require(json::parse(response.substr(split + 4))["result"]["serverInfo"]["name"] == "FoxEditor",
            "response body is the initialize result");

    // Stop must not wait for a client that never sends its request.
    const SOCKET idle = Connect(port);
    server.Stop();
    closesocket(idle);
    WSACleanup();
}

} // namespace

int main()
{
    TestParse();
    TestRouting();
    TestProtocol();
    TestSocketRoundTrip();
    std::printf("EditorMcpTests passed\n");
    return 0;
}
