#include <windows.h>
#include <DirectXMath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Scene/SceneSaver.h"
#include "EditorTheme.h"
#include "EditorCamera.h"
#include "EditorPanels.h"
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorGizmo.h"
#include "EditorOutliner.h"
#include "EditorDetails.h"
#include "EditorContentBrowser.h"
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
        // The load worker may still be creating D3D resources. Join it before
        // the renderer and device are released by the base engine shutdown.
        m_scene.Shutdown();
        SE::Engine::Shutdown();
    }

    bool Setup(const std::string& scenePath = "")
    {
        // Keep editor dock layout separate from TestGame's imgui.ini.
        ImGui::GetIO().IniFilename = "editor.ini";
        SetTitleBase(L"FoxEditor");
        Editor::ApplyTheme();

        if (!m_scene.Init(GetRenderer(), GetShaders(), GetAssets(), GetInput(),
                          GetWindow().GetHandle(),
                          GetWindow().GetWidth(), GetWindow().GetHeight()))
            return false;

        const auto sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (sceneFiles.empty() && scenePath.empty())
        {
            SE_LOG_ERROR("No scene files found in Assets/Scenes/");
            return false;
        }

        std::string targetScene = scenePath.empty() ? sceneFiles[0] : scenePath;
        m_scene.RequestSceneLoad(targetScene);
        SE_LOG_INFO("Editor initial scene queued — %s", targetScene.c_str());
        return true;
    }

protected:
    void OnUpdate() override
    {
        if (m_scene.TickSceneLoad([this]() { return GetWindow().PumpMessages(); }))
            OnSceneReady();

        // Match the offscreen viewport RT to last frame's panel size before
        // this frame's ImGui::Image captures its SRV.
        UpdateViewportRT();

        const bool sceneActive = m_scene.HasScene() && !m_scene.IsLoading();
        if (sceneActive)
            UpdateDirtyFlag();
        if (sceneActive && m_viewportRT.GetRTV())
        {
            float dt = GetClock().GetDeltaTime();
            m_editorCam.Update(dt, GetInput(), *m_scene.camera,
                               GetWindow().GetHandle(), m_viewportHovered);
            m_scene.Update(dt, static_cast<float>(GetClock().GetTotalTime()));
            m_scene.Render(static_cast<float>(m_viewportRT.GetWidth()) /
                           static_cast<float>(m_viewportRT.GetHeight()));
        }

        DrawUI(sceneActive);
    }

    void OnPostProcess() override
    {
        if (m_scene.HasScene() && !m_scene.IsLoading() && m_viewportRT.GetRTV())
        {
            m_scene.PostProcess(m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                                GetClock().GetDeltaTime(),
                                [this]() { BindViewportRT(); });
        }
        // Bind + resolve the swap chain so ImGui draws over a known background.
        GetRenderer().ResolveToBackBuffer();
    }

