#include "EditorMcpTools.h"

#include "EditorCamera.h"
#include "EditorConsole.h"
#include "EditorEntityOps.h"
#include "EditorOutliner.h"
#include "EditorUndo.h"
#include "EditorViewportToolbar.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Clock.h"
#include "Engine/Core/Logger.h"
#include "Engine/Core/Project.h"
#include "Engine/Input/InputManager.h"
#include "Engine/Input/KeyNames.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/RenderTarget.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Scene/SceneSaver.h"
#include "Engine/Scripting/ScriptSystem.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>

using nlohmann::json;
using Editor::Mcp::ToolResult;
using Editor::Mcp::JsonResult;
using Editor::Mcp::ErrorResult;
using Kind = Editor::Selection::Kind;

namespace Editor {

struct McpTools::PendingCall
{
    std::string             name;
    json                    arguments;
    std::promise<ToolResult> promise;
    bool                    done = false; // main thread only
};

struct McpTools::Waiter
{
    CallPtr call;
    Poll    poll;
};

struct McpTools::Capture
{
    CallPtr     call;
    std::string target; // viewport | viewport_ui | window
    uint32_t    maxWidth = 0;
    uint64_t    queuedFrame = 0;
};

struct McpTools::PlayInput
{
    CallPtr                  call;
    std::vector<int>         vks;
    std::vector<std::string> keyNames;
    int32_t                  dx = 0;
    int32_t                  dy = 0;
    int                      frames = 1;
    int                      frame = 1;
};

namespace {

constexpr auto k_callTimeout = std::chrono::seconds(600);
constexpr auto k_loadTimeout = std::chrono::seconds(300);
constexpr uint64_t k_captureFrameLimit = 120;
constexpr const char* k_noViewportImage = "no viewport image (scene loading or viewport hidden)";

struct KindEntry
{
    Kind        kind;
    const char* name;
};
// list_entities order; world is listed only on request.
constexpr KindEntry k_kinds[] = {
    { Kind::World, "world" },          { Kind::Camera, "camera" },
    { Kind::Sun, "sun" },              { Kind::SpotLight, "spot_light" },
    { Kind::PointLight, "point_light" }, { Kind::Mesh, "mesh" },
    { Kind::Object, "object" },        { Kind::Emitter, "emitter" },
    { Kind::Empty, "empty" },          { Kind::Actor, "actor" },
};

const char* KindName(Kind kind)
{
    for (const KindEntry& entry : k_kinds)
        if (entry.kind == kind)
            return entry.name;
    return "none";
}

bool ParseKind(const std::string& name, Kind& out)
{
    for (const KindEntry& entry : k_kinds)
        if (name == entry.name)
        {
            out = entry.kind;
            return true;
        }
    return false;
}

bool IsSingleton(Kind kind)
{
    return kind == Kind::World || kind == Kind::Camera || kind == Kind::Sun ||
           kind == Kind::SpotLight;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string FormatId(uint64_t id)
{
    char text[17];
    sprintf_s(text, "%016llx", static_cast<unsigned long long>(id));
    return text;
}

json IdJson(uint64_t id)
{
    return id == 0 ? json(nullptr) : json(FormatId(id));
}

json Vec(const DirectX::XMFLOAT3& v)
{
    return json::array({ v.x, v.y, v.z });
}

std::string JoinList(const std::vector<std::string>& items)
{
    std::string out;
    for (const std::string& item : items)
        out += (out.empty() ? "" : ", ") + item;
    return out.empty() ? "none" : out;
}

// --- Argument readers: false + error on a present value of the wrong type ---

const json* Arg(const json& args, const char* key)
{
    const auto it = args.find(key);
    return it == args.end() || it->is_null() ? nullptr : &*it;
}

bool ReadBool(const json& args, const char* key, std::optional<bool>& out, std::string& error)
{
    const json* v = Arg(args, key);
    if (!v) return true;
    if (!v->is_boolean())
    {
        error = std::string("'") + key + "' must be a boolean";
        return false;
    }
    out = v->get<bool>();
    return true;
}

bool ReadString(const json& args, const char* key, std::optional<std::string>& out, std::string& error)
{
    const json* v = Arg(args, key);
    if (!v) return true;
    if (!v->is_string())
    {
        error = std::string("'") + key + "' must be a string";
        return false;
    }
    out = v->get<std::string>();
    return true;
}

bool ReadInt(const json& args, const char* key, int64_t min, int64_t max,
             std::optional<int64_t>& out, std::string& error)
{
    const json* v = Arg(args, key);
    if (!v) return true;
    if (!v->is_number_integer() || v->get<int64_t>() < min || v->get<int64_t>() > max)
    {
        error = std::string("'") + key + "' must be an integer " + std::to_string(min) + ".." +
                std::to_string(max);
        return false;
    }
    out = v->get<int64_t>();
    return true;
}

bool ReadNumber(const json& args, const char* key, double min, double max,
                std::optional<double>& out, std::string& error)
{
    const json* v = Arg(args, key);
    if (!v) return true;
    if (!v->is_number() || !std::isfinite(v->get<double>()) || v->get<double>() < min ||
        v->get<double>() > max)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "'%s' must be a number %g..%g", key, min, max);
        error = buf;
        return false;
    }
    out = v->get<double>();
    return true;
}

bool ReadVec3(const json& args, const char* key, std::optional<DirectX::XMFLOAT3>& out,
              std::string& error)
{
    const json* v = Arg(args, key);
    if (!v) return true;
    const bool ok = v->is_array() && v->size() == 3 &&
                    std::all_of(v->begin(), v->end(), [](const json& n) {
                        return n.is_number() && std::isfinite(n.get<double>());
                    });
    if (!ok)
    {
        error = std::string("'") + key + "' must be 3 finite numbers";
        return false;
    }
    out = DirectX::XMFLOAT3{ (*v)[0].get<float>(), (*v)[1].get<float>(), (*v)[2].get<float>() };
    return true;
}

std::string HrText(const char* what, HRESULT hr)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

// Reads a texture back and encodes it as PNG: optional crop (texture pixels),
// opaque alpha, optional downscale to maxWidth.
bool CaptureTexturePng(ID3D11Device* device, ID3D11DeviceContext* context,
                       ID3D11Texture2D* texture, const RECT* crop, uint32_t maxWidth,
                       std::vector<uint8_t>& png, uint32_t& width, uint32_t& height,
                       uint32_t& sourceWidth, uint32_t& sourceHeight, std::string& error)
{
    using namespace DirectX;
    // WIC (Resize, PNG encoding) needs COM on this thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope
    {
        bool owns;
        ~ComScope() { if (owns) CoUninitialize(); }
    } comScope{ com == S_OK || com == S_FALSE };

    ScratchImage captured;
    HRESULT hr = CaptureTexture(device, context, texture, captured);
    if (FAILED(hr))
    {
        error = HrText("CaptureTexture", hr);
        return false;
    }
    const Image* image = captured.GetImage(0, 0, 0);
    ScratchImage converted;
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT,
                     TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr))
        {
            error = HrText("Convert", hr);
            return false;
        }
        image = converted.GetImage(0, 0, 0);
    }

    ScratchImage cropped;
    if (crop)
    {
        const LONG w = static_cast<LONG>(image->width);
        const LONG h = static_cast<LONG>(image->height);
        const LONG x0 = std::clamp(crop->left, 0L, w), x1 = std::clamp(crop->right, 0L, w);
        const LONG y0 = std::clamp(crop->top, 0L, h), y1 = std::clamp(crop->bottom, 0L, h);
        if (x1 <= x0 || y1 <= y0)
        {
            error = "the viewport panel is hidden";
            return false;
        }
        const size_t cw = static_cast<size_t>(x1 - x0), ch = static_cast<size_t>(y1 - y0);
        hr = cropped.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, cw, ch, 1, 1);
        if (SUCCEEDED(hr))
            hr = CopyRectangle(*image, Rect(static_cast<size_t>(x0), static_cast<size_t>(y0), cw, ch),
                               *cropped.GetImage(0, 0, 0), TEX_FILTER_DEFAULT, 0, 0);
        if (FAILED(hr))
        {
            error = HrText("CopyRectangle", hr);
            return false;
        }
        image = cropped.GetImage(0, 0, 0);
    }
    sourceWidth = static_cast<uint32_t>(image->width);
    sourceHeight = static_cast<uint32_t>(image->height);

    // Back-buffer alpha is whatever ImGui blended; the PNG should be opaque.
    for (size_t y = 0; y < image->height; ++y)
    {
        uint8_t* row = image->pixels + y * image->rowPitch;
        for (size_t x = 0; x < image->width; ++x)
            row[x * 4 + 3] = 255;
    }

    ScratchImage resized;
    if (maxWidth != 0 && image->width > maxWidth)
    {
        const size_t h = (std::max)(size_t(1), static_cast<size_t>(std::lround(
            static_cast<double>(image->height) * maxWidth / static_cast<double>(image->width))));
        hr = Resize(*image, maxWidth, h, TEX_FILTER_LINEAR, resized);
        if (FAILED(hr))
            hr = Resize(*image, maxWidth, h, TEX_FILTER_LINEAR | TEX_FILTER_FORCE_NON_WIC, resized);
        if (FAILED(hr))
        {
            error = HrText("Resize", hr);
            return false;
        }
        image = resized.GetImage(0, 0, 0);
    }
    width = static_cast<uint32_t>(image->width);
    height = static_cast<uint32_t>(image->height);

    Blob blob;
    hr = SaveToWICMemory(*image, WIC_FLAGS_NONE, GetWICCodec(WIC_CODEC_PNG), blob);
    if (FAILED(hr))
    {
        error = HrText("PNG encoding", hr);
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
    png.assign(bytes, bytes + blob.GetBufferSize());
    return true;
}

