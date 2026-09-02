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
#include "EditorTheme.h"
#include "EditorCamera.h"
#include "EditorPanels.h"

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
        Editor::ApplyTheme();

        if (!m_scene.Init(GetRenderer(), GetShaders(), GetAssets(), GetInput(),
                          GetWindow().GetHandle(),
                          GetWindow().GetWidth(), GetWindow().GetHeight()))
            return false;

        m_sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        if (m_sceneFiles.empty())
        {
            SE_LOG_ERROR("No scene files found in Assets/Scenes/");
            return false;
        }

        std::string targetScene = scenePath.empty() ? m_sceneFiles[0] : scenePath;
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
        for (int i = 0; i < (int)m_sceneFiles.size(); ++i)
            if (m_sceneFiles[i] == m_scene.currentScenePath) { m_selectedScene = i; break; }
        m_editorCam.SyncFromCamera(*m_scene.camera);
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
        DrawMainMenu();

        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (m_firstFrame)
        {
            m_firstFrame = false;
            BuildDefaultLayout(dockspaceId);
        }

        DrawViewportPanel(sceneActive);
        DrawScenePanel();
        DrawStatsPanel();
        Editor::DrawCameraPanel(m_scene, m_editorCam);
        Editor::DrawMaterialPanel(m_scene);
        Editor::DrawPhysicsPanel(m_scene);
        Editor::DrawLightingPanel(m_scene);
        Editor::DrawPostProcessPanel(m_scene, m_viewportRT.GetWidth(), m_viewportRT.GetHeight());
        Editor::DrawParticlesPanel(m_scene, GetAssets(), GetRenderer().GetDevice(), GetShaders());
    }

    void DrawMainMenu()
    {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void BuildDefaultLayout(ImGuiID dockspaceId)
    {
        FILE* f = nullptr;
        fopen_s(&f, "editor.ini", "r");
        const bool hasIni = (f != nullptr);
        if (f) fclose(f);
        if (hasIni)
            return;

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID dockCenter = dockspaceId;
        ImGuiID dockRight;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.22f, &dockRight, &dockCenter);
        ImGuiID dockRightTop, dockRightBottom;
        ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.30f, &dockRightTop, &dockRightBottom);

        ImGui::DockBuilderDockWindow("Viewport", dockCenter);

        // Top tabs: Scene active (last docked = default tab)
        ImGui::DockBuilderDockWindow("Stats",  dockRightTop);
        ImGui::DockBuilderDockWindow("Camera", dockRightTop);
        ImGui::DockBuilderDockWindow("Scene",  dockRightTop);

        // Bottom tabs: Lighting active (last docked = default tab)
        ImGui::DockBuilderDockWindow("Particles",       dockRightBottom);
        ImGui::DockBuilderDockWindow("Physics",         dockRightBottom);
        ImGui::DockBuilderDockWindow("Material",        dockRightBottom);
        ImGui::DockBuilderDockWindow("Post Processing", dockRightBottom);
        ImGui::DockBuilderDockWindow("Lighting",        dockRightBottom);
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
            Editor::DrawLightIndicators(m_scene, ImGui::GetItemRectMin(),
                                        ImGui::GetItemRectMax());
        }
        else
        {
            DrawViewportLoadingOverlay();
        }

        ImGui::End();
        ImGui::PopStyleVar();
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
                m_scene.RequestSceneLoad(m_sceneFiles[m_selectedScene]);
            }
        }
        ImGui::EndGroup();
    }

    void DrawScenePanel()
    {
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

        ImGui::Begin("Scene");
        if (ImGui::BeginCombo("Scene File", getFilename(m_sceneFiles[m_selectedScene]).c_str()))
        {
            for (int i = 0; i < (int)m_sceneFiles.size(); ++i)
            {
                bool selected = (i == m_selectedScene);
                if (ImGui::Selectable(getFilename(m_sceneFiles[i]).c_str(), selected) &&
                    i != m_selectedScene)
                {
                    m_scene.RequestSceneLoad(m_sceneFiles[i]);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Reload"))
            m_scene.RequestSceneLoad(m_sceneFiles[m_selectedScene]);
        ImGui::SameLine();
        if (ImGui::Button("Rescan"))
            m_sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        ImGui::Text("Active: %s", m_scene.currentDesc.name.c_str());
        if (!m_scene.sceneLoadError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               m_scene.sceneLoadError.c_str());
        ImGui::End();
    }

    void DrawStatsPanel()
    {
        ImGui::Begin("Stats");
        ImGui::Text("%.1f fps  |  %.2f ms",
                    GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f);
        ImGui::Text("Viewport: %ux%u", m_viewportRT.GetWidth(), m_viewportRT.GetHeight());
        uint32_t subMeshCount = 0;
        for (const auto& loaded : m_scene.loadedMeshes)
            if (loaded.mesh)
                subMeshCount += loaded.mesh->GetSubMeshCount();
        ImGui::Text("Meshes: %u  Textures: %u  Submeshes: %u",
                    GetAssets().CachedMeshCount(), GetAssets().CachedTextureCount(),
                    subMeshCount);
        ImGui::End();
    }

    SE::SceneRuntime      m_scene;
    Editor::EditorCamera  m_editorCam;
    SE::RenderTarget      m_viewportRT;
    ImVec2           m_viewportSize    = ImVec2(1280.0f, 720.0f);
    bool             m_viewportHovered = false;
    bool             m_firstFrame      = true;

    std::vector<std::string> m_sceneFiles;
    int                      m_selectedScene = 0;
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
