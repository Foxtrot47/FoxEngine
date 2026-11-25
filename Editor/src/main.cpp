#include <windows.h>
#include <shellapi.h>
#include <DirectXMath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Scene/SceneSaver.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "EditorTheme.h"
#include "EditorCamera.h"
#include "EditorViewportToolbar.h"
#include "EditorPanels.h"
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorGizmo.h"
#include "EditorEntityOps.h"
#include "EditorOutliner.h"
#include "EditorDetails.h"
#include "EditorContentBrowser.h"
#include "EditorConsole.h"
#include "EditorScripts.h"
#include "EditorMcpTools.h"
#include <ImGuizmo.h>

class EditorApp : public SE::Engine
{
public:
    ~EditorApp()
    {
        m_scene.Shutdown();
    }

    void Shutdown()
    {
        m_mcp.reset();
        m_editorCam.CancelDrag();
        if (m_scripts.IsRunning())
            m_scripts.Stop();
        GetInput().SetCursorCaptured(false);
        m_scriptCatalog.Stop();
        // The load worker may still be creating D3D resources. Join it before
        // the renderer and device are released by the base engine shutdown.
        m_scene.Shutdown();
        SE::Engine::Shutdown();
        m_console.Detach();
    }

    // mcpPort 0 disables the MCP server.
    bool Setup(const std::string& scenePath, uint16_t mcpPort)
    {
        m_console.Attach();
        // Keep editor dock layout separate from the player's imgui.ini.
        ImGui::GetIO().IniFilename = "editor.ini";
        SetTitleBase(L"FoxEditor");
        Editor::ApplyTheme();
        // Window close goes through the unsaved-changes prompt.
        GetWindow().SetCloseInterception(true);

        if (!m_scene.Init(GetRenderer(), GetShaders(), GetAssets(), GetInput(),
                          GetWindow().GetHandle(),
                          GetWindow().GetWidth(), GetWindow().GetHeight()))
            return false;
        // Script schemas for Details; a failure only disables property editing.
        m_scriptCatalog.Start();
        // Edit mode previews placed actors from their class files; the play
        // session installs its own resolver while it runs.
        m_scene.actorClassResolver = [this](const std::string& path) {
            return m_scriptCatalog.ResolveActorClass(path);
        };

        const auto sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (sceneFiles.empty() && scenePath.empty())
        {
            SE_LOG_ERROR("No scene files found in Assets/Scenes/");
            return false;
        }

        std::string targetScene = scenePath.empty() ? sceneFiles[0] : scenePath;
        m_scene.RequestSceneLoad(targetScene);
        SE_LOG_INFO("Editor initial scene queued — %s", targetScene.c_str());
        if (mcpPort != 0)
            StartMcp(mcpPort);
        return true;
    }

protected:
    void OnUpdate() override
    {
        // A ready signal while another load is already in flight belongs to a
        // superseded request (e.g. Stop pressed while the play scene loaded).
        if (m_scene.TickSceneLoad([this]() { return GetWindow().PumpMessages(); }) &&
            !m_scene.IsLoading())
            OnSceneLoadReady();
        UpdatePlayTransitions();
        if (m_mcp) m_mcp->BeginFrame();

        // Match the offscreen viewport RT to last frame's panel size before
        // this frame's ImGui::Image captures its SRV.
        UpdateViewportRT();

        const bool sceneActive = m_scene.HasScene() && !m_scene.IsLoading();
        if (sceneActive && m_playState == PlayState::Edit)
            UpdateDirtyFlag();
        // Resolve this frame's image, toolbar and gizmo ownership before
        // camera input. Using last frame's window hover let UI clicks navigate.
        DrawUI(sceneActive);
        if (std::exchange(m_togglePlayRequested, false))
            TogglePlay();
        if (m_scene.HasScene() && !m_scene.IsLoading() && m_viewportRT.GetRTV())
            UpdateSceneViewport();

        const std::string openRequest = m_contentBrowser.TakeSceneOpenRequest();
        if (!openRequest.empty())
            RequestSceneAction(SceneAction::Open, openRequest);
        if (GetWindow().ConsumeCloseRequest())
            RequestExit();
    }

    void OnPostProcess() override
    {
        const bool rendered = m_scene.HasScene() && !m_scene.IsLoading() && m_viewportRT.GetRTV();
        if (rendered)
        {
            const bool animate = m_viewOptions.realtime || m_playState != PlayState::Edit;
            m_scene.PostProcess(m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                                animate ? GetClock().GetDeltaTime() : 0.0f,
                                [this]() { BindViewportRT(); });
        }
        // Bind the swap chain so ImGui draws over a known background.
        GetRenderer().BindBackBuffer(GetRenderer().GetContext());
        if (rendered && m_mcp) m_mcp->OnViewportRendered();
    }

    void OnPrePresent() override
    {
        if (m_mcp) m_mcp->OnFrameRendered();
    }

private:
    void StartMcp(uint16_t port)
    {
        Editor::McpEditorAccess access;
        access.scene       = &m_scene;
        access.selection   = &m_selection;
        access.undo        = &m_undo;
        access.camera      = &m_editorCam;
        access.viewOptions = &m_viewOptions;
        access.gizmoOp     = &m_gizmoOp;
        access.scripts     = &m_scripts;
        access.console     = &m_console;
        access.input       = &GetInput();
        access.renderer    = &GetRenderer();
        access.clock       = &GetClock();
        access.assets      = &GetAssets();
        access.viewportRT  = &m_viewportRT;
        access.imageMin    = &m_imageMin;
        access.imageMax    = &m_imageMax;
        access.playState = [this]() {
            switch (m_playState)
            {
            case PlayState::Starting: return "starting";
            case PlayState::Playing:  return "playing";
            case PlayState::Stopping: return "stopping";
            default:                  return "edit";
            }
        };
        access.canTogglePlay     = [this]() { return CanTogglePlay(); };
        access.requestTogglePlay = [this]() { m_togglePlayRequested = true; };
        access.isDirty           = [this]() { return IsSceneDirtyNow(); };
        access.save              = [this]() { return SaveScene(); };
        access.saveAs            = [this](const std::string& path) { return SaveSceneAs(path); };
        access.loadEditedScene   = [this](const std::string& json) {
            m_editedSceneJson = json;
            m_scene.RequestSceneLoad(m_scene.currentScenePath, json);
        };
        access.sceneReadyCount   = [this]() { return m_sceneReadyCount; };
        access.spawnPosition     = [this]() { return SpawnPosition(); };
        m_mcp = std::make_unique<Editor::McpTools>(std::move(access));
        if (!m_mcp->Start(port))
            m_mcp.reset();
    }

