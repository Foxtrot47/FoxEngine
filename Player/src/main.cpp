// The generic game player. Everything game-specific lives in the project:
// project.json (window, startup scene, input actions, autoload scripts) and
// the scenes and Lua scripts it references. `FoxProject publish` ships this
// executable as <ProjectName>.exe next to the published content.
//
// Usage: FoxPlayer.exe [--project <dir-or-project.json>] [--scene <path>]

#include <windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace {

std::wstring Widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// Command-line paths, in the code page the engine's narrow file APIs use.
std::string NarrowPath(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, narrow.data(), length, nullptr, nullptr);
    narrow.pop_back(); // terminator
    return narrow;
}

std::string FileName(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void ShowFatalError(const std::string& message)
{
    MessageBoxW(nullptr, Widen(message).c_str(), L"FoxPlayer", MB_OK | MB_ICONERROR);
}

struct LaunchOptions
{
    std::filesystem::path project;
    std::string error;
    std::string scene;                 // overrides project.json startupScene
    std::vector<std::string> warnings; // logged once the log file is open
};

// Unknown arguments are warnings: launchers may pass their own.
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
        else
            options.warnings.push_back("ignoring command-line argument '" + NarrowPath(arg.c_str()) +
                                       "' (usage: --project <dir-or-project.json> --scene <path>)");
    }
    LocalFree(argv);
    if (!options.scene.empty() && options.scene.find_first_of("\\/") == std::string::npos)
        options.scene = "Assets/Scenes/" + options.scene;
    return options;
}

} // namespace

class Player : public SE::Engine
{
public:
    ~Player()
    {
        m_scene.Shutdown();
    }

    void Shutdown()
    {
        if (m_scripts.IsRunning())
            m_scripts.Stop();
        GetInput().SetCursorCaptured(false);
        // The load worker may still be creating D3D resources. Join it before
        // the renderer and device are released by the base engine shutdown.
        m_scene.Shutdown();
        SE::Engine::Shutdown();
    }

    bool Setup(const SE::ProjectSettings& project, const std::string& startupScene)
    {
        m_title = project.window.title.empty() ? project.name : project.window.title;
        // Development runs show frame timings in the title; published games do not.
        SetTitleStats(!project.published);
        SetTitleBase(Widen(m_title));
        if (project.window.fullscreen)
            GetWindow().ToggleFullscreen();

        if (!m_scene.Init(GetRenderer(), GetShaders(), GetAssets(), GetInput(),
                          GetWindow().GetHandle(),
                          GetWindow().GetWidth(), GetWindow().GetHeight()))
            return false;

        SE::ScriptHost host;
        host.input        = &GetInput();
        host.window       = &GetWindow();
        host.scene        = &m_scene;
        host.projectName  = project.name;
        host.published    = project.published;
        // Development runs pick up saved scripts while playing.
        host.hotReload    = !project.published;
        host.inputActions = project.inputActions;
        host.scenes       = SE::Project::SceneList(project);
        // Posted, so it is safe inside a script callback.
        host.requestQuit  = [this]() { PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0); };
        host.setTitle     = [this](const std::string& title) {
            SetTitleBase(Widen(title.empty() ? m_title : title));
        };
        if (!m_scripts.Start(host, project.autoloads))
            return false;

        m_startupScene = startupScene;
        m_scene.RequestSceneLoad(startupScene);
        SE_LOG_INFO("Player: running '%s' from %s", project.name.c_str(), startupScene.c_str());
        return true;
    }