// --- Tool schemas ---

json TargetSchema()
{
    json kinds = json::array();
    for (const KindEntry& entry : k_kinds)
        kinds.push_back(entry.name);
    return {
        { "type", "object" },
        { "description", "Entity: {kind, index} from list_entities, {id} (level id, 1-16 hex digits) "
                         "or {name} (exact and unique). index defaults to 0 for world, camera, sun "
                         "and spot_light." },
        { "properties", { { "kind", { { "type", "string" }, { "enum", kinds } } },
                          { "index", { { "type", "integer" }, { "minimum", 0 } } },
                          { "id", { { "type", "string" } } },
                          { "name", { { "type", "string" } } } } },
        { "additionalProperties", false },
    };
}

Mcp::ToolSpec Spec(const char* name, bool readOnly, const char* description,
                   const char* properties, std::vector<std::string> required = {},
                   bool withTarget = false)
{
    json props = json::parse(properties, nullptr, false);
    if (props.is_discarded())
    {
        SE_LOG_ERROR("MCP: schema of %s is not valid JSON", name);
        props = json::object();
    }
    if (withTarget)
        props["target"] = TargetSchema();
    json schema = { { "type", "object" }, { "properties", props },
                    { "additionalProperties", false } };
    if (!required.empty())
        schema["required"] = required;
    return { name, description, std::move(schema), readOnly };
}

} // namespace

std::vector<Mcp::ToolSpec> McpTools::Specs()
{
    return {
        Spec("editor_status", true,
             "Snapshot of the editor: project, scene, dirty flag, play state, frame, load status, "
             "selection, undo/redo, editor camera, viewport size, view options, render stats, "
             "script session, entity counts and level diagnostics.",
             "{}"),
        Spec("list_entities", true,
             "Entities of the open scene in kind order camera, sun, spot_light, point_light, mesh, "
             "object, emitter, empty, actor. Each has {kind, index, id, name}, its transform and "
             "kind-specific fields. Indices change when entities are added or deleted.",
             R"({"kind":{"type":"string","description":"Only this kind; world lists the level root"},
                 "name_contains":{"type":"string","description":"Case-insensitive name filter"}})"),
        Spec("get_entity", true,
             "One entity's description plus 'level': its FoxLevel JSON object as it would be saved. "
             "Ids of entities that were never saved are reassigned on save.",
             "{}", { "target" }, true),
        Spec("get_scene_json", true,
             "The whole scene as FoxLevel JSON (the unsaved edit state, or the running play state "
             "while playing). Edit it and pass it to apply_scene_json.",
             "{}"),
        Spec("get_logs", true,
             "Editor console lines (engine and script output). Poll with since = next_since of "
             "the previous call to get only new lines.",
             R"({"since":{"type":"integer","minimum":0,"default":0},
                 "min_level":{"type":"string","enum":["debug","info","warning","error"],"default":"info"},
                 "contains":{"type":"string","description":"Case-insensitive substring"},
                 "limit":{"type":"integer","minimum":1,"maximum":2000,"default":100}})"),
        Spec("screenshot", true,
             "PNG of the editor. viewport: the rendered scene without editor overlays; viewport_ui: "
             "the viewport panel with gizmo, grid, icons and play overlay; window: the whole "
             "editor window. Works while the window is minimized or covered.",
             R"({"target":{"type":"string","enum":["viewport","viewport_ui","window"],"default":"viewport"},
                 "max_width":{"type":"integer","minimum":0,"maximum":16384,"default":1280,
                              "description":"Downscale wider images; 0 = native size"}})"),
        Spec("select", false,
             "Select an entity (shown in Outliner/Details, gizmo target) or clear the selection. "
             "Allowed while playing.",
             R"({"clear":{"type":"boolean"}})", {}, true),
        Spec("set_transform", false,
             "Move an entity (undoable). Meshes, empties and actors take position, rotation "
             "(degrees: pitch, yaw, roll) and scale; lights, emitters and objects position only. "
             "Mesh colliders keep their load pose until the scene is reloaded.",
             R"({"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                 "rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                 "scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}})",
             { "target" }, true),
        Spec("rename_entity", false, "Rename an entity (undoable).",
             R"({"name":{"type":"string","minLength":1}})", { "target", "name" }, true),
        Spec("add_entity", false,
             "Add an entity (undoable) and select it. Meshes cannot be added here: add a mesh "
             "entity to the get_scene_json JSON and use apply_scene_json.",
             R"({"type":{"type":"string","enum":["point_light","particle_emitter","sphere","plane","empty","actor"]},
                 "actor_class":{"type":"string","description":"Required for actor: Assets/Scripts/....lua, a fox.actor class"},
                 "position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,
                             "description":"Default: 8 m in front of the editor camera"}})",
             { "type" }),
        Spec("duplicate_entity", false, "Duplicate an entity (undoable) and select the copy.",
             "{}", { "target" }, true),
        Spec("delete_entity", false, "Delete an entity (undoable).", "{}", { "target" }, true),
        Spec("undo", false, "Undo the last edit.", "{}"),
        Spec("redo", false, "Redo the last undone edit.", "{}"),
        Spec("save_scene", false,
             "Save the scene. Without path: to its current file (while playing: the edit state from "
             "before Play). With path (a name, or Assets/Scenes/<name>.json): Save As.",
             R"({"path":{"type":"string"}})"),
        Spec("open_scene", false,
             "Open another scene (a file name in Assets/Scenes or a project-relative path).",
             R"({"path":{"type":"string"},
                 "discard_changes":{"type":"boolean","default":false},
                 "wait":{"type":"boolean","default":true,"description":"Return once the scene is loaded"}})",
             { "path" }),
        Spec("apply_scene_json", false,
             "Replace the unsaved edit state with FoxLevel JSON (e.g. get_scene_json output with "
             "changes). The scene reloads from it, undo history is cleared and the file on disk is "
             "not written (use save_scene).",
             R"({"json":{"type":["object","string"]},
                 "wait":{"type":"boolean","default":true}})",
             { "json" }),
        Spec("set_camera", false,
             "Move the editor camera: eye and/or target (a missing target keeps the view "
             "direction), or a view_mode preset; fov_deg and ortho_height adjust the projection.",
             R"({"eye":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                 "target":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
                 "fov_deg":{"type":"number","minimum":5,"maximum":170},
                 "ortho_height":{"type":"number","exclusiveMinimum":0},
                 "view_mode":{"type":"string","enum":["perspective","isometric","top","bottom","front","back","left","right"]}})"),
        Spec("frame_entity", false, "Point the editor camera at an entity (like F).", "{}",
             { "target" }, true),
        Spec("set_view_options", false,
             "Viewport display options (toolbar toggles, shading mode, gizmo tool).",
             R"({"realtime":{"type":"boolean"},"game_view":{"type":"boolean"},
                 "show_lights":{"type":"boolean"},"show_grid":{"type":"boolean"},
                 "show_axes":{"type":"boolean"},"show_stats":{"type":"boolean"},
                 "immersive":{"type":"boolean"},"show_colliders":{"type":"boolean"},
                 "shading":{"type":"string","enum":["lit","unlit","wireframe"]},
                 "gizmo":{"type":"string","enum":["select","translate","rotate","scale"]}})"),
        Spec("play", false,
             "Start or stop Play mode. Play runs the project's scripts on a copy of the unsaved "
             "edit state; Stop restores it.",
             R"({"action":{"type":"string","enum":["start","stop"]},
                 "wait":{"type":"boolean","default":true}})",
             { "action" }),
        Spec("eval_lua", false,
             "Evaluate Lua in the running Play session (like the Console input) and return the "
             "inspected result.",
             R"({"code":{"type":"string"}})", { "code" }),
        Spec("play_input", false,
             "Feed keyboard/mouse input to the game for a number of frames: keys held (fox.Key "
             "names such as W, Space, MouseLeft), actions (project.json input names, first "
             "keyboard/mouse binding) and a per-frame mouse delta. The real cursor is not moved. "
             "fox.ui (ImGui) does not see injected input.",
             R"({"keys":{"type":"array","items":{"type":"string"}},
                 "actions":{"type":"array","items":{"type":"string"}},
                 "mouse_delta":{"type":"array","items":{"type":"integer"},"minItems":2,"maxItems":2},
                 "frames":{"type":"integer","minimum":1,"maximum":600,"default":1}})"),
        Spec("wait_frames", true, "Wait until the editor has run this many more frames.",
             R"({"frames":{"type":"integer","minimum":1,"maximum":600}})", { "frames" }),
    };
}

