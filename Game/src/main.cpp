#include <windows.h>
#include <DirectXMath.h>
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneRuntime.h"

using namespace DirectX;

class Game : public SE::Engine
{
public:
    ~Game()
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

        std::string targetScene = scenePath;
        if (targetScene.empty())
            targetScene = m_sceneFiles[0];

        // Queue the first load so the render loop can present loading feedback
        // before potentially expensive asset import begins.
        m_initialScenePath = targetScene;
        m_scene.RequestSceneLoad(targetScene);
        m_initialSceneLoad = true;
        SE_LOG_INFO("Game initial scene queued — %s", targetScene.c_str());
        return true;
    }

protected:
    void OnUpdate() override
    {
        if (m_scene.TickSceneLoad([this]() { return GetWindow().PumpMessages(); }))
            OnSceneReady();

        if (m_initialSceneLoad)
        {
            if (!m_scene.sceneLoadError.empty())
            {
                if (GetInput().IsKeyPressed('R'))
                {
                    m_scene.sceneLoadError.clear();
                    m_scene.RequestSceneLoad(m_initialScenePath);
                }
                if (GetInput().IsKeyPressed(VK_ESCAPE))
                    PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
            }
            DrawLoadingUI();
            return;
        }
        if (m_scene.IsLoading())
        {
            DrawLoadingUI();
            return;
        }

        if (GetInput().IsKeyPressed(VK_ESCAPE))
            m_paused = !m_paused;

        float dt     = GetClock().GetDeltaTime();
        float aspect = (float)GetWindow().GetWidth() / (float)GetWindow().GetHeight();

        if (!m_paused)
            UpdateCameraAndCharacter(dt);

        // A paused game holds the simulation but keeps presenting the scene.
        m_scene.Update(m_paused ? 0.0f : dt,
                       static_cast<float>(GetClock().GetTotalTime()));
        m_scene.Render(aspect);

        if (m_showFps)
            DrawFpsCounter();
        if (m_paused)
            DrawPauseMenu();
    }

    void OnPostProcess() override
    {
        if (m_initialSceneLoad || m_scene.IsLoading())
        {
            // BeginFrame clears the MSAA surface, not the swap chain. Resolve it so
            // the loading UI is drawn over a known background before Present.
            GetRenderer().ResolveToBackBuffer();
            return;
        }

        m_scene.PostProcess(GetWindow().GetWidth(), GetWindow().GetHeight(),
                            GetClock().GetDeltaTime(),
                            [this]() { GetRenderer().BindBackBuffer(GetRenderer().GetContext()); });
    }