private:
    void OnSceneReady()
    {
        m_editorCam.SyncFromCamera(*m_scene.camera);
        m_selection.Clear();
        m_undo.Clear(); // undo entries point into the previous scene
        TakeSavedSnapshot();
    }

    // --- Scene save + dirty tracking ---

    void SaveScene()
    {
        if (!m_scene.HasScene() || m_scene.IsLoading())
            return;
        m_scene.SyncDescriptorFromRuntime();
        if (SE::SceneSaver::SaveToFile(m_scene.currentScenePath, m_scene.currentDesc))
            TakeSavedSnapshot();
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
        auto pos = m_scene.currentScenePath.find_last_of("\\/");
        std::string file = (pos != std::string::npos)
            ? m_scene.currentScenePath.substr(pos + 1) : m_scene.currentScenePath;
        std::wstring title = L"FoxEditor \u2014 " + std::wstring(file.begin(), file.end());
        if (m_sceneDirty)
            title += L"*";
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
        HandleHotkeys();
        DrawMainMenu();

        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (m_firstFrame || m_resetLayout)
        {
            BuildDefaultLayout(dockspaceId, m_resetLayout);
            m_firstFrame  = false;
            m_resetLayout = false;
        }

        DrawViewportPanel(sceneActive);
        if (Editor::DrawOutlinerPanel(m_scene, m_selection, SpawnPosition()))
            m_undo.Clear(); // index-based undo entries are stale after a delete
        Editor::DrawDetailsPanel(m_scene, m_selection, m_undo, GetAssets(),
                                 GetRenderer().GetDevice(), GetShaders(),
                                 m_viewportRT.GetWidth(), m_viewportRT.GetHeight());
        // Mesh adds are appends, so existing undo entries stay valid.
        m_contentBrowser.Draw(m_scene, m_selection, GetAssets(),
                              GetRenderer().GetDevice(), GetRenderer().GetContext(),
                              SpawnPosition());
    }

    void DrawMainMenu()
    {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            const bool canSave = m_scene.HasScene() && !m_scene.IsLoading();
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canSave))
                SaveScene();
            if (ImGui::MenuItem("Reload Scene", nullptr, false, canSave))
                m_scene.RequestSceneLoad(m_scene.currentScenePath);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
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

    void DrawEditMenu()
    {
        if (!ImGui::BeginMenu("Edit"))
            return;
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_undo.CanUndo()))
            m_undo.Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_undo.CanRedo()))
            m_undo.Redo();
        ImGui::EndMenu();
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
    void HandleHotkeys()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
            return;
        SE::InputManager& input = GetInput();
        if (io.KeyCtrl)
        {
            if (input.IsKeyPressed('Z')) m_undo.Undo();
            if (input.IsKeyPressed('Y')) m_undo.Redo();
            if (input.IsKeyPressed('S')) SaveScene();
            if (input.IsKeyPressed('D')) Editor::DuplicateSelection(m_scene, m_selection);
            return;
        }
        if (input.IsKeyPressed(VK_DELETE) &&
            Editor::DeleteSelection(m_scene, m_selection))
            m_undo.Clear();
        if (input.IsKeyPressed(VK_F2))
            Editor::RequestRenameSelection(m_scene, m_selection);
        if (input.IsKeyPressed(VK_ESCAPE))
            m_selection.Clear();
        if (!m_viewportHovered || m_editorCam.IsFlying())
            return;
        HandleViewportKeys(input);
    }

    void HandleViewportKeys(SE::InputManager& input)
    {
        if (input.IsKeyPressed('Q')) m_gizmoOp = Editor::GizmoOp::None;
        if (input.IsKeyPressed('W')) m_gizmoOp = Editor::GizmoOp::Translate;
        if (input.IsKeyPressed('E')) m_gizmoOp = Editor::GizmoOp::Rotate;
        if (input.IsKeyPressed('R')) m_gizmoOp = Editor::GizmoOp::Scale;
        if (input.IsKeyPressed('F')) FocusSelection();
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
        if (Editor::GetSelectionBounds(m_scene, m_selection, center, radius))
            m_editorCam.FocusOn(center, radius);
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
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void DrawViewportPanel(bool sceneActive)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Viewport");
        m_viewportHovered = ImGui::IsWindowHovered();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_viewportSize = ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f));

        if (sceneActive && m_viewportRT.GetSRV())
        {
            ImGui::Image(reinterpret_cast<ImTextureID>(m_viewportRT.GetSRV()), avail);
            const ImVec2 imageMin   = ImGui::GetItemRectMin();
            const ImVec2 imageMax   = ImGui::GetItemRectMax();
            const bool imageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            DrawViewportOverlays(imageMin, imageMax, imageClicked);
            DrawViewportToolbar(imageMin, imageMax);
            DrawViewportStats(imageMin, imageMax);
        }
        else
        {
            DrawViewportLoadingOverlay();
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Gizmo, light indicators, and click picking on top of the viewport image.
    void DrawViewportOverlays(ImVec2 imageMin, ImVec2 imageMax, bool imageClicked)
    {
        const bool gizmoBusy = Editor::DrawGizmo(m_scene, m_selection, m_undo,
                                                 m_gizmoOp, imageMin, imageMax);
        const bool lightClicked =
            Editor::DrawLightIndicators(m_scene, imageMin, imageMax, m_selection);

        if (imageClicked && !gizmoBusy && !lightClicked && !ImGui::GetIO().KeyAlt)
            m_selection = Editor::PickScene(m_scene, imageMin, imageMax,
                                            ImGui::GetMousePos());
    }

    // UE-style viewport camera dropdown (top-right overlay button).
    void DrawViewportToolbar(ImVec2 imageMin, ImVec2 imageMax)
    {
        ImGui::SetCursorScreenPos({ imageMax.x - 84.0f, imageMin.y + 6.0f });
        if (ImGui::Button("Camera"))
            ImGui::OpenPopup("##viewport_camera");
        ImGui::SetNextWindowSize({ 340.0f, 0.0f });
        if (!ImGui::BeginPopup("##viewport_camera"))
            return;
        ImGui::TextDisabled("RMB fly  Alt+LMB orbit  MMB pan  F focus");
        ImGui::Separator();
        ImGui::PushItemWidth(210.0f);
        ImGui::SliderFloat("Fly Speed",   &m_editorCam.moveSpeed,   1.0f, 200.0f);
        ImGui::SliderFloat("Sensitivity", &m_editorCam.sensitivity, 0.05f, 0.5f);
        if (m_scene.HasScene())
        {
            ImGui::Separator();
            ImGui::SliderFloat("Field of View", &m_scene.camera->fovDeg,
                               20.0f, 120.0f, "%.0f deg");
            ImGui::SliderFloat("Near Z", &m_scene.camera->nearZ, 0.001f, 10.0f, "%.3f");
            ImGui::SliderFloat("Far Z",  &m_scene.camera->farZ, 100.0f, 20000.0f, "%.0f");
        }
        ImGui::PopItemWidth();
        ImGui::EndPopup();
    }

    // Frame/scene stats overlaid at the bottom-left of the viewport image.
    void DrawViewportStats(ImVec2 imageMin, ImVec2 imageMax)
    {
        uint32_t subMeshCount = 0;
        for (const auto& loaded : m_scene.loadedMeshes)
            if (loaded.mesh)
                subMeshCount += loaded.mesh->GetSubMeshCount();

        char line[160];
        sprintf_s(line, "%.1f fps  %.2f ms   |   %ux%u   |   meshes %u  tex %u  submeshes %u",
                  GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f,
                  m_viewportRT.GetWidth(), m_viewportRT.GetHeight(),
                  GetAssets().CachedMeshCount(), GetAssets().CachedTextureCount(),
                  subMeshCount);

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
                m_scene.RequestSceneLoad(m_scene.lastRequestedScenePath);
            }
        }
        ImGui::EndGroup();
    }

    SE::SceneRuntime      m_scene;
    Editor::EditorCamera  m_editorCam;
    Editor::Selection     m_selection;
    Editor::UndoStack     m_undo;
    Editor::ContentBrowser m_contentBrowser;
    Editor::GizmoOp       m_gizmoOp = Editor::GizmoOp::Translate;
    SE::RenderTarget      m_viewportRT;
    ImVec2           m_viewportSize    = ImVec2(1280.0f, 720.0f);
    bool             m_viewportHovered = false;
    bool             m_firstFrame      = true;
    bool             m_resetLayout     = false;

    std::string              m_savedSceneJson;
    bool                     m_sceneDirty       = false;
    int                      m_dirtyCheckFrames = 0;
};