McpTools::McpTools(McpEditorAccess access)
    : m_access(std::move(access))
{
}

McpTools::~McpTools()
{
    Stop();
}

bool McpTools::Start(uint16_t port)
{
    m_projectName = SE::Project::Load().project.name;
    m_protocol = std::make_unique<Mcp::Protocol>(
        Specs(), [this](const std::string& name, const json& args) { return Call(name, args); });
    m_server = std::make_unique<Mcp::HttpServer>();
    std::string error;
    const Mcp::Protocol* protocol = m_protocol.get();
    if (!m_server->Start(port, [protocol, port](const Mcp::HttpRequest& request) {
            return Mcp::RouteMcpRequest(request, *protocol, port);
        }, error))
    {
        SE_LOG_WARN("MCP server disabled: %s (another FoxEditor may own port %u; use "
                    "--mcp-port <port> or --no-mcp)", error.c_str(), port);
        m_server.reset();
        return false;
    }
    m_port = m_server->Port();
    SE_LOG_INFO("MCP server listening on http://127.0.0.1:%u/mcp", m_port);
    return true;
}

void McpTools::Stop()
{
    std::deque<CallPtr> queued;
    {
        std::lock_guard lock(m_mutex);
        m_stopped = true;
        queued.swap(m_queue);
    }
    const ToolResult shutdown = ErrorResult("FoxEditor is shutting down");
    for (const CallPtr& call : queued)
        Complete(call, shutdown);
    for (const auto& waiter : m_waiters)
        Complete(waiter->call, shutdown);
    m_waiters.clear();
    for (const auto& capture : m_captures)
        Complete(capture->call, shutdown);
    m_captures.clear();
    if (m_playInput)
    {
        const CallPtr call = m_playInput->call;
        EndPlayInput();
        Complete(call, shutdown);
    }
    if (m_server)
    {
        m_server->Stop();
        m_server.reset();
    }
}

ToolResult McpTools::Call(const std::string& name, const json& arguments)
{
    auto call = std::make_shared<PendingCall>();
    call->name = name;
    call->arguments = arguments;
    std::future<ToolResult> result = call->promise.get_future();
    {
        std::lock_guard lock(m_mutex);
        if (m_stopped)
            return ErrorResult("FoxEditor is shutting down");
        m_queue.push_back(call);
    }
    if (result.wait_for(k_callTimeout) != std::future_status::ready)
        return ErrorResult("FoxEditor did not finish " + name + " within 600 s");
    return result.get();
}

void McpTools::Complete(const CallPtr& call, ToolResult result)
{
    if (call->done)
        return;
    call->done = true;
    call->promise.set_value(std::move(result));
}