private:
    void OnSceneReady()
    {
        m_initialSceneLoad = false;
        m_paused = false;

        const std::string& name = m_scene.currentDesc.name;
        std::wstring title = L"FoxEngine " + std::wstring(name.begin(), name.end());
        SetWindowTextW(GetWindow().GetHandle(), title.c_str());
    }

    void UpdateCameraAndCharacter(float dt)
    {
        bool mouseBlocked = ImGui::GetIO().WantCaptureMouse;
        SE::CameraController::Mode prevMode = m_scene.camCtrl.GetMode();

        m_scene.camCtrl.Update(dt, GetInput(), *m_scene.camera,
                               GetWindow().GetHandle(), mouseBlocked);

        SE::CameraController::Mode mode = m_scene.camCtrl.GetMode();

        // Init character position when first entering FPS mode.
        if (prevMode == SE::CameraController::Mode::FreeFly &&
            mode     == SE::CameraController::Mode::FPS)
        {
            m_scene.cc.position = { m_scene.camera->eye.x,
                                    m_scene.camera->eye.y - m_scene.cc.eyeHeight,
                                    m_scene.camera->eye.z };
            m_scene.cc.velY = 0.0f;
        }

        // Drive character + camera in FPS mode.
        if (mode == SE::CameraController::Mode::FPS)
        {
            auto& inp = GetInput();
            auto& cc  = m_scene.cc;
            float yawRad = XMConvertToRadians(m_scene.camCtrl.fps.yawDeg);
            XMFLOAT3 fwd   = { sinf(yawRad), 0.0f,  cosf(yawRad) };
            XMFLOAT3 right = { cosf(yawRad), 0.0f, -sinf(yawRad) };

            XMFLOAT3 wishVel = { 0.0f, 0.0f, 0.0f };
            if (inp.IsKeyDown('W')) { wishVel.x += fwd.x;   wishVel.z += fwd.z; }
            if (inp.IsKeyDown('S')) { wishVel.x -= fwd.x;   wishVel.z -= fwd.z; }
            if (inp.IsKeyDown('D')) { wishVel.x += right.x; wishVel.z += right.z; }
            if (inp.IsKeyDown('A')) { wishVel.x -= right.x; wishVel.z -= right.z; }

            // Normalise diagonal, then scale to moveSpeed.
            float len2 = wishVel.x * wishVel.x + wishVel.z * wishVel.z;
            if (len2 > 1.0f) {
                float inv = 1.0f / sqrtf(len2);
                wishVel.x *= inv; wishVel.z *= inv;
            }
            wishVel.x *= cc.moveSpeed;
            wishVel.z *= cc.moveSpeed;

            if (inp.IsKeyPressed(VK_SPACE)) cc.Jump();

            m_scene.physicsWorld.StepCharacter(cc, wishVel, dt);

            // Sync camera to character eye.
            float pitchRad = XMConvertToRadians(m_scene.camCtrl.fps.pitchDeg);
            XMFLOAT3 eye  = cc.GetEyePosition();
            XMFLOAT3 look = {
                sinf(yawRad) * cosf(pitchRad),
                sinf(pitchRad),
                cosf(yawRad) * cosf(pitchRad)
            };
            m_scene.camera->eye    = eye;
            m_scene.camera->target = { eye.x + look.x, eye.y + look.y, eye.z + look.z };
            m_scene.camera->up     = { 0.0f, 1.0f, 0.0f };
        }
    }

    void DrawLoadingUI()
    {
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

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

        const float progress = std::clamp(status.progress, 0.0f, 1.0f);
        const std::string& path = m_scene.GetLoadingScenePath().empty()
            ? m_initialScenePath : m_scene.GetLoadingScenePath();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float overlayHeight = 64.0f;
        constexpr float barHeight = 6.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x,
                                      viewport->Pos.y + viewport->Size.y - overlayHeight),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, overlayHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 6.0f));
        ImGui::Begin("##LevelLoadingStrip", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoInputs);

        // blue (#00A0DE), with a dark shadow so the
        // borderless overlay remains legible over bright level backgrounds.
        constexpr ImU32 blue = IM_COL32(0, 160, 222, 255);
        constexpr ImU32 textShadow = IM_COL32(0, 18, 32, 230);
        const ImVec4 blueFloat(0.0f, 160.0f / 255.0f, 222.0f / 255.0f, 1.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto drawShadow = [&](const char* text) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            drawList->AddText(ImVec2(pos.x + 2.0f, pos.y + 2.0f), textShadow, text);
        };

        char levelText[256];
        sprintf_s(levelText, "// LOADING LEVEL: %s", getFilename(path).c_str());
        drawShadow(levelText);
        ImGui::TextColored(blueFloat, "%s", levelText);
        if (m_scene.sceneLoadError.empty())
        {
            drawShadow(phaseText);
            ImGui::TextColored(blueFloat, "%s", phaseText);
        }
        else
        {
            char errorText[256];
            sprintf_s(errorText, "%s  //  [R] RETRY   [ESC] QUIT",
                      m_scene.sceneLoadError.c_str());
            drawShadow(errorText);
            ImGui::TextColored(ImVec4(1.0f, 0.18f, 0.08f, 1.0f), "%s", errorText);
        }


        const ImVec2 barMin(viewport->Pos.x,
                            viewport->Pos.y + viewport->Size.y - barHeight);
        const ImVec2 barMax(viewport->Pos.x + viewport->Size.x * progress,
                            viewport->Pos.y + viewport->Size.y);
        drawList->AddRectFilled(barMin, barMax, blue);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void DrawFpsCounter()
    {
        char buf[64];
        sprintf_s(buf, "%.0f FPS  %.2f ms",
                  GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(ImVec2(12.0f, 12.0f), IM_COL32(0, 0, 0, 200), buf);
        dl->AddText(ImVec2(10.0f, 10.0f), IM_COL32(255, 255, 255, 220), buf);
    }

    // One combined anti-aliasing / upscaling choice, game-menu style.
    // FSR 3.1, engine TAA and FXAA are mutually exclusive paths.
    enum AAMode : int
    {
        AA_Off = 0, AA_FXAA, AA_TAA,
        AA_FSRNative, AA_FSRQuality, AA_FSRBalanced, AA_FSRPerformance,
        AA_Count
    };

    int CurrentAAMode() const
    {
        if (m_scene.fsrEnabled)
        {
            switch (m_scene.fsrQuality)
            {
            case 0: return AA_FSRQuality;
            case 1: return AA_FSRBalanced;
            case 2: return AA_FSRPerformance;
            default: return AA_FSRNative;
            }
        }
        if (m_scene.taa.enabled) return AA_TAA;
        if (m_scene.fxaa.enabled) return AA_FXAA;
        return AA_Off;
    }

    void ApplyAAMode(int mode)
    {
        m_scene.fsrEnabled   = mode >= AA_FSRNative;
        m_scene.taa.enabled  = mode == AA_TAA;
        m_scene.fxaa.enabled = mode == AA_FXAA;
        switch (mode)
        {
        case AA_FSRQuality:     m_scene.fsrQuality = 0; break;
        case AA_FSRBalanced:    m_scene.fsrQuality = 1; break;
        case AA_FSRPerformance: m_scene.fsrQuality = 2; break;
        case AA_FSRNative:      m_scene.fsrQuality = 3; break;
        default: break;
        }
        m_scene.taa.ResetHistory();
    }

    void DrawGraphicsSettings()
    {
        static const char* aaModes[AA_Count] = {
            "Off", "FXAA", "TAA",
            "FSR 3.1 Native AA", "FSR 3.1 Quality", "FSR 3.1 Balanced",
            "FSR 3.1 Performance"
        };
        int aaMode = CurrentAAMode();
        if (ImGui::Combo("Anti-Aliasing", &aaMode, aaModes, AA_Count))
            ApplyAAMode(aaMode);

        if (m_scene.fsrEnabled)
        {
            ImGui::Checkbox("Sharpening", &m_scene.fsrSharpen);
            if (m_scene.fsrSharpen)
                ImGui::SliderFloat("Sharpness", &m_scene.fsrSharpness, 0.0f, 1.0f, "%.2f");
        }

        ImGui::Checkbox("Bloom",          &m_scene.bloom.enabled);
        ImGui::Checkbox("Ambient Occlusion", &m_scene.ssao.enabled);
        ImGui::Checkbox("Volumetric Fog", &m_scene.volFog.enabled);
        ImGui::Checkbox("Show FPS",       &m_showFps);
    }

    void DrawLevelSelect()
    {
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

        for (const std::string& file : m_sceneFiles)
        {
            const bool active = (file == m_scene.currentScenePath);
            if (ImGui::Selectable(getFilename(file).c_str(), active) && !active)
            {
                m_scene.RequestSceneLoad(file);
                m_paused = false;
            }
        }
    }

    void DrawPauseMenu()
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();

        // Dim the scene behind the menu.
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->Pos,
            ImVec2(viewport->Pos.x + viewport->Size.x,
                   viewport->Pos.y + viewport->Size.y),
            IM_COL32(0, 0, 0, 140));

        const float menuWidth = 380.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(menuWidth, 0.0f), ImGuiCond_Always);
        ImGui::Begin("##PauseMenu", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize);

        const float contentWidth = ImGui::GetContentRegionAvail().x;
        {
            const char* title = "PAUSED";
            float textWidth = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((menuWidth - textWidth) * 0.5f);
            ImGui::TextUnformatted(title);
        }
        ImGui::Spacing();

        if (ImGui::Button("Resume", ImVec2(contentWidth, 0.0f)))
            m_paused = false;

        ImGui::Spacing();
        ImGui::SeparatorText("Graphics");
        DrawGraphicsSettings();

        ImGui::Spacing();
        ImGui::SeparatorText("Level");
        DrawLevelSelect();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Quit", ImVec2(contentWidth, 0.0f)))
            PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);

        ImGui::End();
    }

    SE::SceneRuntime m_scene;

    std::vector<std::string> m_sceneFiles;
    std::string              m_initialScenePath;
    bool                     m_initialSceneLoad = false;
    bool                     m_paused           = false;
    bool                     m_showFps          = false;
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Set working directory to project root (walk up from exe location until we find Engine/ and Assets/)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 5; ++i)
        {
            if (std::filesystem::exists(dir / "Engine") && std::filesystem::exists(dir / "Assets"))
            {
                SetCurrentDirectoryW(dir.c_str());
                break;
            }
            dir = dir.parent_path();
        }
    }

    SE::WindowDesc desc;
    desc.title  = L"FoxEngine";
    desc.width  = 1280;
    desc.height = 720;

    // Parse --scene argument
    std::string scenePath;
    if (lpCmdLine && strlen(lpCmdLine) > 0)
    {
        std::string args(lpCmdLine);
        auto pos = args.find("--scene");
        if (pos != std::string::npos)
        {
            pos += 7; // skip "--scene"
            while (pos < args.size() && (args[pos] == ' ' || args[pos] == '=')) ++pos;
            auto end = args.find(' ', pos);
            scenePath = args.substr(pos, end - pos);
            // If just a filename, prepend Assets/Scenes/
            if (scenePath.find('/') == std::string::npos && scenePath.find('\\') == std::string::npos)
                scenePath = "Assets/Scenes/" + scenePath;
        }
    }

    Game game;
    if (!game.Initialize(desc)) return 1;
    if (!game.Setup(scenePath)) return 1;
    game.Run();
    game.Shutdown();
    return 0;
}
