#include "EditorMcpProtocol.h"

#include <algorithm>
#include <utility>

using nlohmann::json;

namespace Editor::Mcp {

namespace {

constexpr const char* k_latestVersion = "2025-11-25";
constexpr const char* k_supportedVersions[] = { "2025-11-25", "2025-06-18", "2025-03-26" };

constexpr const char* k_instructions =
    "Live FoxEditor session. Start with editor_status. list_entities returns {kind,index} "
    "targets usable by every entity tool. screenshot returns a PNG. Edits are undoable; "
    "apply_scene_json replaces the unsaved edit state with edited FoxLevel JSON. play, "
    "play_input, eval_lua and wait_frames drive Play mode.";

bool IsSupportedVersion(const std::string& version)
{
    return std::find_if(std::begin(k_supportedVersions), std::end(k_supportedVersions),
                        [&](const char* v) { return version == v; }) !=
           std::end(k_supportedVersions);
}

HttpResponse JsonResponse(int status, const json& body)
{
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json";
    r.body = DumpJson(body);
    return r;
}

HttpResponse EmptyResponse(int status)
{
    HttpResponse r;
    r.status = status;
    return r;
}

json ErrorMessage(const json& id, int code, const std::string& message)
{
    return { { "jsonrpc", "2.0" }, { "id", id },
             { "error", { { "code", code }, { "message", message } } } };
}

json ResultMessage(const json& id, json result)
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
}

} // namespace

std::string DumpJson(const json& value, int indent)
{
    return value.dump(indent, ' ', false, json::error_handler_t::replace);
}

ToolResult JsonResult(const json& value)
{
    ToolResult r;
    r.content.push_back({ { "type", "text" }, { "text", DumpJson(value, 2) } });
    return r;
}

ToolResult ErrorResult(const std::string& message)
{
    ToolResult r;
    r.content.push_back({ { "type", "text" }, { "text", message } });
    r.isError = true;
    return r;
}

ToolResult ImageResult(const std::vector<uint8_t>& png, const json& info)
{
    ToolResult r;
    r.content.push_back({ { "type", "image" },
                          { "data", Base64Encode(png.data(), png.size()) },
                          { "mimeType", "image/png" } });
    r.content.push_back({ { "type", "text" }, { "text", DumpJson(info, 2) } });
    return r;
}