// Set working directory to project root (walk up from exe location until we
// find Engine/ and Assets/).
static void SetProjectRootCwd()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; ++i)
    {
        if (std::filesystem::exists(dir / "Engine") && std::filesystem::exists(dir / "Assets"))
        {
            SetCurrentDirectoryW(dir.c_str());
            return;
        }
        dir = dir.parent_path();
    }
}

static std::string ParseSceneArg(LPSTR lpCmdLine)
{
    if (!lpCmdLine || strlen(lpCmdLine) == 0)
        return "";
    std::string args(lpCmdLine);
    auto pos = args.find("--scene");
    if (pos == std::string::npos)
        return "";
    pos += 7; // skip "--scene"
    while (pos < args.size() && (args[pos] == ' ' || args[pos] == '=')) ++pos;
    auto end = args.find(' ', pos);
    std::string scenePath = args.substr(pos, end - pos);
    if (scenePath.find('/') == std::string::npos && scenePath.find('\\') == std::string::npos)
        scenePath = "Assets/Scenes/" + scenePath;
    return scenePath;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    SetProjectRootCwd();

    SE::WindowDesc desc;
    desc.title  = L"FoxEditor";
    desc.width  = 1600;
    desc.height = 900;

    std::string scenePath = ParseSceneArg(lpCmdLine);

    EditorApp app;
    if (!app.Initialize(desc)) return 1;
    if (!app.Setup(scenePath)) return 1;
    app.Run();
    app.Shutdown();
    return 0;
}