    // Edit: the editor owns the scene. Starting/Stopping: the play or edit
    // copy of the scene is loading. Playing: scripts drive the scene.
    enum class PlayState { Edit, Starting, Playing, Stopping };

    // Pose of the scene camera component the editor camera drives.
    struct CameraPose
    {
        DirectX::XMFLOAT3 eye = {}, target = {}, up = {};
        float fovDeg = 60.0f, nearZ = 0.1f, farZ = 1000.0f;
        bool  orthographic = false;
        float orthoHeight = 20.0f;
    };

    bool IsPlaySession() const { return m_playState != PlayState::Edit; }

    void UpdateSceneViewport()
    {
        const float dt = GetClock().GetDeltaTime();
        const float aspect = static_cast<float>(m_viewportRT.GetWidth()) /
                             static_cast<float>(m_viewportRT.GetHeight());
        if (m_playState == PlayState::Playing)
        {
            m_scene.drawDebugHelpers = false;
            m_scene.simulatePhysics = true;
            UpdatePlayViewport(dt);
            m_scene.Render(aspect);
            return;
        }
        if (IsPlaySession())
        {
            // Between play transitions: keep the image valid, simulate nothing.
            m_scene.drawDebugHelpers = false;
            m_scene.simulatePhysics = false;
            m_scene.Update(0.0f, m_previewTime);
            m_scene.Render(aspect);
            return;
        }
        const bool blocked = ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (blocked && !ImGuizmo::IsUsing()) m_editorCam.CancelDrag();
        m_editorCam.Update(dt, GetInput(), *m_scene.camera, GetWindow().GetHandle(),
            m_viewportHovered && !blocked, m_viewportSize.y);
        CheckViewportCameraCut();
        const float sceneDt = m_viewOptions.realtime ? dt : 0.0f;
        m_previewTime += sceneDt;
        m_scene.drawDebugHelpers = !m_viewOptions.gameView && m_viewOptions.showLights;
        // Edit mode animates the preview but never moves dynamic actors.
        m_scene.simulatePhysics = false;
        if (!m_scene.IsLoading())
            m_scriptCatalog.RefreshActorPreviews(m_scene);
        m_scene.Update(sceneDt, m_previewTime);
        m_scene.Render(aspect);
    }

    // Scripts see the viewport image as their game view; Simulate drives
    // the scene update.
    void UpdatePlayViewport(float dt)
    {
        const float width  = m_imageMax.x - m_imageMin.x;
        const float height = m_imageMax.y - m_imageMin.y;
        m_scripts.SetViewRect({ m_imageMin.x, m_imageMin.y, width, height });
        m_scripts.SetInputFocus(m_gameFocused || (m_mcp && m_mcp->AgentHasInputFocus()));
        const ImVec2 origin = ImGui::GetMainViewport()->Pos; // ImGui -> client coords
        RECT capture;
        capture.left   = static_cast<LONG>(m_imageMin.x - origin.x);
        capture.top    = static_cast<LONG>(m_imageMin.y - origin.y);
        capture.right  = static_cast<LONG>(m_imageMax.x - origin.x);
        capture.bottom = static_cast<LONG>(m_imageMax.y - origin.y);
        GetInput().SetCaptureRect(capture);
        m_scripts.Simulate(dt);
        m_playSimulated = true;
    }

    void CheckViewportCameraCut()
    {
        const auto view = m_editorCam.GetViewMode();
        const auto shading = m_scene.pipeline.viewShading;
        if (view == m_previousView && shading == m_previousShading) return;
        m_scene.ResetTemporalHistory();
        m_previousView = view;
        m_previousShading = shading;
    }

    void OnSceneLoadReady()
    {
        switch (m_playState)
        {
        case PlayState::Starting:
            m_playState = PlayState::Playing;
            m_playSimulated = false;
            m_gameFocused = true;
            m_scripts.OnSceneReady();
            SE_LOG_INFO("Play mode: running %s", m_playScenePath.c_str());
            break;
        case PlayState::Stopping:
            FinishStop();
            break;
        default:
            OnSceneReady();
            break;
        }
        ++m_sceneReadyCount;
    }

    void OnSceneReady()
    {
        m_previewTime = 0.0f;
        m_editorCam.SyncFromCamera(*m_scene.camera);
        m_selection.Clear();
        m_undo.Clear(); // undo entries point into the previous scene
        // An agent's apply_scene_json loads unsaved edits: compare them
        // against the last save instead of treating them as clean.
        if (!m_editedSceneJson.empty() && m_scene.lastRequestedSceneJson == m_editedSceneJson)
        {
            m_scene.SyncDescriptorFromRuntime();
            m_sceneDirty = SE::SceneSaver::ToJson(m_scene.currentDesc) != m_savedSceneJson;
            m_dirtyCheckFrames = 0;
            UpdateWindowTitle();
        }
        else
            TakeSavedSnapshot();
        m_editedSceneJson.clear();
    }

    // --- Play mode ---------------------------------------------------------

    bool CanTogglePlay() const
    {
        switch (m_playState)
        {
        case PlayState::Edit:
            return m_scene.HasScene() && !m_scene.IsLoading() &&
                   m_pendingAction == SceneAction::None;
        case PlayState::Starting:
        case PlayState::Playing:
            return true;
        case PlayState::Stopping: // only to retry a failed restore
            return !m_scene.IsLoading() && !m_scene.sceneLoadError.empty();
        }
        return false;
    }

    void TogglePlay()
    {
        if (!CanTogglePlay())
            return;
        switch (m_playState)
        {
        case PlayState::Edit:     StartPlay(); break;
        case PlayState::Starting:
        case PlayState::Playing:  StopPlay(); break;
        case PlayState::Stopping:
            m_scene.sceneLoadError.clear();
            m_scene.RequestSceneLoad(m_playScenePath, m_playEditJson);
            break;
        }
    }