protected:
    void OnUpdate() override
    {
        // A ready signal while another load is already in flight belongs to
        // a superseded request (a script loaded another scene meanwhile).
        if (m_scene.TickSceneLoad([this]() { return GetWindow().PumpMessages(); }) &&
            !m_scene.IsLoading())
            m_scripts.OnSceneReady();

        if (!m_scene.HasScene() || m_scene.IsLoading())
        {
            if (!m_scene.HasScene() && !m_scene.sceneLoadError.empty())
                UpdateStartupFailure();
            DrawLoadingUI();
            return;
        }
        if (!m_scene.sceneLoadError.empty())
        {
            // A failed fox.scene.load leaves the current scene running.
            SE_LOG_ERROR("Could not load scene %s: %s", m_scene.lastRequestedScenePath.c_str(),
                         m_scene.sceneLoadError.c_str());
            m_scene.sceneLoadError.clear();
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        m_scripts.SetViewRect({ viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y });
        m_scripts.SetInputFocus(GetForegroundWindow() == GetWindow().GetHandle());
        m_scripts.Simulate(GetClock().GetDeltaTime());

        const float aspect = static_cast<float>(GetWindow().GetWidth()) /
                             static_cast<float>((std::max)(GetWindow().GetHeight(), 1u));
        m_scene.Render(aspect);
        m_scripts.DrawGui();
    }

    void OnPostProcess() override
    {
        if (!m_scene.HasScene() || m_scene.IsLoading())
        {
            // The loading UI draws directly over the cleared back buffer.
            GetRenderer().BindBackBuffer(GetRenderer().GetContext());
            return;
        }
        m_scene.PostProcess(GetWindow().GetWidth(), GetWindow().GetHeight(),
                            GetClock().GetDeltaTime(),
                            [this]() { GetRenderer().BindBackBuffer(GetRenderer().GetContext()); });
    }

private:
    // The startup scene failed to load, so there is nothing to play.
    void UpdateStartupFailure()
    {
        if (GetInput().IsKeyPressed('R'))
        {
            m_scene.sceneLoadError.clear();
            m_scene.RequestSceneLoad(m_startupScene);
        }
        if (GetInput().IsKeyPressed(VK_ESCAPE))
            PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
    }

    void DrawLoadingUI()
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

        const float progress = std::clamp(status.progress, 0.0f, 1.0f);
        const std::string& path = m_scene.IsLoading() ? m_scene.GetLoadingScenePath()
                                                      : m_scene.lastRequestedScenePath;
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

        // Blue (#00A0DE) with a dark shadow so the borderless overlay stays
        // legible over bright level backgrounds.
        constexpr ImU32 blue = IM_COL32(0, 160, 222, 255);
        constexpr ImU32 textShadow = IM_COL32(0, 18, 32, 230);
        const ImVec4 blueFloat(0.0f, 160.0f / 255.0f, 222.0f / 255.0f, 1.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto drawShadow = [&](const char* text) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            drawList->AddText(ImVec2(pos.x + 2.0f, pos.y + 2.0f), textShadow, text);
        };

        char levelText[256];
        sprintf_s(levelText, "// LOADING LEVEL: %s", FileName(path).c_str());
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
            sprintf_s(errorText, "%s  //  [R] RETRY   [ESC] QUIT", m_scene.sceneLoadError.c_str());
            drawShadow(errorText);
            ImGui::TextColored(ImVec4(1.0f, 0.18f, 0.08f, 1.0f), "%s", errorText);
        }

        const ImVec2 barMin(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - barHeight);
        const ImVec2 barMax(viewport->Pos.x + viewport->Size.x * progress,
                            viewport->Pos.y + viewport->Size.y);
        drawList->AddRectFilled(barMin, barMax, blue);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    SE::SceneRuntime  m_scene;
    SE::ScriptSystem  m_scripts;
    std::string       m_title;         // window title when no script set one
    std::string       m_startupScene;
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    const LaunchOptions options = ParseCommandLine();
    if (!options.error.empty())
    {
        ShowFatalError(options.error + "\nUsage: --project <dir-or-project.json> --scene <path>");
        return 1;
    }
    if (!SE::SetProjectRootCwd(options.project))
    {
        ShowFatalError("Could not select a project. Use --project <dir-or-project.json> to select a valid project.");
        return 1;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(SE::Project::k_fileName, ec))
    {
        ShowFatalError("No project.json in " + std::filesystem::current_path(ec).string() +
                       ".\nUse --project <dir-or-project.json> to select a valid project.");
        return 1;
    }
    // A broken project would run without its scripts or input: refuse instead.
    const SE::ProjectLoadResult loaded = SE::Project::Load();
    if (!loaded.Succeeded())
    {
        std::string message = "project.json has errors (run `FoxProject check` for details):\n";
        for (size_t i = 0; i < loaded.errors.size() && i < 10; ++i)
            message += "\n" + loaded.errors[i];
        if (loaded.errors.size() > 10)
            message += "\n...";
        ShowFatalError(message);
        return 1;
    }
    const SE::ProjectSettings& project = loaded.project;

    std::string startupScene = options.scene;
    if (startupScene.empty())
        startupScene = project.startupScene;
    if (startupScene.empty())
    {
        const std::vector<std::string> scenes = SE::Project::SceneList(project);
        if (!scenes.empty())
            startupScene = scenes.front();
    }
    if (startupScene.empty())
    {
        ShowFatalError("Nothing to play: project.json has no startupScene and Assets/Scenes has no scenes.");
        return 1;
    }

    SE::WindowDesc desc;
    desc.title  = Widen(project.window.title.empty() ? project.name : project.window.title);
    desc.width  = project.window.width;
    desc.height = project.window.height;

    Player player;
    if (!player.Initialize(desc))
        return 1;
    for (const std::string& warning : options.warnings)
        SE_LOG_WARN("%s", warning.c_str());
    if (!player.Setup(project, startupScene))
    {
        player.Shutdown();
        ShowFatalError("Could not start the game; see FoxEngine.log.");
        return 1;
    }
    player.Run();
    player.Shutdown();
    return 0;
}
