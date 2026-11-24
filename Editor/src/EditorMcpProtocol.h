#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "EditorMcpHttp.h"

namespace Editor::Mcp {

// MCP (Streamable HTTP, stateless, JSON responses) for the editor's tools.

struct ToolResult
{
    nlohmann::json content = nlohmann::json::array();
    bool isError = false;
};

ToolResult JsonResult(const nlohmann::json& value); // one text item, value.dump(2)
ToolResult ErrorResult(const std::string& message);
// An image item (base64 PNG) followed by a text item holding info.
ToolResult ImageResult(const std::vector<uint8_t>& png, const nlohmann::json& info);
std::string Base64Encode(const uint8_t* data, size_t size);
// JSON text that never throws on invalid UTF-8 (replaced with U+FFFD).
std::string DumpJson(const nlohmann::json& value, int indent = -1);

struct ToolSpec
{
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
    bool readOnly = false;
};

// Thread-safe; blocks until the editor finished the call.
using CallTool = std::function<ToolResult(const std::string& name, const nlohmann::json& arguments)>;

// Immutable after construction; HandlePost runs on connection threads.
class Protocol
{
public:
    Protocol(std::vector<ToolSpec> tools, CallTool call);
    HttpResponse HandlePost(const std::string& body, const std::string& protocolVersionHeader) const;

private:
    nlohmann::json Dispatch(const std::string& method, const nlohmann::json& params,
                            const nlohmann::json& id) const;

    std::vector<ToolSpec> m_tools;
    nlohmann::json        m_toolList;
    CallTool              m_call;
};

// Host/Origin (DNS rebinding guard), path and method checks, then HandlePost.
HttpResponse RouteMcpRequest(const HttpRequest& request, const Protocol& protocol, uint16_t port);

} // namespace Editor::Mcp