    // The play session runs on a fresh copy of the unsaved edit state; the
    // edit state itself is kept as JSON and reloaded on Stop.
    void StartPlay()
    {
        m_editorCam.CancelDrag();
        m_scene.SyncDescriptorFromRuntime();
        m_playEditDesc       = m_scene.currentDesc;
        m_playEditJson       = SE::SceneSaver::ToJson(m_playEditDesc);
        m_playScenePath      = m_scene.currentScenePath;
        m_playSavedEditorCam = m_editorCam;
        m_playSavedCamera    = CapturePose(*m_scene.camera);
        m_playSavedPreviewTime = m_previewTime;

        SE::ProjectLoadResult loaded = SE::Project::Load();
        for (const std::string& error : loaded.errors)
            SE_LOG_ERROR("%s", error.c_str());
        const SE::ProjectSettings& project = loaded.project;

        SE::ScriptHost host;
        host.input        = &GetInput();
        host.window       = &GetWindow();
        host.scene        = &m_scene;
        host.projectName  = project.name;
        host.editor       = true;
        host.published    = false;
        host.hotReload    = true;
        host.inputActions = project.inputActions;
        host.scenes       = SE::Project::SceneList(project);
        // Never stop inside a script callback: the request is handled next frame.
        host.requestQuit  = [this]() { m_quitRequested = true; };
        host.setTitle     = [this](const std::string& title) { m_playTitle = title; };
        if (!m_scripts.Start(host, project.autoloads))
        {
            SE_LOG_ERROR("Play mode: could not create the script session");
            return;
        }

        m_undo.Clear(); // entries point into the edit scene, which is replaced
        m_pendingPick   = false;
        m_quitRequested = false;
        m_playTitle.clear();
        m_scene.sceneLoadError.clear();
        m_playState = PlayState::Starting;
        m_scene.RequestSceneLoad(m_playScenePath, m_playEditJson);
        UpdateWindowTitle();
        SE_LOG_INFO("Play mode: starting %s", m_playScenePath.c_str());
    }

    void StopPlay()
    {
        if (m_playState != PlayState::Starting && m_playState != PlayState::Playing)
            return;
        if (m_scripts.IsRunning())
            m_scripts.Stop();
        ReleaseGameFocus();
        m_quitRequested = false;
        m_playTitle.clear();
        m_scene.sceneLoadError.clear();
        m_playState = PlayState::Stopping;
        m_scene.RequestSceneLoad(m_playScenePath, m_playEditJson);
        UpdateWindowTitle();
        SE_LOG_INFO("Play mode: stopped");
    }

    // The edit state is back: restore the editor view and compare against
    // the last save (not the reload) for the dirty marker.
    void FinishStop()
    {
        m_playState = PlayState::Edit;
        m_editorCam = m_playSavedEditorCam;
        ApplyPose(m_playSavedCamera, *m_scene.camera);
        m_previewTime = m_playSavedPreviewTime;
        m_scene.ResetTemporalHistory();
        m_selection.Clear();
        m_undo.Clear();
        m_sceneDirty       = m_playEditJson != m_savedSceneJson;
        m_dirtyCheckFrames = 0;
        m_playEditJson.clear();
        m_playEditDesc = {};
        UpdateWindowTitle();
    }

    // Deferred work that must not run inside script callbacks.
    void UpdatePlayTransitions()
    {
        if (std::exchange(m_quitRequested, false))
            StopPlay();
        // A failed play load leaves the edit scene in place: go straight back.
        if (m_playState == PlayState::Starting && !m_scene.IsLoading() &&
            !m_scene.sceneLoadError.empty())
        {
            SE_LOG_ERROR("Play mode: scene load failed: %s", m_scene.sceneLoadError.c_str());
            m_scene.sceneLoadError.clear();
            if (m_scripts.IsRunning())
                m_scripts.Stop();
            ReleaseGameFocus();
            m_playState = PlayState::Edit;
            m_playEditJson.clear();
            m_playEditDesc = {};
            UpdateWindowTitle();
        }
    }

    void ReleaseGameFocus()
    {
        m_gameFocused = false;
        GetInput().SetCursorCaptured(false);
    }

    // Clicking the viewport image focuses the game; clicking any other editor
    // window (script windows excepted) or leaving the app releases it.
    void UpdateGameFocus(bool imageHovered)
    {
        if (m_playState != PlayState::Playing)
            return;
        if (GetForegroundWindow() != GetWindow().GetHandle())
        {
            if (m_gameFocused || GetInput().IsCursorCaptured())
                ReleaseGameFocus();
            return;
        }
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                             ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                             ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        if (!clicked)
            return;
        if (imageHovered)
            m_gameFocused = true;
        else if (m_gameFocused && !m_scripts.WantsMouse())
            ReleaseGameFocus();
    }

    static CameraPose CapturePose(const SE::CameraComponent& cam)
    {
        CameraPose pose;
        pose.eye = cam.eye;
        pose.target = cam.target;
        pose.up = cam.up;
        pose.fovDeg = cam.fovDeg;
        pose.nearZ = cam.nearZ;
        pose.farZ = cam.farZ;
        pose.orthographic = cam.orthographic;
        pose.orthoHeight = cam.orthoHeight;
        return pose;
    }

    static void ApplyPose(const CameraPose& pose, SE::CameraComponent& cam)
    {
        cam.eye = pose.eye;
        cam.target = pose.target;
        cam.up = pose.up;
        cam.fovDeg = pose.fovDeg;
        cam.nearZ = pose.nearZ;
        cam.farZ = pose.farZ;
        cam.orthographic = pose.orthographic;
        cam.orthoHeight = pose.orthoHeight;
    }

    // --- Scene save, open/exit guard + dirty tracking ---

    enum class SceneAction { None, Open, Exit };

    bool SaveScene()
    {
        if (IsPlaySession())
            return SavePlayEditState();
        return SaveSceneAs(m_scene.currentScenePath);
    }

    // While playing (or restoring) the runtime is not the edit state; the
    // snapshot taken on Play is.
    bool SavePlayEditState()
    {
        if (!SE::SceneSaver::SaveToFile(m_playScenePath, m_playEditDesc))
            return false;
        m_savedSceneJson   = m_playEditJson;
        m_sceneDirty       = false;
        m_dirtyCheckFrames = 0;
        UpdateWindowTitle();
        return true;
    }

    bool SaveSceneAs(const std::string& path)
    {
        if (!m_scene.HasScene() || m_scene.IsLoading() || IsPlaySession())
            return false;
        m_scene.SyncDescriptorFromRuntime();
        if (!SE::SceneSaver::SaveToFile(path, m_scene.currentDesc))
            return false;
        if (path != m_scene.currentScenePath)
        {
            m_scene.currentScenePath       = path;
            m_scene.lastRequestedScenePath = path;
            m_contentBrowser.RequestRefresh();
        }
        TakeSavedSnapshot();
        return true;
    }

