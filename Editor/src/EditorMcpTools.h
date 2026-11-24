#pragma once
#include <DirectXMath.h>
#include <imgui.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "EditorGizmo.h"
#include "EditorMcpProtocol.h"
#include "EditorSelection.h"

namespace SE {
class AssetManager;
class Clock;
class InputManager;
class Renderer;
class RenderTarget;
class SceneRuntime;
class ScriptSystem;
}

namespace Editor {

class Console;
class EditorCamera;
class UndoStack;
struct ViewportOptions;

// What the MCP tools may read and drive. Filled by EditorApp; every pointer
// outlives McpTools and is only touched on the main thread.
struct McpEditorAccess
{
    SE::SceneRuntime*   scene = nullptr;
    Selection*          selection = nullptr;
    UndoStack*          undo = nullptr;
    EditorCamera*       camera = nullptr;
    ViewportOptions*    viewOptions = nullptr;
    GizmoOp*            gizmoOp = nullptr;
    SE::ScriptSystem*   scripts = nullptr;
    Console*            console = nullptr;
    SE::InputManager*   input = nullptr;
    SE::Renderer*       renderer = nullptr;
    const SE::Clock*    clock = nullptr;
    SE::AssetManager*   assets = nullptr;
    const SE::RenderTarget* viewportRT = nullptr;
    const ImVec2*       imageMin = nullptr; // viewport image rect (ImGui screen coords)
    const ImVec2*       imageMax = nullptr;
    std::function<const char*()> playState;  // "edit" | "starting" | "playing" | "stopping"
    std::function<bool()> canTogglePlay;
    std::function<void()> requestTogglePlay;  // handled after this frame's UI
    std::function<bool()> isDirty;
    std::function<bool()> save;
    std::function<bool(const std::string&)> saveAs;
    // Loads FoxLevel JSON text as the unsaved edit state of the current scene.
    std::function<void(const std::string&)> loadEditedScene;
    // Increments every time a requested scene load has been applied and handled.
    std::function<uint64_t()> sceneReadyCount;
    std::function<DirectX::XMFLOAT3()> spawnPosition;
};

// MCP server for agents driving the running editor. Requests arrive on HTTP
// connection threads and are executed on the main thread in BeginFrame.
class McpTools
{
public:
    explicit McpTools(McpEditorAccess access);
    ~McpTools();
    McpTools(const McpTools&) = delete;
    McpTools& operator=(const McpTools&) = delete;

    bool Start(uint16_t port);   // logs the result; false = the editor runs without MCP
    void Stop();                 // fails pending calls, then stops the server

    void BeginFrame();           // main thread, start of OnUpdate
    bool AgentHasInputFocus() const; // a play_input is feeding the game this frame
    void OnViewportRendered();   // main thread, after the viewport image is final
    void OnFrameRendered();      // main thread, before Present

    // Tool definitions (also used to validate arguments).
    static std::vector<Mcp::ToolSpec> Specs();

private:
    struct PendingCall;
    struct Waiter;
    struct Capture;
    struct PlayInput;
    using CallPtr = std::shared_ptr<PendingCall>;
    using Poll = std::function<std::optional<Mcp::ToolResult>()>;

    // A handler finishes now (result), polls every later frame (poll), or
    // has handed the call to a capture / play_input (deferred).
    struct Outcome
    {
        std::optional<Mcp::ToolResult> result;
        Poll poll;
        bool deferred = false;
    };
    static Outcome Ok(const nlohmann::json& value);
    static Outcome Fail(const std::string& message);

    Mcp::ToolResult Call(const std::string& name, const nlohmann::json& arguments);
    void Complete(const CallPtr& call, Mcp::ToolResult result);
    Outcome Run(const CallPtr& call);
    void ServeCaptures(bool viewportOnly);

    // Tools
    Outcome EditorStatus();
    Outcome ListEntities(const nlohmann::json& args);
    Outcome GetEntity(const nlohmann::json& args);
    Outcome GetSceneJson();
    Outcome GetLogs(const nlohmann::json& args);
    Outcome Screenshot(const CallPtr& call, const nlohmann::json& args);
    Outcome Select(const nlohmann::json& args);
    Outcome SetTransform(const nlohmann::json& args);
    Outcome RenameEntity(const nlohmann::json& args);
    Outcome AddEntity(const nlohmann::json& args);
    Outcome DuplicateEntity(const nlohmann::json& args);
    Outcome DeleteEntity(const nlohmann::json& args);
    Outcome UndoRedo(bool redo);
    Outcome SaveScene(const nlohmann::json& args);
    Outcome OpenScene(const nlohmann::json& args);
    Outcome ApplySceneJson(const nlohmann::json& args);
    Outcome SetCamera(const nlohmann::json& args);
    Outcome FrameEntity(const nlohmann::json& args);
    Outcome SetViewOptions(const nlohmann::json& args);
    Outcome Play(const nlohmann::json& args);
    Outcome EvalLua(const nlohmann::json& args);
    Outcome PlayInputTool(const CallPtr& call, const nlohmann::json& args);
    Outcome WaitFrames(const nlohmann::json& args);

    // Shared helpers
    bool Playing() const;
    std::optional<std::string> EditGuard() const; // error when edits are not allowed
    bool ResolveTarget(const nlohmann::json& target, Selection& out, std::string& error) const;
    int Count(Selection::Kind kind) const;
    uint64_t EntityId(const Selection& sel) const;
    std::string DisplayName(const Selection& sel) const;
    nlohmann::json Describe(const Selection& sel) const;
    nlohmann::json SelectionRef(const Selection& sel) const;
    nlohmann::json CameraJson() const;
    nlohmann::json ViewOptionsJson() const;
    // Completes when the requested load was applied (onReady) or failed.
    Poll SceneLoadWaiter(std::function<Mcp::ToolResult()> onReady);
    void StepPlayInput();
    void EndPlayInput();

    McpEditorAccess m_access;
    std::string     m_projectName;
    std::unique_ptr<Mcp::Protocol>   m_protocol;
    std::unique_ptr<Mcp::HttpServer> m_server;
    uint16_t        m_port = 0;

    std::mutex           m_mutex; // guards m_queue and m_stopped
    std::deque<CallPtr>  m_queue;
    bool                 m_stopped = false;

    // Main thread only.
    std::vector<std::unique_ptr<Waiter>>  m_waiters;
    std::vector<std::unique_ptr<Capture>> m_captures;
    std::unique_ptr<PlayInput>            m_playInput;
    uint64_t                              m_agentFocusFrame = ~uint64_t(0);
};

} // namespace Editor