void McpTools::BeginFrame()
{
    const uint64_t frame = m_access.clock->GetFrameCount();
    if (m_playInput)
        StepPlayInput();

    for (size_t i = 0; i < m_waiters.size();)
    {
        if (std::optional<ToolResult> result = m_waiters[i]->poll())
        {
            Complete(m_waiters[i]->call, std::move(*result));
            m_waiters.erase(m_waiters.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
            ++i;
    }

    for (size_t i = 0; i < m_captures.size();)
    {
        if (frame - m_captures[i]->queuedFrame > k_captureFrameLimit)
        {
            Complete(m_captures[i]->call, ErrorResult(k_noViewportImage));
            m_captures.erase(m_captures.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
            ++i;
    }

    std::deque<CallPtr> calls;
    {
        std::lock_guard lock(m_mutex);
        calls.swap(m_queue);
    }
    for (const CallPtr& call : calls)
    {
        Outcome outcome = Run(call);
        if (outcome.result)
            Complete(call, std::move(*outcome.result));
        else if (outcome.poll)
            m_waiters.push_back(std::make_unique<Waiter>(Waiter{ call, std::move(outcome.poll) }));
    }
}

bool McpTools::AgentHasInputFocus() const
{
    return m_playInput != nullptr || m_agentFocusFrame == m_access.clock->GetFrameCount();
}

void McpTools::OnViewportRendered()
{
    ServeCaptures(true);
}

void McpTools::OnFrameRendered()
{
    ServeCaptures(false);
}

void McpTools::ServeCaptures(bool viewportPhase)
{
    for (size_t i = 0; i < m_captures.size();)
    {
        Capture& capture = *m_captures[i];
        if ((capture.target == "viewport") != viewportPhase)
        {
            ++i;
            continue;
        }
        SE::Renderer& renderer = *m_access.renderer;
        ID3D11Texture2D* texture = viewportPhase ? m_access.viewportRT->GetTexture()
                                                 : renderer.GetBackBuffer();
        RECT crop = {};
        const bool cropped = capture.target == "viewport_ui";
        if (cropped)
        {
            const ImVec2 origin = ImGui::GetMainViewport()->Pos;
            crop.left   = static_cast<LONG>(std::lround(m_access.imageMin->x - origin.x));
            crop.top    = static_cast<LONG>(std::lround(m_access.imageMin->y - origin.y));
            crop.right  = static_cast<LONG>(std::lround(m_access.imageMax->x - origin.x));
            crop.bottom = static_cast<LONG>(std::lround(m_access.imageMax->y - origin.y));
        }
        std::vector<uint8_t> png;
        uint32_t width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
        std::string error;
        if (!texture)
            Complete(capture.call, ErrorResult(k_noViewportImage));
        else if (!CaptureTexturePng(renderer.GetDevice(), renderer.GetContext(), texture,
                                    cropped ? &crop : nullptr, capture.maxWidth, png, width,
                                    height, sourceWidth, sourceHeight, error))
            Complete(capture.call, ErrorResult("screenshot failed: " + error));
        else
            Complete(capture.call,
                     Mcp::ImageResult(png, { { "target", capture.target },
                                             { "width", width },
                                             { "height", height },
                                             { "source_width", sourceWidth },
                                             { "source_height", sourceHeight },
                                             { "frame", m_access.clock->GetFrameCount() } }));
        m_captures.erase(m_captures.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

McpTools::Outcome McpTools::Run(const CallPtr& call)
{
    const std::string& name = call->name;
    const json& args = call->arguments;

    static const std::vector<Mcp::ToolSpec> specs = Specs();
    const auto spec = std::find_if(specs.begin(), specs.end(),
                                   [&](const Mcp::ToolSpec& s) { return s.name == name; });
    if (spec == specs.end())
        return { ErrorResult("Unknown tool: " + name), {}, false };
    const json& properties = spec->inputSchema["properties"];
    for (const auto& item : args.items())
        if (!properties.contains(item.key()))
            return { ErrorResult("unknown argument '" + item.key() + "' for " + name), {}, false };
    if (spec->inputSchema.contains("required"))
        for (const json& key : spec->inputSchema["required"])
            if (!Arg(args, key.get<std::string>().c_str()))
                return { ErrorResult("missing argument '" + key.get<std::string>() + "'"), {}, false };

    if (name == "editor_status") return EditorStatus();
    if (name == "list_entities") return ListEntities(args);
    if (name == "get_entity") return GetEntity(args);
    if (name == "get_scene_json") return GetSceneJson();
    if (name == "get_logs") return GetLogs(args);
    if (name == "screenshot") return Screenshot(call, args);
    if (name == "select") return Select(args);
    if (name == "set_transform") return SetTransform(args);
    if (name == "rename_entity") return RenameEntity(args);
    if (name == "add_entity") return AddEntity(args);
    if (name == "duplicate_entity") return DuplicateEntity(args);
    if (name == "delete_entity") return DeleteEntity(args);
    if (name == "undo") return UndoRedo(false);
    if (name == "redo") return UndoRedo(true);
    if (name == "save_scene") return SaveScene(args);
    if (name == "open_scene") return OpenScene(args);
    if (name == "apply_scene_json") return ApplySceneJson(args);
    if (name == "set_camera") return SetCamera(args);
    if (name == "frame_entity") return FrameEntity(args);
    if (name == "set_view_options") return SetViewOptions(args);
    if (name == "play") return Play(args);
    if (name == "eval_lua") return EvalLua(args);
    if (name == "play_input") return PlayInputTool(call, args);
    return WaitFrames(args);
}

// --- Helpers -------------------------------------------------------------

McpTools::Outcome McpTools::Ok(const json& value) { return { JsonResult(value), {}, false }; }
McpTools::Outcome McpTools::Fail(const std::string& message) { return { ErrorResult(message), {}, false }; }

bool McpTools::Playing() const
{
    return std::string(m_access.playState()) != "edit";
}

std::optional<std::string> McpTools::EditGuard() const
{
    if (Playing())
        return std::string("editing is disabled while playing");
    if (m_access.scene->IsLoading() || !m_access.scene->HasScene())
        return std::string("the scene is loading");
    return std::nullopt;
}

int McpTools::Count(Kind kind) const
{
    const SE::SceneRuntime& scene = *m_access.scene;
    switch (kind)
    {
    case Kind::World:
    case Kind::Camera:
    case Kind::Sun:
    case Kind::SpotLight:  return 1;
    case Kind::PointLight: return scene.lights.numLights;
    case Kind::Mesh:       return static_cast<int>(scene.loadedMeshes.size());
    case Kind::Object:     return static_cast<int>(scene.sceneObjects.size());
    case Kind::Emitter:    return static_cast<int>(scene.particleSystems.size());
    case Kind::Empty:      return static_cast<int>(scene.emptyEntities.size());
    case Kind::Actor:      return static_cast<int>(scene.actors.size());
    default:               return 0;
    }
}

uint64_t McpTools::EntityId(const Selection& sel) const
{
    const SE::SceneRuntime& scene = *m_access.scene;
    const size_t i = static_cast<size_t>(sel.index);
    switch (sel.kind)
    {
    case Kind::Camera:     return scene.currentDesc.camera.sourceEntityID;
    case Kind::Sun:        return scene.currentDesc.sun.sourceEntityID;
    case Kind::PointLight: return i < scene.pointLightDescs.size() ? scene.pointLightDescs[i].sourceEntityID : 0;
    case Kind::Mesh:       return i < scene.loadedMeshes.size() ? scene.loadedMeshes[i].def.sourceEntityID : 0;
    case Kind::Object:     return i < scene.sceneObjects.size() ? scene.sceneObjects[i].def.sourceEntityID : 0;
    case Kind::Emitter:    return i < scene.particleEmitterDescs.size() ? scene.particleEmitterDescs[i].sourceEntityID : 0;
    case Kind::Empty:      return i < scene.emptyEntities.size() ? scene.emptyEntities[i].def.sourceEntityID : 0;
    case Kind::Actor:      return i < scene.actors.size() ? scene.actors[i].def.sourceEntityID : 0;
    default:               return 0;
    }
}

// The name SceneSaver writes for the entity (same fallbacks).
std::string McpTools::DisplayName(const Selection& sel) const
{
    SE::SceneRuntime& scene = *m_access.scene;
    switch (sel.kind)
    {
    case Kind::World:     return "World";
    case Kind::Camera:    return scene.currentDesc.camera.name.empty() ? "Camera" : scene.currentDesc.camera.name;
    case Kind::Sun:       return scene.currentDesc.sun.name.empty() ? "Sun" : scene.currentDesc.sun.name;
    case Kind::SpotLight: return "Spot Light";
    default: break;
    }
    const std::string* name = EntityName(scene, sel.kind, sel.index);
    if (name && !name->empty())
        return *name;
    const std::string n = std::to_string(sel.index + 1);
    switch (sel.kind)
    {
    case Kind::PointLight: return "Point Light " + n;
    case Kind::Object:     return "Object " + n;
    case Kind::Emitter:    return "Particle Emitter " + n;
    case Kind::Empty:      return "Empty";
    case Kind::Actor:      return "Actor";
    default:               return name ? *name : std::string();
    }
}

json McpTools::SelectionRef(const Selection& sel) const
{
    if (sel.kind == Kind::None)
        return nullptr;
    return { { "kind", KindName(sel.kind) }, { "index", sel.index },
             { "id", IdJson(EntityId(sel)) }, { "name", DisplayName(sel) } };
}

json McpTools::Describe(const Selection& sel) const
{
    SE::SceneRuntime& scene = *m_access.scene;
    json d = SelectionRef(sel);
    TransformSnapshot t;
    if (CaptureTransform(scene, sel, t))
    {
        d["position"] = Vec(t.position);
        if (HasFullTransform(sel.kind))
        {
            d["rotation"] = Vec(t.euler);
            d["scale"] = Vec(t.scale);
        }
    }
    const size_t i = static_cast<size_t>(sel.index);
    switch (sel.kind)
    {
    case Kind::Mesh:
    {
        const auto& m = scene.loadedMeshes[i];
        d["path"] = m.def.path;
        d["visible"] = m.visible;
        d["collider_only"] = !m.def.visible;
        d["collision"] = m.def.collision;
        break;
    }
    case Kind::Object:
        d["type"] = scene.sceneObjects[i].def.type == SE::SceneDescriptor::SceneObject::Type::Plane
            ? "plane" : "sphere";
        break;
    case Kind::SpotLight:
        d["enabled"] = scene.spotLight.enabled;
        break;
    case Kind::Sun:
        d["elevation"] = scene.lights.elevDeg;
        d["azimuth"] = scene.lights.azimDeg;
        break;
    case Kind::Actor:
        d["class"] = scene.actors[i].def.actorClass;
        d["spawned"] = scene.actors[i].spawned;
        break;
    default:
        break;
    }
    return d;
}

bool McpTools::ResolveTarget(const json& target, Selection& out, std::string& error) const
{
    if (!m_access.scene->HasScene())
    {
        error = "the scene is loading";
        return false;
    }
    if (!target.is_object())
    {
        error = "target needs exactly one of kind/index, id or name";
        return false;
    }
    const bool byKind = target.contains("kind") || target.contains("index");
    const bool byId = target.contains("id");
    const bool byName = target.contains("name");
    if (int(byKind) + int(byId) + int(byName) != 1)
    {
        error = "target needs exactly one of kind/index, id or name";
        return false;
    }

    if (byKind)
    {
        Kind kind = Kind::None;
        if (!target.contains("kind") || !target["kind"].is_string() ||
            !ParseKind(target["kind"].get<std::string>(), kind))
        {
            error = "target kind must be one of world, camera, sun, spot_light, point_light, mesh, "
                    "object, emitter, empty, actor";
            return false;
        }
        int index = 0;
        if (target.contains("index"))
        {
            if (!target["index"].is_number_integer())
            {
                error = "target index must be an integer";
                return false;
            }
            index = target["index"].get<int>();
        }
        else if (!IsSingleton(kind))
        {
            error = std::string(KindName(kind)) + " target needs an index";
            return false;
        }
        const int count = Count(kind);
        if (index < 0 || index >= count)
        {
            error = std::string(KindName(kind)) + " index " + std::to_string(index) +
                    " out of range (" + std::to_string(count) + " entities)";
            return false;
        }
        out.Set(kind, index);
        return true;
    }

    if (byId)
    {
        const std::string text = target["id"].is_string() ? target["id"].get<std::string>() : "";
        const bool hex = !text.empty() && text.size() <= 16 &&
            std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        if (!hex)
        {
            error = "target id must be 1-16 hex digits";
            return false;
        }
        const uint64_t id = std::strtoull(text.c_str(), nullptr, 16);
        for (const KindEntry& entry : k_kinds)
            for (int i = 0; id != 0 && i < Count(entry.kind); ++i)
                if (EntityId({ entry.kind, i }) == id)
                {
                    out.Set(entry.kind, i);
                    return true;
                }
        error = "no entity with id " + text;
        return false;
    }

    const std::string name = target["name"].is_string() ? target["name"].get<std::string>() : "";
    std::vector<Selection> matches;
    for (const KindEntry& entry : k_kinds)
    {
        if (entry.kind == Kind::World)
            continue;
        for (int i = 0; i < Count(entry.kind); ++i)
            if (DisplayName({ entry.kind, i }) == name)
                matches.push_back({ entry.kind, i });
    }
    if (matches.empty())
    {
        error = "no entity named '" + name + "'";
        return false;
    }
    if (matches.size() > 1)
    {
        std::vector<std::string> refs;
        for (const Selection& m : matches)
            refs.push_back(std::string(KindName(m.kind)) + " " + std::to_string(m.index));
        error = "name '" + name + "' is ambiguous: " + JoinList(refs);
        return false;
    }
    out = matches[0];
    return true;
}

json McpTools::CameraJson() const
{
    const SE::CameraComponent* cam = m_access.scene->camera;
    if (!cam)
        return nullptr;
    return { { "eye", Vec(cam->eye) },
             { "target", Vec(cam->target) },
             { "up", Vec(cam->up) },
             { "fov_deg", cam->fovDeg },
             { "near", cam->nearZ },
             { "far", cam->farZ },
             { "orthographic", cam->orthographic },
             { "ortho_height", cam->orthoHeight },
             { "view_mode", ViewModeLabel(m_access.camera->GetViewMode()) } };
}

json McpTools::ViewOptionsJson() const
{
    const ViewportOptions& o = *m_access.viewOptions;
    const char* shading = "lit";
    switch (m_access.scene->pipeline.viewShading)
    {
    case SE::ForwardPipeline::ViewShading::Unlit:     shading = "unlit"; break;
    case SE::ForwardPipeline::ViewShading::Wireframe: shading = "wireframe"; break;
    default: break;
    }
    const char* gizmo = "select";
    switch (*m_access.gizmoOp)
    {
    case GizmoOp::Translate: gizmo = "translate"; break;
    case GizmoOp::Rotate:    gizmo = "rotate"; break;
    case GizmoOp::Scale:     gizmo = "scale"; break;
    default: break;
    }
    return { { "realtime", o.realtime },       { "game_view", o.gameView },
             { "show_lights", o.showLights },  { "show_grid", o.showGrid },
             { "show_axes", o.showAxes },      { "show_stats", o.showStats },
             { "immersive", o.immersive },     { "show_colliders", m_access.scene->showColliders },
             { "shading", shading },           { "gizmo", gizmo } };
}

McpTools::Poll McpTools::SceneLoadWaiter(std::function<ToolResult()> onReady)
{
    const uint64_t readyBefore = m_access.sceneReadyCount();
    const auto deadline = std::chrono::steady_clock::now() + k_loadTimeout;
    auto sawLoading = std::make_shared<bool>(false);
    SE::SceneRuntime* scene = m_access.scene;
    auto readyCount = m_access.sceneReadyCount;
    return [=]() -> std::optional<ToolResult> {
        if (readyCount() != readyBefore)
            return onReady();
        if (scene->IsLoading())
            *sawLoading = true;
        else if (*sawLoading && !scene->sceneLoadError.empty())
            return ErrorResult("scene load failed: " + scene->sceneLoadError);
        if (std::chrono::steady_clock::now() > deadline)
            return ErrorResult("the scene did not finish loading within 300 s");
        return std::nullopt;
    };
}

// --- Read tools --------------------------------------------------------------

McpTools::Outcome McpTools::EditorStatus()
{
    SE::SceneRuntime& scene = *m_access.scene;
    const SE::Clock& clock = *m_access.clock;
    const SE::LevelLoadStatus load = scene.GetLoadStatus();
    const char* loadState = "idle";
    switch (load.state)
    {
    case SE::LevelLoadState::Preparing: loadState = "preparing"; break;
    case SE::LevelLoadState::Uploading: loadState = "uploading"; break;
    case SE::LevelLoadState::Complete:  loadState = "complete"; break;
    case SE::LevelLoadState::Failed:    loadState = "failed"; break;
    case SE::LevelLoadState::Cancelled: loadState = "cancelled"; break;
    default: break;
    }
    const UndoStack& undo = *m_access.undo;
    json diagnostics = json::array();
    for (size_t i = 0; i < scene.levelDiagnostics.size() && i < 50; ++i)
    {
        const SE::LevelDiagnostic& d = scene.levelDiagnostics[i];
        diagnostics.push_back({ { "severity", d.severity == SE::LevelDiagnosticSeverity::Error ? "error" : "warning" },
                                { "path", d.path }, { "message", d.message } });
    }
    const bool hasScene = scene.HasScene();
    const SE::ScriptSystem& scripts = *m_access.scripts;
    json status = {
        { "project", m_projectName },
        { "scene", scene.currentScenePath },
        { "dirty", m_access.isDirty() },
        { "play_state", m_access.playState() },
        { "frame", clock.GetFrameCount() },
        { "loading", { { "state", loadState }, { "progress", load.progress },
                       { "current_asset", load.currentAsset }, { "failure", load.failureReason } } },
        { "scene_load_error", scene.sceneLoadError },
        { "selection", hasScene ? SelectionRef(*m_access.selection) : json(nullptr) },
        { "undo", { { "can_undo", undo.CanUndo() }, { "undo_label", undo.UndoLabel() },
                    { "can_redo", undo.CanRedo() }, { "redo_label", undo.RedoLabel() } } },
        { "camera", CameraJson() },
        { "viewport", { { "width", m_access.viewportRT->GetWidth() },
                        { "height", m_access.viewportRT->GetHeight() } } },
        { "view_options", ViewOptionsJson() },
        { "stats", { { "fps", clock.GetFPS() }, { "frame_ms", clock.GetDeltaTime() * 1000.0f },
                     { "draw_calls", scene.pipeline.GetLastDrawCalls() },
                     { "culled", scene.pipeline.GetLastCulledCount() },
                     { "cached_meshes", m_access.assets->CachedMeshCount() },
                     { "cached_textures", m_access.assets->CachedTextureCount() } } },
        { "scripts", { { "running", scripts.IsRunning() },
                       { "error_count", scripts.IsRunning() ? scripts.ErrorCount() : 0u } } },
        { "counts", { { "mesh", scene.loadedMeshes.size() }, { "point_light", scene.lights.numLights },
                      { "object", scene.sceneObjects.size() }, { "emitter", scene.particleSystems.size() },
                      { "empty", scene.emptyEntities.size() }, { "actor", scene.actors.size() },
                      { "vehicle", scene.vehicles.size() } } },
        { "diagnostics", diagnostics },
        { "mcp_port", m_port },
    };
    return Ok(status);
}

McpTools::Outcome McpTools::ListEntities(const json& args)
{
    std::string error;
    std::optional<std::string> kindName, contains;
    if (!ReadString(args, "kind", kindName, error) || !ReadString(args, "name_contains", contains, error))
        return Fail(error);
    if (!m_access.scene->HasScene())
        return Fail("the scene is loading");
    Kind only = Kind::None;
    if (kindName && !ParseKind(*kindName, only))
        return Fail("unknown kind '" + *kindName + "'");
    const std::string needle = contains ? Lower(*contains) : std::string();
    json entities = json::array();
    for (const KindEntry& entry : k_kinds)
    {
        if (only == Kind::None ? entry.kind == Kind::World : entry.kind != only)
            continue;
        for (int i = 0; i < Count(entry.kind); ++i)
        {
            const Selection sel{ entry.kind, i };
            if (!needle.empty() && Lower(DisplayName(sel)).find(needle) == std::string::npos)
                continue;
            entities.push_back(Describe(sel));
        }
    }
    return Ok({ { "entities", entities } });
}

McpTools::Outcome McpTools::GetEntity(const json& args)
{
    Selection sel;
    std::string error;
    if (!ResolveTarget(args["target"], sel, error))
        return Fail(error);
    SE::SceneRuntime& scene = *m_access.scene;
    json d = Describe(sel);

    // WriteEntities order: meshes, camera, sun, point lights, objects,
    // emitters, empties, placed actors.
    const SE::SceneDescriptor& desc = scene.currentDesc;
    size_t index = 0;
    bool inLevel = true;
    scene.SyncDescriptorFromRuntime();
    const size_t m = desc.meshes.size(), p = desc.pointLights.size(), o = desc.objects.size(),
                 e = desc.particles.size(), y = desc.empties.size();
    const size_t i = static_cast<size_t>(sel.index);
    switch (sel.kind)
    {
    case Kind::Mesh:       index = i; break;
    case Kind::Camera:     index = m; break;
    case Kind::Sun:        index = m + 1; break;
    case Kind::PointLight: index = m + 2 + i; break;
    case Kind::Object:     index = m + 2 + p + i; break;
    case Kind::Emitter:    index = m + 2 + p + o + i; break;
    case Kind::Empty:      index = m + 2 + p + o + e + i; break;
    case Kind::Actor:
    {
        if (scene.actors[i].spawned)
        {
            inLevel = false;
            break;
        }
        size_t placed = 0;
        for (size_t a = 0; a < i; ++a)
            placed += scene.actors[a].spawned ? 0 : 1;
        index = m + 2 + p + o + e + y + placed;
        break;
    }
    default:
        inLevel = false;
        break;
    }
    if (inLevel)
    {
        const json level = json::parse(SE::SceneSaver::ToJson(desc), nullptr, false);
        const json* entity = nullptr;
        if (level.is_object() && level.contains("entities") && index < level["entities"].size())
            entity = &level["entities"][index];
        if (entity && entity->value("name", std::string()) == d["name"].get<std::string>())
            d["level"] = *entity;
        else
            d["level_error"] = "level JSON order did not match the runtime";
    }
    return Ok(d);
}

McpTools::Outcome McpTools::GetSceneJson()
{
    SE::SceneRuntime& scene = *m_access.scene;
    if (!scene.HasScene())
        return Fail("the scene is loading");
    scene.SyncDescriptorFromRuntime();
    return Ok({ { "scene", scene.currentScenePath },
                { "source", Playing() ? "play" : "edit" },
                { "json", json::parse(SE::SceneSaver::ToJson(scene.currentDesc), nullptr, false) } });
}

McpTools::Outcome McpTools::GetLogs(const json& args)
{
    std::string error;
    std::optional<int64_t> since, limit;
    std::optional<std::string> minLevel, contains;
    if (!ReadInt(args, "since", 0, INT64_MAX, since, error) ||
        !ReadInt(args, "limit", 1, 2000, limit, error) ||
        !ReadString(args, "min_level", minLevel, error) ||
        !ReadString(args, "contains", contains, error))
        return Fail(error);
    int minRank = 1;
    if (minLevel)
    {
        static const char* k_levels[] = { "debug", "info", "warning", "error" };
        const auto it = std::find_if(std::begin(k_levels), std::end(k_levels),
                                     [&](const char* l) { return *minLevel == l; });
        if (it == std::end(k_levels))
            return Fail("min_level must be debug, info, warning or error");
        minRank = static_cast<int>(it - std::begin(k_levels));
    }
    auto rank = [](SE::LogLevel level) {
        switch (level)
        {
        case SE::LogLevel::Debug:   return 0;
        case SE::LogLevel::Info:    return 1;
        case SE::LogLevel::Warning: return 2;
        default:                    return 3;
        }
    };
    static const char* k_names[] = { "debug", "info", "warning", "error" };
    const std::string needle = contains ? Lower(*contains) : std::string();
    const size_t maxLines = static_cast<size_t>(limit.value_or(100));

    std::vector<Console::LogEntry> matches;
    for (Console::LogEntry& entry : m_access.console->Snapshot(static_cast<uint64_t>(since.value_or(0))))
    {
        if (rank(entry.level) < minRank)
            continue;
        if (!needle.empty() && Lower(entry.text).find(needle) == std::string::npos)
            continue;
        matches.push_back(std::move(entry));
    }
    const size_t first = matches.size() > maxLines ? matches.size() - maxLines : 0;
    json lines = json::array();
    for (size_t i = first; i < matches.size(); ++i)
        lines.push_back({ { "seq", matches[i].seq }, { "level", k_names[rank(matches[i].level)] },
                          { "text", matches[i].text } });
    return Ok({ { "lines", lines }, { "next_since", m_access.console->NextSeq() } });
}

McpTools::Outcome McpTools::Screenshot(const CallPtr& call, const json& args)
{
    std::string error;
    std::optional<std::string> target;
    std::optional<int64_t> maxWidth;
    if (!ReadString(args, "target", target, error) ||
        !ReadInt(args, "max_width", 0, 16384, maxWidth, error))
        return Fail(error);
    const std::string which = target.value_or("viewport");
    if (which != "viewport" && which != "viewport_ui" && which != "window")
        return Fail("target must be viewport, viewport_ui or window");
    const SE::SceneRuntime& scene = *m_access.scene;
    if (which != "window" &&
        !(scene.HasScene() && !scene.IsLoading() && m_access.viewportRT->GetRTV()))
        return Fail(k_noViewportImage);
    auto capture = std::make_unique<Capture>();
    capture->call = call;
    capture->target = which;
    capture->maxWidth = static_cast<uint32_t>(maxWidth.value_or(1280));
    capture->queuedFrame = m_access.clock->GetFrameCount();
    m_captures.push_back(std::move(capture));
    return { std::nullopt, {}, true };
}

// --- Edit tools ----------------------------------------------------------------

McpTools::Outcome McpTools::Select(const json& args)
{
    std::string error;
    std::optional<bool> clear;
    if (!ReadBool(args, "clear", clear, error))
        return Fail(error);
    const bool hasTarget = Arg(args, "target") != nullptr;
    if (clear.value_or(false) == hasTarget)
        return Fail("pass either target or clear: true");
    if (clear.value_or(false))
    {
        m_access.selection->Clear();
        return Ok({ { "selection", nullptr } });
    }
    Selection sel;
    if (!ResolveTarget(args["target"], sel, error))
        return Fail(error);
    *m_access.selection = sel;
    return Ok({ { "selection", SelectionRef(sel) } });
}

McpTools::Outcome McpTools::SetTransform(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    Selection sel;
    std::string error;
    std::optional<DirectX::XMFLOAT3> position, rotation, scale;
    if (!ResolveTarget(args["target"], sel, error) ||
        !ReadVec3(args, "position", position, error) ||
        !ReadVec3(args, "rotation", rotation, error) ||
        !ReadVec3(args, "scale", scale, error))
        return Fail(error);
    if (!position && !rotation && !scale)
        return Fail("pass at least one of position, rotation, scale");
    SE::SceneRuntime& scene = *m_access.scene;
    TransformSnapshot before;
    if (!CaptureTransform(scene, sel, before))
        return Fail(std::string(KindName(sel.kind)) + " has no transform");
    if ((rotation || scale) && !HasFullTransform(sel.kind))
        return Fail(std::string(KindName(sel.kind)) + " supports position only");
    TransformSnapshot after = before;
    if (position) after.position = *position;
    if (rotation) after.euler = *rotation;
    if (scale) after.scale = *scale;
    ApplyTransform(scene, sel, after);
    PushTransformUndo(scene, *m_access.undo, sel, before, after);
    return Ok(Describe(sel));
}

McpTools::Outcome McpTools::RenameEntity(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    Selection sel;
    std::string error;
    std::optional<std::string> name;
    if (!ResolveTarget(args["target"], sel, error) || !ReadString(args, "name", name, error))
        return Fail(error);
    if (!name || name->empty())
        return Fail("name must not be empty");
    if (!Editor::RenameEntity(*m_access.scene, *m_access.undo, sel, *name))
        return Fail(std::string(KindName(sel.kind)) + " has no editable name");
    return Ok(Describe(sel));
}

McpTools::Outcome McpTools::AddEntity(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    std::string error;
    std::optional<std::string> type, actorClass;
    std::optional<DirectX::XMFLOAT3> position;
    if (!ReadString(args, "type", type, error) || !ReadString(args, "actor_class", actorClass, error) ||
        !ReadVec3(args, "position", position, error))
        return Fail(error);
    SE::SceneRuntime& scene = *m_access.scene;
    const DirectX::XMFLOAT3 at = position ? *position : m_access.spawnPosition();
    using ObjType = SE::SceneDescriptor::SceneObject::Type;
    Selection spawned;
    if (*type == "point_light")
    {
        if (scene.lights.numLights >= 8)
            return Fail("at most 8 point lights");
        if (!scene.AddPointLight(at))
            return Fail("could not add a point light");
        spawned.Set(Kind::PointLight, scene.lights.numLights - 1);
    }
    else if (*type == "particle_emitter")
    {
        if (!scene.AddParticleEmitter(at))
            return Fail("could not add a particle emitter");
        spawned.Set(Kind::Emitter, static_cast<int>(scene.particleSystems.size()) - 1);
    }
    else if (*type == "sphere" || *type == "plane")
    {
        if (!scene.AddSceneObject(*type == "plane" ? ObjType::Plane : ObjType::Sphere, at))
            return Fail("could not add a " + *type);
        spawned.Set(Kind::Object, static_cast<int>(scene.sceneObjects.size()) - 1);
    }
    else if (*type == "empty")
    {
        if (!scene.AddEmpty(at))
            return Fail("could not add an empty");
        spawned.Set(Kind::Empty, static_cast<int>(scene.emptyEntities.size()) - 1);
    }
    else if (*type == "actor")
    {
        if (!actorClass)
            return Fail("actor_class is required for type actor");
        const std::string& path = *actorClass;
        const bool shaped = path.rfind("Assets/Scripts/", 0) == 0 && path.size() > 4 &&
                            path.compare(path.size() - 4, 4, ".lua") == 0;
        if (!shaped || !std::filesystem::is_regular_file(path))
            return Fail("actor_class must name an existing Assets/Scripts/....lua file");
        const int index = scene.AddActor(path, at);
        if (index < 0)
            return Fail("could not place actor " + path + " (see get_logs)");
        spawned.Set(Kind::Actor, index);
    }
    else
        return Fail("type must be point_light, particle_emitter, sphere, plane, empty or actor");

    *m_access.selection = spawned;
    PushSpawnUndo(scene, *m_access.undo, *m_access.selection, spawned, "Add");
    return Ok(Describe(spawned));
}

McpTools::Outcome McpTools::DuplicateEntity(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    Selection sel;
    std::string error;
    if (!ResolveTarget(args["target"], sel, error))
        return Fail(error);
    *m_access.selection = sel;
    if (!DuplicateSelection(*m_access.scene, *m_access.selection, *m_access.undo))
        return Fail(std::string(KindName(sel.kind)) + " cannot be duplicated");
    return Ok(Describe(*m_access.selection));
}

McpTools::Outcome McpTools::DeleteEntity(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    Selection sel;
    std::string error;
    if (!ResolveTarget(args["target"], sel, error))
        return Fail(error);
    const json deleted = SelectionRef(sel);
    *m_access.selection = sel;
    if (!DeleteSelection(*m_access.scene, *m_access.selection, *m_access.undo))
        return Fail(std::string(KindName(sel.kind)) + " cannot be deleted");
    return Ok({ { "deleted", deleted } });
}

McpTools::Outcome McpTools::UndoRedo(bool redo)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    UndoStack& undo = *m_access.undo;
    if (redo ? !undo.CanRedo() : !undo.CanUndo())
        return Fail(redo ? "nothing to redo" : "nothing to undo");
    const std::string label = redo ? undo.RedoLabel() : undo.UndoLabel();
    redo ? undo.Redo() : undo.Undo();
    return Ok({ { "done", label } });
}

McpTools::Outcome McpTools::SaveScene(const json& args)
{
    std::string error;
    std::optional<std::string> path;
    if (!ReadString(args, "path", path, error))
        return Fail(error);
    SE::SceneRuntime& scene = *m_access.scene;
    const char* failed = "save failed (the level did not validate); see get_logs";
    if (!path)
    {
        if (!Playing() && (scene.IsLoading() || !scene.HasScene()))
            return Fail("the scene is loading");
        if (!m_access.save())
            return Fail(failed);
        return Ok({ { "saved", scene.currentScenePath } });
    }
    if (auto guard = EditGuard())
        return Fail(*guard);
    std::string name = *path;
    if (name.rfind("Assets/Scenes/", 0) == 0)
        name = name.substr(14);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
        name.resize(name.size() - 5);
    if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
        return Fail("invalid scene name");
    const std::string target = "Assets/Scenes/" + name + ".json";
    if (!m_access.saveAs(target))
        return Fail(failed);
    return Ok({ { "saved", target } });
}

McpTools::Outcome McpTools::OpenScene(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    std::string error;
    std::optional<std::string> path;
    std::optional<bool> discard, wait;
    if (!ReadString(args, "path", path, error) || !ReadBool(args, "discard_changes", discard, error) ||
        !ReadBool(args, "wait", wait, error))
        return Fail(error);
    std::string scenePath = *path;
    if (scenePath.find_first_of("\\/") == std::string::npos)
        scenePath = "Assets/Scenes/" + scenePath;
    if (!std::filesystem::is_regular_file(scenePath))
        return Fail("no scene " + scenePath + "; available: " +
                    JoinList(SE::SceneLoader::ScanSceneDirectory("Assets/Scenes")));
    if (m_access.isDirty() && !discard.value_or(false))
        return Fail("the scene has unsaved changes; save_scene first or pass discard_changes: true");
    SE::SceneRuntime* scene = m_access.scene;
    Poll waiter = SceneLoadWaiter([scene]() { return JsonResult({ { "scene", scene->currentScenePath } }); });
    scene->sceneLoadError.clear();
    scene->RequestSceneLoad(scenePath);
    if (!wait.value_or(true))
        return Ok({ { "requested", scenePath } });
    return { std::nullopt, std::move(waiter), false };
}

McpTools::Outcome McpTools::ApplySceneJson(const json& args)
{
    if (auto guard = EditGuard())
        return Fail(*guard);
    std::string error;
    std::optional<bool> wait;
    if (!ReadBool(args, "wait", wait, error))
        return Fail(error);
    const json& value = args["json"];
    json root;
    if (value.is_string())
    {
        root = json::parse(value.get<std::string>(), nullptr, false);
        if (root.is_discarded())
            return Fail("json is not valid JSON");
    }
    else if (value.is_object())
        root = value;
    else
        return Fail("json must be a FoxLevel object or its text");

    SE::SceneRuntime* scene = m_access.scene;
    const SE::LevelAssetLoadResult parsed = SE::LevelAssetLoader::Parse(root, scene->currentScenePath);
    if (!parsed.Succeeded())
    {
        std::string message = "the level did not validate:";
        for (const SE::LevelDiagnostic& d : parsed.diagnostics)
            if (d.severity == SE::LevelDiagnosticSeverity::Error)
                message += "\n" + d.path + ": " + d.message;
        return Fail(message);
    }
    auto isDirty = m_access.isDirty;
    Poll waiter = SceneLoadWaiter([scene, isDirty]() {
        return JsonResult({ { "scene", scene->currentScenePath }, { "dirty", isDirty() } });
    });
    scene->sceneLoadError.clear();
    m_access.loadEditedScene(Mcp::DumpJson(root, 2) + "\n");
    if (!wait.value_or(true))
        return Ok({ { "requested", scene->currentScenePath } });
    return { std::nullopt, std::move(waiter), false };
}

McpTools::Outcome McpTools::SetCamera(const json& args)
{
    if (Playing())
        return Fail("the game owns the camera while playing");
    if (auto guard = EditGuard())
        return Fail(*guard);
    std::string error;
    std::optional<DirectX::XMFLOAT3> eye, target;
    std::optional<double> fov, orthoHeight;
    std::optional<std::string> viewMode;
    if (!ReadVec3(args, "eye", eye, error) || !ReadVec3(args, "target", target, error) ||
        !ReadNumber(args, "fov_deg", 5.0, 170.0, fov, error) ||
        !ReadNumber(args, "ortho_height", 1e-6, 1e7, orthoHeight, error) ||
        !ReadString(args, "view_mode", viewMode, error))
        return Fail(error);
    if (viewMode && (eye || target))
        return Fail("view_mode cannot be combined with eye or target");

    using VM = EditorCamera::ViewMode;
    static const std::pair<const char*, VM> k_modes[] = {
        { "perspective", VM::Perspective }, { "isometric", VM::Isometric }, { "top", VM::Top },
        { "bottom", VM::Bottom }, { "front", VM::Front }, { "back", VM::Back },
        { "left", VM::Left }, { "right", VM::Right },
    };
    SE::CameraComponent& cam = *m_access.scene->camera;
    if (viewMode)
    {
        const auto it = std::find_if(std::begin(k_modes), std::end(k_modes),
                                     [&](const auto& m) { return *viewMode == m.first; });
        if (it == std::end(k_modes))
            return Fail("unknown view_mode '" + *viewMode + "'");
        m_access.camera->SetViewMode(it->second, cam);
    }
    if (eye || target)
    {
        using namespace DirectX;
        const XMFLOAT3 newEye = eye ? *eye : cam.eye;
        XMFLOAT3 newTarget;
        if (target)
            newTarget = *target;
        else
            XMStoreFloat3(&newTarget, XMLoadFloat3(&newEye) +
                                          (XMLoadFloat3(&cam.target) - XMLoadFloat3(&cam.eye)));
        const XMVECTOR dir = XMLoadFloat3(&newTarget) - XMLoadFloat3(&newEye);
        const float length = XMVectorGetX(XMVector3Length(dir));
        if (length < 1e-4f)
            return Fail("eye and target must differ");
        cam.eye = newEye;
        cam.target = newTarget;
        // A view straight up or down needs another up vector.
        cam.up = std::fabs(XMVectorGetY(dir) / length) > 0.999f ? XMFLOAT3{ 0, 0, 1 } : XMFLOAT3{ 0, 1, 0 };
        cam.orthographic = false;
        m_access.camera->SyncFromCamera(cam);
    }
    if (fov)
        cam.fovDeg = static_cast<float>(*fov);
    if (orthoHeight)
        cam.orthoHeight = static_cast<float>(*orthoHeight);
    m_access.scene->ResetTemporalHistory();
    return Ok(CameraJson());
}

McpTools::Outcome McpTools::FrameEntity(const json& args)
{
    if (Playing())
        return Fail("the game owns the camera while playing");
    if (auto guard = EditGuard())
        return Fail(*guard);
    Selection sel;
    std::string error;
    if (!ResolveTarget(args["target"], sel, error))
        return Fail(error);
    DirectX::XMFLOAT3 center;
    float radius = 0.0f;
    if (!GetSelectionBounds(*m_access.scene, sel, center, radius))
        return Fail(std::string(KindName(sel.kind)) + " has no position to frame");
    const float aspect = static_cast<float>(m_access.viewportRT->GetWidth()) /
                         static_cast<float>((std::max)(m_access.viewportRT->GetHeight(), 1u));
    m_access.camera->FocusOn(center, radius, *m_access.scene->camera, aspect);
    m_access.scene->ResetTemporalHistory();
    return Ok(CameraJson());
}

McpTools::Outcome McpTools::SetViewOptions(const json& args)
{
    ViewportOptions& o = *m_access.viewOptions;
    std::string error;
    struct BoolOption { const char* key; bool* field; };
    const BoolOption options[] = {
        { "realtime", &o.realtime },     { "game_view", &o.gameView },
        { "show_lights", &o.showLights }, { "show_grid", &o.showGrid },
        { "show_axes", &o.showAxes },    { "show_stats", &o.showStats },
        { "immersive", &o.immersive },   { "show_colliders", &m_access.scene->showColliders },
    };
    std::optional<bool> values[std::size(options)];
    for (size_t i = 0; i < std::size(options); ++i)
        if (!ReadBool(args, options[i].key, values[i], error))
            return Fail(error);
    std::optional<std::string> shading, gizmo;
    if (!ReadString(args, "shading", shading, error) || !ReadString(args, "gizmo", gizmo, error))
        return Fail(error);

    using VS = SE::ForwardPipeline::ViewShading;
    VS shadingValue = m_access.scene->pipeline.viewShading;
    if (shading)
    {
        if (*shading == "lit") shadingValue = VS::Lit;
        else if (*shading == "unlit") shadingValue = VS::Unlit;
        else if (*shading == "wireframe") shadingValue = VS::Wireframe;
        else return Fail("shading must be lit, unlit or wireframe");
    }
    GizmoOp gizmoValue = *m_access.gizmoOp;
    if (gizmo)
    {
        if (*gizmo == "select") gizmoValue = GizmoOp::None;
        else if (*gizmo == "translate") gizmoValue = GizmoOp::Translate;
        else if (*gizmo == "rotate") gizmoValue = GizmoOp::Rotate;
        else if (*gizmo == "scale") gizmoValue = GizmoOp::Scale;
        else return Fail("gizmo must be select, translate, rotate or scale");
    }
    for (size_t i = 0; i < std::size(options); ++i)
        if (values[i])
            *options[i].field = *values[i];
    m_access.scene->pipeline.viewShading = shadingValue;
    *m_access.gizmoOp = gizmoValue;
    return Ok(ViewOptionsJson());
}

// --- Play tools -----------------------------------------------------------------

McpTools::Outcome McpTools::Play(const json& args)
{
    std::string error;
    std::optional<std::string> action;
    std::optional<bool> wait;
    if (!ReadString(args, "action", action, error) || !ReadBool(args, "wait", wait, error))
        return Fail(error);
    const std::string state = m_access.playState();
    SE::SceneRuntime* scene = m_access.scene;
    auto playState = m_access.playState;
    const auto deadline = std::chrono::steady_clock::now() + k_loadTimeout;
    Poll waiter;
    if (*action == "start")
    {
        if (state != "edit")
            return Fail("already playing");
        if (!m_access.canTogglePlay())
            return Fail("cannot start Play now (scene loading or a prompt is open)");
        waiter = [playState, deadline]() -> std::optional<ToolResult> {
            const std::string now = playState();
            if (now == "playing")
                return JsonResult({ { "play_state", now } });
            if (now == "edit")
                return ErrorResult("Play failed to start; see get_logs");
            if (std::chrono::steady_clock::now() > deadline)
                return ErrorResult("Play did not start within 300 s");
            return std::nullopt;
        };
    }
    else if (*action == "stop")
    {
        if (state == "edit")
            return Fail("not playing");
        if (!m_access.canTogglePlay())
            return Fail("cannot stop Play now");
        waiter = [playState, scene, deadline]() -> std::optional<ToolResult> {
            const std::string now = playState();
            if (now == "edit")
                return JsonResult({ { "play_state", now } });
            if (now == "stopping" && !scene->IsLoading() && !scene->sceneLoadError.empty())
                return ErrorResult("restoring the edit scene failed: " + scene->sceneLoadError);
            if (std::chrono::steady_clock::now() > deadline)
                return ErrorResult("Play did not stop within 300 s");
            return std::nullopt;
        };
    }
    else
        return Fail("action must be start or stop");
    m_access.requestTogglePlay();
    if (!wait.value_or(true))
        return Ok({ { "requested", *action } });
    return { std::nullopt, std::move(waiter), false };
}

McpTools::Outcome McpTools::EvalLua(const json& args)
{
    std::string error;
    std::optional<std::string> code;
    if (!ReadString(args, "code", code, error))
        return Fail(error);
    if (!m_access.scripts->IsRunning() || std::string(m_access.playState()) != "playing")
        return Fail("eval_lua needs a running Play session (play {action:\"start\"}); for "
                    "headless Lua use FoxProject eval");
    const SE::ScriptEvalResult result = m_access.scripts->Execute(*code);
    if (!result.ok)
        return Fail(result.text);
    return Ok({ { "result", result.text } });
}

McpTools::Outcome McpTools::PlayInputTool(const CallPtr& call, const json& args)
{
    if (std::string(m_access.playState()) != "playing")
        return Fail("play_input needs a running Play session (play {action:\"start\"})");
    if (m_playInput)
        return Fail("play_input is already running");
    std::string error;
    std::optional<int64_t> frames;
    if (!ReadInt(args, "frames", 1, 600, frames, error))
        return Fail(error);

    auto input = std::make_unique<PlayInput>();
    input->call = call;
    input->frames = static_cast<int>(frames.value_or(1));
    auto addKey = [&input](const SE::KeyCode& key, const std::string& name) {
        const int vk = key.code;
        if (std::find(input->vks.begin(), input->vks.end(), vk) == input->vks.end())
        {
            input->vks.push_back(vk);
            input->keyNames.push_back(name);
        }
    };
    auto readNames = [&](const char* key, std::vector<std::string>& out) {
        const json* v = Arg(args, key);
        if (!v) return true;
        if (!v->is_array() || !std::all_of(v->begin(), v->end(), [](const json& s) { return s.is_string(); }))
        {
            error = std::string("'") + key + "' must be an array of strings";
            return false;
        }
        for (const json& s : *v)
            out.push_back(s.get<std::string>());
        return true;
    };
    std::vector<std::string> keys, actions;
    if (!readNames("keys", keys) || !readNames("actions", actions))
        return Fail(error);

    for (const std::string& name : keys)
    {
        SE::KeyCode key;
        if (!SE::ParseKeyName(name, key))
            return Fail("unknown key '" + name + "'");
        if (key.device == SE::KeyCode::Device::Gamepad)
            return Fail("gamepad input injection is not supported");
        addKey(key, name);
    }
    if (!actions.empty())
    {
        const SE::ProjectSettings project = SE::Project::Load().project;
        for (const std::string& action : actions)
        {
            const auto it = std::find_if(project.inputActions.begin(), project.inputActions.end(),
                                         [&](const auto& a) { return a.first == action; });
            if (it == project.inputActions.end())
            {
                std::vector<std::string> declared;
                for (const auto& a : project.inputActions)
                    declared.push_back(a.first);
                return Fail("unknown action '" + action + "' (declared: " + JoinList(declared) + ")");
            }
            bool bound = false;
            for (const std::string& name : it->second)
            {
                SE::KeyCode key;
                if (SE::ParseKeyName(name, key) && key.device == SE::KeyCode::Device::Keyboard)
                {
                    addKey(key, name);
                    bound = true;
                    break;
                }
            }
            if (!bound)
                return Fail("action '" + action + "' has no keyboard or mouse binding");
        }
    }
    if (const json* delta = Arg(args, "mouse_delta"))
    {
        const bool ok = delta->is_array() && delta->size() == 2 &&
            std::all_of(delta->begin(), delta->end(), [](const json& n) {
                return n.is_number_integer() && std::llabs(n.get<int64_t>()) <= 10000;
            });
        if (!ok)
            return Fail("'mouse_delta' must be [dx, dy] integers within +-10000");
        input->dx = (*delta)[0].get<int32_t>();
        input->dy = (*delta)[1].get<int32_t>();
    }

    // Frame 1: press and first delta, seen by this frame's Simulate.
    SE::InputManager& im = *m_access.input;
    im.SetVirtualCapture(true);
    for (int vk : input->vks)
        im.InjectKey(vk, true);
    im.InjectMouseDelta(input->dx, input->dy);
    m_agentFocusFrame = m_access.clock->GetFrameCount();
    m_playInput = std::move(input);
    return { std::nullopt, {}, true };
}

void McpTools::StepPlayInput()
{
    PlayInput& in = *m_playInput;
    const CallPtr call = in.call;
    if (std::string(m_access.playState()) != "playing")
    {
        EndPlayInput();
        Complete(call, ErrorResult("the Play session ended during play_input"));
        return;
    }
    ++in.frame;
    m_agentFocusFrame = m_access.clock->GetFrameCount();
    if (in.frame <= in.frames)
    {
        m_access.input->InjectMouseDelta(in.dx, in.dy);
        return;
    }
    const json result = { { "frames", in.frames }, { "keys", in.keyNames },
                          { "play_state", m_access.playState() },
                          { "frame", m_access.clock->GetFrameCount() } };
    EndPlayInput();
    Complete(call, JsonResult(result));
}

void McpTools::EndPlayInput()
{
    if (!m_playInput)
        return;
    for (int vk : m_playInput->vks)
        m_access.input->InjectKey(vk, false);
    m_access.input->SetVirtualCapture(false);
    m_playInput.reset();
}

McpTools::Outcome McpTools::WaitFrames(const json& args)
{
    std::string error;
    std::optional<int64_t> frames;
    if (!ReadInt(args, "frames", 1, 600, frames, error))
        return Fail(error);
    auto remaining = std::make_shared<int64_t>(*frames);
    const SE::Clock* clock = m_access.clock;
    return { std::nullopt,
             [remaining, clock]() -> std::optional<ToolResult> {
                 if (--*remaining > 0)
                     return std::nullopt;
                 return JsonResult({ { "frame", clock->GetFrameCount() } });
             },
             false };
}

} // namespace Editor