    // Unthrottled dirty check for decisions that discard the scene.
    bool IsSceneDirtyNow()
    {
        if (IsPlaySession())
            return m_playEditJson != m_savedSceneJson;
        if (!m_scene.HasScene() || m_scene.IsLoading())
            return false;
        m_scene.SyncDescriptorFromRuntime();
        return SE::SceneSaver::ToJson(m_scene.currentDesc) != m_savedSceneJson;
    }

    // Opening another scene, reloading, or exiting first asks to save
    // unsaved edits; clean scenes proceed immediately.
    void RequestSceneAction(SceneAction action, const std::string& path = {})
    {
        if (action == SceneAction::Open && (m_scene.IsLoading() || IsPlaySession()))
            return;
        m_pendingAction = action;
        m_pendingScenePath = path;
        if (IsSceneDirtyNow())
            m_openUnsavedPrompt = true;
        else
            PerformSceneAction();
    }

    // Exit stops a running game first; the save prompt then works on the
    // edit state kept from Play.
    void RequestExit()
    {
        StopPlay();
        RequestSceneAction(SceneAction::Exit);
    }

    void PerformSceneAction()
    {
        const SceneAction action = std::exchange(m_pendingAction, SceneAction::None);
        if (action == SceneAction::Open && !m_scene.IsLoading() && !IsPlaySession())
            m_scene.RequestSceneLoad(m_pendingScenePath);
        if (action == SceneAction::Exit)
        {
            GetWindow().SetCloseInterception(false);
            PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
        }
    }