std::string Base64Encode(const uint8_t* data, size_t size)
{
    static constexpr char k_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= size; i += 3)
    {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += k_table[(v >> 18) & 63];
        out += k_table[(v >> 12) & 63];
        out += k_table[(v >> 6) & 63];
        out += k_table[v & 63];
    }
    if (i + 1 == size)
    {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += k_table[(v >> 18) & 63];
        out += k_table[(v >> 12) & 63];
        out += "==";
    }
    else if (i + 2 == size)
    {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += k_table[(v >> 18) & 63];
        out += k_table[(v >> 12) & 63];
        out += k_table[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

Protocol::Protocol(std::vector<ToolSpec> tools, CallTool call)
    : m_tools(std::move(tools)), m_toolList(json::array()), m_call(std::move(call))
{
    for (const ToolSpec& tool : m_tools)
    {
        json entry = { { "name", tool.name },
                       { "description", tool.description },
                       { "inputSchema", tool.inputSchema } };
        if (tool.readOnly)
            entry["annotations"] = { { "readOnlyHint", true } };
        m_toolList.push_back(std::move(entry));
    }
}

json Protocol::Dispatch(const std::string& method, const json& params, const json& id) const
{
    if (method == "initialize")
    {
        std::string version = k_latestVersion;
        if (params.is_object() && params.contains("protocolVersion") &&
            params["protocolVersion"].is_string() &&
            IsSupportedVersion(params["protocolVersion"].get<std::string>()))
            version = params["protocolVersion"].get<std::string>();
        return ResultMessage(id, { { "protocolVersion", version },
                                   { "capabilities", { { "tools", { { "listChanged", false } } } } },
                                   { "serverInfo", { { "name", "FoxEditor" }, { "version", "0.1.0" } } },
                                   { "instructions", k_instructions } });
    }
    if (method == "ping")
        return ResultMessage(id, json::object());
    if (method == "tools/list")
        return ResultMessage(id, { { "tools", m_toolList } });
    if (method == "tools/call")
    {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
            return ErrorMessage(id, -32602, "tools/call needs a string 'name'");
        const std::string name = params["name"].get<std::string>();
        const bool known = std::any_of(m_tools.begin(), m_tools.end(),
                                       [&](const ToolSpec& t) { return t.name == name; });
        if (!known)
            return ErrorMessage(id, -32602, "Unknown tool: " + name);
        json arguments = json::object();
        if (params.contains("arguments") && !params["arguments"].is_null())
        {
            if (!params["arguments"].is_object())
                return ErrorMessage(id, -32602, "tools/call 'arguments' must be an object");
            arguments = params["arguments"];
        }
        const ToolResult result = m_call(name, arguments);
        return ResultMessage(id, { { "content", result.content }, { "isError", result.isError } });
    }
    return ErrorMessage(id, -32601, "Method not found: " + method);
}

HttpResponse Protocol::HandlePost(const std::string& body, const std::string& protocolVersionHeader) const
{
    const json message = json::parse(body, nullptr, false);
    if (message.is_discarded())
        return JsonResponse(400, ErrorMessage(nullptr, -32700, "Parse error"));
    if (message.is_array())
        return JsonResponse(200, ErrorMessage(nullptr, -32600, "batches are not supported"));
    if (!message.is_object())
        return JsonResponse(200, ErrorMessage(nullptr, -32600, "Invalid Request"));

    const auto methodIt = message.find("method");
    const bool hasMethod = methodIt != message.end() && methodIt->is_string();
    const std::string method = hasMethod ? methodIt->get<std::string>() : std::string();

    // A client speaking a newer, stateless revision falls back to initialize
    // when its first request is refused with an empty 400.
    if (method != "initialize" && !protocolVersionHeader.empty() &&
        !IsSupportedVersion(protocolVersionHeader))
        return EmptyResponse(400);

    if (message.contains("result") || message.contains("error") || !message.contains("id"))
        return EmptyResponse(202); // notifications and responses need no reply

    const json& id = message["id"];
    if (!id.is_string() && !id.is_number_integer() && !id.is_number_unsigned())
        return JsonResponse(200, ErrorMessage(nullptr, -32600, "Invalid Request: bad id"));
    if (!hasMethod)
        return JsonResponse(200, ErrorMessage(id, -32600, "Invalid Request: missing method"));

    const auto paramsIt = message.find("params");
    const json params = paramsIt != message.end() ? *paramsIt : json::object();
    return JsonResponse(200, Dispatch(method, params, id));
}

HttpResponse RouteMcpRequest(const HttpRequest& request, const Protocol& protocol, uint16_t port)
{
    const std::string p = std::to_string(port);
    const auto header = [&](const char* name) -> const std::string* {
        const auto it = request.headers.find(name);
        return it == request.headers.end() ? nullptr : &it->second;
    };

    const std::string* host = header("host");
    if (!host || (*host != "127.0.0.1:" + p && *host != "localhost:" + p))
        return { 403, "text/plain", "forbidden host", {} };
    const std::string* origin = header("origin");
    if (origin && *origin != "http://127.0.0.1:" + p && *origin != "http://localhost:" + p)
        return { 403, "text/plain", "forbidden origin", {} };
    if (request.path != "/mcp")
        return { 404, "text/plain", "not found; the MCP endpoint is /mcp", {} };
    if (request.method != "POST")
        return { 405, "text/plain", "the MCP endpoint accepts POST only", { { "Allow", "POST" } } };

    const std::string* version = header("mcp-protocol-version");
    return protocol.HandlePost(request.body, version ? *version : std::string());
}

} // namespace Editor::Mcp