    static std::string FileName(const std::string& path)
    {
        const size_t pos = path.find_last_of("\\/");
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

    // Snapshot the serialized scene as the clean state and clear the marker.
    void TakeSavedSnapshot()
    {
        m_scene.SyncDescriptorFromRuntime();
        m_savedSceneJson = SE::SceneSaver::ToJson(m_scene.currentDesc);
        m_sceneDirty       = false;
        m_dirtyCheckFrames = 0;
        UpdateWindowTitle();
    }

    // Serialize-and-compare dirty check, throttled — catches every edit path
    // (details, gizmos, undo/redo) without per-widget change wiring.
    void UpdateDirtyFlag()
    {
        if (++m_dirtyCheckFrames < 30)
            return;
        m_dirtyCheckFrames = 0;
        m_scene.SyncDescriptorFromRuntime();
        const bool dirty = SE::SceneSaver::ToJson(m_scene.currentDesc) != m_savedSceneJson;
        if (dirty != m_sceneDirty)
        {
            m_sceneDirty = dirty;
            UpdateWindowTitle();
        }
    }

    void UpdateWindowTitle()
    {
        const std::string file = FileName(m_scene.currentScenePath);
        std::wstring title = L"FoxEditor \u2014 " + std::wstring(file.begin(), file.end());
        if (m_sceneDirty)
            title += L"*";
        if (IsPlaySession())
            title += L" [Playing]";
        SetTitleBase(title);
    }

    void BindViewportRT()
    {
        ID3D11DeviceContext* ctx = GetRenderer().GetContext();
        ID3D11RenderTargetView* rtv = m_viewportRT.GetRTV();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<float>(m_viewportRT.GetWidth());
        vp.Height   = static_cast<float>(m_viewportRT.GetHeight());
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    }

    void UpdateViewportRT()
    {
        const uint32_t w = (std::max)(1u, static_cast<uint32_t>(m_viewportSize.x));
        const uint32_t h = (std::max)(1u, static_cast<uint32_t>(m_viewportSize.y));
        if (m_viewportRT.GetRTV() && m_viewportRT.GetWidth() == w &&
            m_viewportRT.GetHeight() == h)
            return;
        m_viewportRT.Shutdown();
        m_viewportRT.Init(GetRenderer().GetDevice(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    void DrawUI(bool sceneActive)
    {
        ImGuizmo::BeginFrame();
        DrawMainMenu();
        DrawUnsavedChangesModal();
        DrawSaveAsModal();

        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (m_firstFrame || m_resetLayout)
        {
            BuildDefaultLayout(dockspaceId, m_resetLayout);
            m_firstFrame  = false;
            m_resetLayout = false;
        }

        DrawViewportPanel(sceneActive);
        // Script windows are submitted after the viewport image so they draw over it.
        if (sceneActive && m_playState == PlayState::Playing && m_playSimulated)
            m_scripts.DrawGui();
        HandleHotkeys();
        if (m_viewOptions.immersive) return;
        const bool playing = IsPlaySession();
        Editor::DrawOutlinerPanel(m_scene, m_selection, m_undo, SpawnPosition(), m_scriptCatalog, playing);
        Editor::DrawDetailsPanel(m_scene, m_selection, m_undo, GetAssets(),
                                 GetRenderer().GetDevice(), GetShaders(),
                                 m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                                 m_scriptCatalog, playing);
        m_contentBrowser.Draw(m_scene, m_selection, m_undo, GetAssets(),
                              GetRenderer().GetDevice(), GetRenderer().GetContext(),
                              SpawnPosition(), playing);
        // A layout saved before the Console existed has no entry for it: dock
        // it with the Content Browser, as the default layout does.
        if (const ImGuiWindow* browser = ImGui::FindWindowByName("Content Browser");
            browser && browser->DockId != 0)
            ImGui::SetNextWindowDockID(browser->DockId, ImGuiCond_FirstUseEver);
        m_console.Draw(m_scripts, m_playState == PlayState::Playing);
    }

    void DrawMainMenu()
    {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            const bool canSave = m_scene.HasScene() && !m_scene.IsLoading() && !IsPlaySession();
            DrawOpenSceneMenu();
            ImGui::Separator();
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canSave))
                SaveScene();
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, canSave))
                BeginSaveAs();
            if (ImGui::MenuItem("Reload Scene", nullptr, false, canSave))
                RequestSceneAction(SceneAction::Open, m_scene.currentScenePath);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                RequestExit();
            ImGui::EndMenu();
        }
        DrawEditMenu();
        DrawToolsMenu();
        if (ImGui::BeginMenu("Window"))
        {
            if (ImGui::MenuItem("Reset Layout"))
                m_resetLayout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void DrawOpenSceneMenu()
    {
        if (!ImGui::BeginMenu("Open Scene", !m_scene.IsLoading() && !IsPlaySession()))
            return;
        const auto scenes = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (scenes.empty())
            ImGui::TextDisabled("No scenes in Assets/Scenes");
        for (const std::string& path : scenes)
        {
            const bool current = FileName(path) == FileName(m_scene.currentScenePath);
            if (ImGui::MenuItem(FileName(path).c_str(), nullptr, current))
                RequestSceneAction(SceneAction::Open, path);
        }
        ImGui::EndMenu();
    }

    void DrawEditMenu()
    {
        if (!ImGui::BeginMenu("Edit"))
            return;
        const bool canEdit = m_scene.HasScene() && !m_scene.IsLoading() && !IsPlaySession();
        const std::string undoText = m_undo.CanUndo()
            ? std::string("Undo ") + m_undo.UndoLabel() : std::string("Undo");
        const std::string redoText = m_undo.CanRedo()
            ? std::string("Redo ") + m_undo.RedoLabel() : std::string("Redo");
        if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false, canEdit && m_undo.CanUndo()))
            m_undo.Undo();
        if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false, canEdit && m_undo.CanRedo()))
            m_undo.Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2", false, canEdit))
            Editor::RequestRenameSelection(m_scene, m_selection);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canEdit))
            Editor::DuplicateSelection(m_scene, m_selection, m_undo);
        if (ImGui::MenuItem("Delete", "Del", false, canEdit))
            Editor::DeleteSelection(m_scene, m_selection, m_undo);
        ImGui::Separator();
        const char* playLabel = IsPlaySession() ? "Stop" : "Play";
        if (ImGui::MenuItem(playLabel, "F5", false, CanTogglePlay()))
            m_togglePlayRequested = true;
        ImGui::EndMenu();
    }

    void DrawUnsavedChangesModal()
    {
        constexpr const char* k_popup = "Unsaved Changes";
        if (std::exchange(m_openUnsavedPrompt, false))
        {
            m_saveError = false;
            ImGui::OpenPopup(k_popup);
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(k_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("Save changes to %s before %s?",
                    FileName(m_scene.currentScenePath).c_str(),
                    m_pendingAction == SceneAction::Exit ? "exiting" : "closing it");
        if (m_saveError)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Save failed; see log.");
        ImGui::Spacing();
        bool proceed = false;
        if (ImGui::Button("Save", ImVec2(110.0f, 0.0f)))
        {
            m_saveError = !SaveScene();
            proceed = !m_saveError;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110.0f, 0.0f)))
            proceed = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_pendingAction = SceneAction::None;
            ImGui::CloseCurrentPopup();
        }
        if (proceed)
        {
            ImGui::CloseCurrentPopup();
            PerformSceneAction();
        }
        ImGui::EndPopup();
    }

    void BeginSaveAs()
    {
        std::string stem = FileName(m_scene.currentScenePath);
        if (const size_t dot = stem.rfind('.'); dot != std::string::npos)
            stem.resize(dot);
        sprintf_s(m_saveAsName, "%s", stem.c_str());
        m_saveError = false;
        m_openSaveAs = true;
    }

    // Save As writes <name>.json into Assets/Scenes and switches the editor
    // to the new file.
    void DrawSaveAsModal()
    {
        constexpr const char* k_popup = "Save Scene As";
        if (std::exchange(m_openSaveAs, false))
            ImGui::OpenPopup(k_popup);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(k_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(320.0f);
        const bool entered = ImGui::InputText("##name", m_saveAsName, sizeof(m_saveAsName),
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                              ImGuiInputTextFlags_AutoSelectAll);
        std::string name = m_saveAsName;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
            name.resize(name.size() - 5);
        const bool valid = !name.empty() &&
            name.find_first_of("\\/:*?\"<>|") == std::string::npos;
        const std::string path = "Assets/Scenes/" + name + ".json";
        if (!valid)
            ImGui::TextDisabled("Enter a file name");
        else
            ImGui::TextDisabled("%s", path.c_str());
        if (valid && std::filesystem::exists(path))
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Overwrites the existing file.");
        if (m_saveError)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Save failed; see log.");

        ImGui::BeginDisabled(!valid);
        const bool save = ImGui::Button("Save", ImVec2(110.0f, 0.0f)) || (entered && valid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        if (save)
        {
            m_saveError = !SaveSceneAs(path);
            if (!m_saveError)
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawToolsMenu()
    {
        if (!ImGui::BeginMenu("Tools"))
            return;
        const bool hasScene = m_scene.HasScene();
        if (ImGui::MenuItem("Fire All Single Emitters", nullptr, false, hasScene))
        {
            for (auto& emitter : m_scene.particleSystems)
                if (emitter->config.emitMode == 1)
                    emitter->Burst(8);
        }
        if (ImGui::MenuItem("Rescan Particle Presets", nullptr, false, hasScene))
            m_scene.RefreshParticlePresets();
        ImGui::EndMenu();
    }

    // UE-style bindings: Q/W/E/R select gizmo op over the viewport,
    // Ctrl+Z / Ctrl+Y undo/redo anywhere outside text input.
    bool CanUseEditorShortcuts()
    {
        return m_scene.HasScene() && !m_scene.IsLoading() &&
            !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
            !m_editorCam.IsNavigating() && !GetInput().IsKeyDown(VK_RBUTTON);
    }

    void HandleHotkeys()
    {
        // F5 and Shift+F1 work even while the game has focus.
        HandlePlayKeys();
        if (m_gameFocused) return;
        if (!CanUseEditorShortcuts()) return;
        const ImGuiIO& io = ImGui::GetIO();
        auto& input = GetInput();
        if (IsPlaySession())
        {
            // Editing is off while playing; only view toggles remain.
            if (m_viewportHovered && !io.KeyCtrl && !io.KeyAlt &&
                input.IsKeyPressed(VK_F11))
                m_viewOptions.immersive = !m_viewOptions.immersive;
            return;
        }
        if (io.KeyCtrl)
        {
            HandleControlKeys(input);
            return;
        }
        if (io.KeyAlt)
        {
            if (m_viewportHovered) HandleViewModeKeys(input, io.KeyShift);
            return;
        }
        HandleEditKeys(input);
        if (m_viewportHovered) HandleViewportKeys(input);
    }

    void HandlePlayKeys()
    {
        const ImGuiIO& io = ImGui::GetIO();
        auto& input = GetInput();
        if (io.KeyShift && input.IsKeyPressed(VK_F1))
            ReleaseGameFocus();
        // Outside the game, F5 must not fire while typing or in a modal prompt.
        const bool editorBusy = io.WantTextInput ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (input.IsKeyPressed(VK_F5) && (m_gameFocused || !editorBusy))
            m_togglePlayRequested = true;
    }

    void HandleControlKeys(SE::InputManager& input)
    {
        const bool shift = ImGui::GetIO().KeyShift;
        if (input.IsKeyPressed('Z')) shift ? m_undo.Redo() : m_undo.Undo();
        if (input.IsKeyPressed('Y')) m_undo.Redo();
        if (input.IsKeyPressed('S')) shift ? BeginSaveAs() : (void)SaveScene();
        if (input.IsKeyPressed('D')) Editor::DuplicateSelection(m_scene, m_selection, m_undo);
        if (m_viewportHovered && input.IsKeyPressed('R'))
            m_viewOptions.realtime = !m_viewOptions.realtime;
    }

    void HandleEditKeys(SE::InputManager& input)
    {
        if (input.IsKeyPressed(VK_DELETE)) Editor::DeleteSelection(m_scene, m_selection, m_undo);
        if (input.IsKeyPressed(VK_F2)) Editor::RequestRenameSelection(m_scene, m_selection);
        if (input.IsKeyPressed(VK_ESCAPE)) m_selection.Clear();
    }

    void HandleViewportKeys(SE::InputManager& input)
    {
        if (input.IsKeyPressed('Q')) m_gizmoOp = Editor::GizmoOp::None;
        if (input.IsKeyPressed('W')) m_gizmoOp = Editor::GizmoOp::Translate;
        if (input.IsKeyPressed('E')) m_gizmoOp = Editor::GizmoOp::Rotate;
        if (input.IsKeyPressed('R')) m_gizmoOp = Editor::GizmoOp::Scale;
        if (input.IsKeyPressed('F')) FocusSelection();
        if (input.IsKeyPressed(VK_SPACE))
            m_gizmoOp = static_cast<Editor::GizmoOp>(static_cast<int>(m_gizmoOp) % 3 + 1);
        if (input.IsKeyPressed('G')) m_viewOptions.gameView = !m_viewOptions.gameView;
        if (input.IsKeyPressed(VK_F11)) m_viewOptions.immersive = !m_viewOptions.immersive;
    }

    // Unreal view shortcuts. Shift chooses the opposite orthographic side.
    void HandleViewModeKeys(SE::InputManager& input, bool opposite)
    {
        if (!m_scene.HasScene() || !m_scene.camera)
            return;
        using VM = Editor::EditorCamera::ViewMode;
        auto set = [this](VM mode) { m_editorCam.SetViewMode(mode, *m_scene.camera); };

        if (input.IsKeyPressed('G')) set(VM::Perspective);
        if (input.IsKeyPressed('J')) set(opposite ? VM::Bottom : VM::Top);
        if (input.IsKeyPressed('H')) set(opposite ? VM::Back : VM::Front);
        if (input.IsKeyPressed('K')) set(opposite ? VM::Right : VM::Left);
    }

    // New entities spawn a few metres in front of the editor camera.
    DirectX::XMFLOAT3 SpawnPosition() const
    {
        using namespace DirectX;
        if (!m_scene.HasScene() || !m_scene.camera)
            return { 0.0f, 0.0f, 0.0f };
        const auto& cam = *m_scene.camera;
        const XMVECTOR eye = XMLoadFloat3(&cam.eye);
        const XMVECTOR dir = XMVector3Normalize(
            XMVectorSubtract(XMLoadFloat3(&cam.target), eye));
        XMFLOAT3 out;
        XMStoreFloat3(&out, XMVectorAdd(eye, XMVectorScale(dir, 8.0f)));
        return out;
    }

    void FocusSelection()
    {
        DirectX::XMFLOAT3 center;
        float radius;
        if (!Editor::GetSelectionBounds(m_scene, m_selection, center, radius))
            return;
        if (m_scene.camera)
        {
            m_editorCam.FocusOn(center, radius, *m_scene.camera,
                m_viewportSize.x / (std::max)(m_viewportSize.y, 1.0f));
            m_scene.ResetTemporalHistory();
        }
    }

    void BuildDefaultLayout(ImGuiID dockspaceId, bool force)
    {
        FILE* f = nullptr;
        fopen_s(&f, "editor.ini", "r");
        const bool hasIni = (f != nullptr);
        if (f) fclose(f);
        if (hasIni && !force)
            return;

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID dockCenter = dockspaceId;
        ImGuiID dockRight;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.22f, &dockRight, &dockCenter);
        ImGuiID dockBottom;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenter);
        ImGuiID dockRightTop, dockRightBottom;
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.30f, &dockRightTop, &dockRightBottom);

        ImGui::DockBuilderDockWindow("Viewport", dockCenter);

        ImGui::DockBuilderDockWindow("Outliner", dockRightTop);

        ImGui::DockBuilderDockWindow("Details", dockRightBottom);
        ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
        ImGui::DockBuilderDockWindow("Console", dockBottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void DrawViewportPanel(bool sceneActive)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar;
        if (m_viewOptions.immersive)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings;
        }
        const char* title = m_viewOptions.immersive ? "Immersive Viewport" : "Viewport";
        if (!ImGui::Begin(title, nullptr, flags))
        {
            m_viewportHovered = false;
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        if (sceneActive)
        {
            Editor::PlayControls play;
            play.playing    = IsPlaySession();
            play.canToggle  = CanTogglePlay();
            play.errorCount = m_scripts.IsRunning() ? m_scripts.ErrorCount() : 0;
            play.title      = m_playTitle.empty() ? nullptr : m_playTitle.c_str();
            Editor::DrawViewportToolbar(m_editorCam, *m_scene.camera, m_gizmoOp,
                                         m_gizmoSettings, m_viewOptions, m_scene.pipeline,
                                         play);
            if (play.toggleRequested)
                m_togglePlayRequested = true;
        }
        m_viewportHovered = false;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_viewportSize = ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f));

        // A failed restore after Stop keeps the stopped play scene loaded;
        // show the error and Retry instead of that image.
        const bool restoreFailed = m_playState == PlayState::Stopping &&
            !m_scene.sceneLoadError.empty();
        if (sceneActive && m_viewportRT.GetSRV() && !restoreFailed) DrawViewportImage(avail);
        else DrawViewportLoadingOverlay();

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void DrawViewportImage(ImVec2 size)
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(m_viewportRT.GetSRV()), size);
        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        m_imageMin = imageMin;
        m_imageMax = imageMax;
        const bool imageHovered = ImGui::IsItemHovered();
        if (IsPlaySession())
        {
            // The game owns the view: no editor overlays, picking or navigation.
            UpdateGameFocus(imageHovered);
            DrawPlayOverlay(imageMin, imageMax);
            if (m_viewOptions.showStats) DrawViewportStats(imageMin, imageMax);
            return;
        }
        m_viewportHovered = imageHovered;
        if (!m_viewOptions.gameView && m_viewOptions.showGrid)
            Editor::DrawOrthographicGrid(*m_scene.camera, imageMin, imageMax);
        DrawViewportOverlays(imageMin, imageMax);
        if (!m_viewOptions.gameView && m_viewOptions.showAxes)
            Editor::DrawOrientationAxes(*m_scene.camera, imageMin, imageMax);
        if (m_viewOptions.showStats) DrawViewportStats(imageMin, imageMax);
    }

    // Focus frame and hint over the game view.
    void DrawPlayOverlay(ImVec2 imageMin, ImVec2 imageMax)
    {
        if (m_playState != PlayState::Playing)
            return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (m_gameFocused)
        {
            dl->AddRect(imageMin, imageMax, IM_COL32(90, 170, 255, 200), 0.0f, 0, 2.0f);
            return;
        }
        const char* hint = "Click to focus the game  |  Shift+F1 releases focus  |  F5 stops";
        const ImVec2 pos = { imageMin.x + 8.0f, imageMin.y + 6.0f };
        dl->AddText({ pos.x + 1.0f, pos.y + 1.0f }, IM_COL32(0, 0, 0, 200), hint);
        dl->AddText(pos, IM_COL32(220, 220, 220, 230), hint);
    }

    // Gizmo, light indicators, and click picking on top of the viewport image.
    void DrawViewportOverlays(ImVec2 imageMin, ImVec2 imageMax)
    {
        const bool navigating = m_editorCam.IsNavigating() || GetInput().IsKeyDown(VK_RBUTTON);
        bool gizmoBusy = false;
        bool lightClicked = false;
        if (!m_viewOptions.gameView && !navigating)
        {
            gizmoBusy = Editor::DrawGizmo(m_scene, m_selection, m_undo, m_gizmoOp,
                                           m_gizmoSettings, imageMin, imageMax);
            if (m_viewOptions.showLights)
                lightClicked = Editor::DrawLightIndicators(m_scene, imageMin, imageMax, m_selection);
        }
        HandleViewportPicking(imageMin, imageMax, gizmoBusy || lightClicked);
        m_viewportHovered = m_viewportHovered && !gizmoBusy && !lightClicked;
    }

    void HandleViewportPicking(ImVec2 imageMin, ImVec2 imageMax, bool blocked)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_pendingPick = m_viewportHovered && !blocked && !io.KeyAlt;
        // Select on release, not press: LMB navigation must not change selection.
        if (blocked || IsPickingGestureCancelled())
            m_pendingPick = false;
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        if (m_pendingPick && m_viewportHovered)
            m_selection = Editor::PickScene(m_scene, imageMin, imageMax, ImGui::GetMousePos());
        m_pendingPick = false;
    }

    bool IsPickingGestureCancelled() const
    {
        return ImGui::GetIO().KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    }

    // Frame/scene stats overlaid at the bottom-left of the viewport image.
    void DrawViewportStats(ImVec2 imageMin, ImVec2 imageMax)
    {
        uint32_t subMeshCount = 0;
        for (const auto& loaded : m_scene.loadedMeshes)
            if (loaded.mesh)
                subMeshCount += loaded.mesh->GetSubMeshCount();

        char line[160];
        sprintf_s(line, "%.1f fps  %.2f ms   |   %ux%u   |   meshes %u  tex %u  submeshes %u   |   gpu %.2f ms",
                  GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f,
                  m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                  GetAssets().CachedMeshCount(), GetAssets().CachedTextureCount(),
                  subMeshCount, m_scene.GetGpuFrameMs());

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = { imageMin.x + 8.0f, imageMax.y - 24.0f };
        dl->AddText({ pos.x + 1.0f, pos.y + 1.0f }, IM_COL32(0, 0, 0, 200), line);
        dl->AddText(pos, IM_COL32(220, 220, 220, 230), line);
    }

    void DrawViewportLoadingOverlay()
    {
        const SE::LevelLoadStatus status = m_scene.GetLoadStatus();
        const char* phaseText = "READING LEVEL DATA";
        switch (status.state)
        {
        case SE::LevelLoadState::Preparing: phaseText = "PREPARING LEVEL DEPENDENCIES"; break;
        case SE::LevelLoadState::Uploading: phaseText = "UPLOADING LEVEL RESOURCES"; break;
        case SE::LevelLoadState::Complete:  phaseText = "READY"; break;
        case SE::LevelLoadState::Failed:    phaseText = "LOAD FAILED"; break;
        case SE::LevelLoadState::Cancelled: phaseText = "LOAD CANCELLED"; break;
        default: break;
        }

        const ImVec2 region = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(region.x * 0.5f - 120.0f, region.y * 0.5f - 20.0f));
        ImGui::BeginGroup();
        if (m_scene.sceneLoadError.empty())
        {
            ImGui::TextUnformatted(phaseText);
            ImGui::ProgressBar(std::clamp(status.progress, 0.0f, 1.0f),
                               ImVec2(240.0f, 6.0f), "");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               m_scene.sceneLoadError.c_str());
            if (ImGui::Button("Retry"))
            {
                m_scene.sceneLoadError.clear();
                m_scene.RequestSceneLoad(m_scene.lastRequestedScenePath,
                                         m_scene.lastRequestedSceneJson);
            }
        }
        ImGui::EndGroup();
    }

    // Declared first so it is destroyed last: everything else may still log.
    Editor::Console       m_console;
    SE::SceneRuntime      m_scene;
    // The play session references m_scene, so it is declared (and destroyed) after it.
    SE::ScriptSystem      m_scripts;
    Editor::ScriptCatalog m_scriptCatalog;
    Editor::EditorCamera  m_editorCam;
    Editor::Selection     m_selection;
    Editor::UndoStack     m_undo;
    Editor::ContentBrowser m_contentBrowser;
    Editor::GizmoOp       m_gizmoOp = Editor::GizmoOp::Translate;
    Editor::GizmoSettings m_gizmoSettings;
    Editor::ViewportOptions m_viewOptions;
    float m_previewTime = 0.0f;
    bool m_pendingPick = false;
    Editor::EditorCamera::ViewMode m_previousView = Editor::EditorCamera::ViewMode::Perspective;
    SE::ForwardPipeline::ViewShading m_previousShading = SE::ForwardPipeline::ViewShading::Lit;
    SE::RenderTarget      m_viewportRT;
    ImVec2           m_viewportSize    = ImVec2(1280.0f, 720.0f);
    ImVec2           m_imageMin        = ImVec2(0.0f, 0.0f);
    ImVec2           m_imageMax        = ImVec2(1280.0f, 720.0f);
    bool             m_viewportHovered = false;
    bool             m_firstFrame      = true;
    bool             m_resetLayout     = false;

    std::string              m_savedSceneJson;
    bool                     m_sceneDirty       = false;
    int                      m_dirtyCheckFrames = 0;

    SceneAction              m_pendingAction     = SceneAction::None;
    std::string              m_pendingScenePath;
    bool                     m_openUnsavedPrompt = false;
    bool                     m_openSaveAs        = false;
    bool                     m_saveError         = false;
    char                     m_saveAsName[128]   = {};

    // Play mode
    PlayState            m_playState = PlayState::Edit;
    std::string          m_playEditJson;      // the unsaved edit state, reloaded on Stop
    SE::SceneDescriptor  m_playEditDesc;      // same state, for saving on exit while playing
    std::string          m_playScenePath;
    Editor::EditorCamera m_playSavedEditorCam;
    CameraPose           m_playSavedCamera;
    float                m_playSavedPreviewTime = 0.0f;
    std::string          m_playTitle;         // fox.window.set_title, shown in the toolbar
    bool                 m_togglePlayRequested = false;
    bool                 m_quitRequested = false; // fox.app.quit, handled next frame
    bool                 m_gameFocused = false;
    bool                 m_playSimulated = false; // on_gui waits for the first Simulate

    std::string          m_editedSceneJson;   // apply_scene_json text being loaded
    uint64_t             m_sceneReadyCount = 0; // scene loads handled by OnSceneLoadReady
    // Declared last: destroyed first, while everything it references exists.
    std::unique_ptr<Editor::McpTools> m_mcp;
};

namespace {

std::string NarrowPath(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, narrow.data(), length, nullptr, nullptr);
    narrow.pop_back();
    return narrow;
}

void ShowFatalError(const std::string& message)
{
    MessageBoxA(nullptr, message.c_str(), "FoxEditor", MB_OK | MB_ICONERROR);
}

constexpr const char* k_usage =
    "--project <dir-or-project.json> --scene <path> --mcp-port <port> --no-mcp";

// 1..65535, or 0 when text is not such an integer.
uint16_t ParsePort(const std::wstring& text)
{
    if (text.empty() || text.size() > 5 ||
        !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        return 0;
    const unsigned long value = std::wcstoul(text.c_str(), nullptr, 10);
    return value >= 1 && value <= 65535 ? static_cast<uint16_t>(value) : 0;
}

struct LaunchOptions
{
    std::filesystem::path project;
    std::string scene;
    std::string error;
    std::vector<std::string> warnings;
    uint16_t mcpPort = 47800;
    bool mcp = true;
};

LaunchOptions ParseCommandLine()
{
    LaunchOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        options.error = "Could not parse the command line.";
        return options;
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--project" || arg == L"--scene")
        {
            if (i + 1 >= argc || argv[i + 1][0] == L'\0' ||
                std::wstring(argv[i + 1]).rfind(L"--", 0) == 0)
            {
                options.error = "Expected a path after " + NarrowPath(arg.c_str()) + ".";
                break;
            }
            if (arg == L"--project") options.project = argv[++i];
            else options.scene = NarrowPath(argv[++i]);
        }
        else if (arg.rfind(L"--project=", 0) == 0)
        {
            options.project = arg.substr(10);
            if (options.project.empty()) options.error = "Expected a path after --project=.";
        }
        else if (arg.rfind(L"--scene=", 0) == 0)
        {
            options.scene = NarrowPath(arg.c_str() + 8);
            if (options.scene.empty()) options.error = "Expected a path after --scene=.";
        }
        else if (arg == L"--mcp-port" || arg.rfind(L"--mcp-port=", 0) == 0)
        {
            const bool inlineValue = arg != L"--mcp-port";
            const std::wstring value = inlineValue ? arg.substr(11)
                                     : (i + 1 < argc ? std::wstring(argv[++i]) : std::wstring());
            options.mcpPort = ParsePort(value);
            if (options.mcpPort == 0)
            {
                options.error = "Expected a port 1-65535 after --mcp-port.";
                break;
            }
        }
        else if (arg == L"--no-mcp")
            options.mcp = false;
        else
            options.warnings.push_back("ignoring command-line argument '" + NarrowPath(arg.c_str()) +
                                       "' (usage: " + k_usage + ")");
    }
    LocalFree(argv);
    if (!options.scene.empty() && options.scene.find_first_of("\\/") == std::string::npos)
        options.scene = "Assets/Scenes/" + options.scene;
    return options;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    const LaunchOptions options = ParseCommandLine();
    if (!options.error.empty())
    {
        ShowFatalError(options.error + "\nUsage: " + k_usage);
        return 1;
    }
    if (!SE::SetProjectRootCwd(options.project))
    {
        ShowFatalError("Could not select a project. Use --project <dir-or-project.json> to select a valid project.");
        return 1;
    }

    const SE::ProjectLoadResult loaded = SE::Project::Load();
    if (!loaded.Succeeded())
    {
        std::string message = "project.json has errors (run `FoxProject check` for details):\n";
        for (size_t i = 0; i < loaded.errors.size() && i < 10; ++i)
            message += "\n" + loaded.errors[i];
        if (loaded.errors.size() > 10) message += "\n...";
        ShowFatalError(message);
        return 1;
    }

    SE::WindowDesc desc;
    desc.title  = L"FoxEditor";
    desc.width  = 1600;
    desc.height = 900;

    const std::string scenePath = options.scene.empty() ? loaded.project.startupScene : options.scene;

    EditorApp app;
    if (!app.Initialize(desc)) return 1;
    for (const std::string& warning : options.warnings)
        SE_LOG_WARN("%s", warning.c_str());
    if (!app.Setup(scenePath, options.mcp ? options.mcpPort : uint16_t(0))) return 1;
    app.Run();
    app.Shutdown();
    return 0;
}
