#include "Engine/Core/Engine.h"

namespace SE {

bool Engine::Initialize(const WindowDesc& windowDesc)
{
    Logger::Get().Initialize("FoxEngine.log");
    m_clock.Initialize();

    if (!m_window.Open(windowDesc))
    {
        SE_LOG_FATAL("Failed to open window");
        return false;
    }

    if (!m_renderer.Initialize(m_window.GetHandle(),
                                m_window.GetWidth(),
                                m_window.GetHeight()))
    {
        SE_LOG_FATAL("Failed to initialise renderer");
        return false;
    }

    m_assets.Init(m_renderer.GetDevice(), m_renderer.GetContext());
    m_shaders.Init(m_renderer.GetDevice());

    m_window.SetMessageHook(ImGuiLayer::WndProcHandler);
    if (!m_imgui.Init(m_window.GetHandle(),
                      m_renderer.GetDevice(),
                      m_renderer.GetContext()))
    {
        SE_LOG_FATAL("Failed to initialise ImGui");
        return false;
    }

    m_window.SetInputHook(InputManager::WndProcHandler);
    if (!m_input.Init(m_window.GetHandle()))
    {
        SE_LOG_FATAL("Failed to initialise InputManager");
        return false;
    }

    SE_LOG_INFO("Engine initialised — %ux%u", windowDesc.width, windowDesc.height);
    return true;
}

void Engine::Run()
{
    SE_LOG_INFO("Entering main loop");

    float fpsTimer = 0.0f;

    while (true)
    {
        // Clear one-shot input states before pumping new messages.
        m_input.NewFrame();
        if (!m_window.PumpMessages()) break;

        if (m_window.IsSizeDirty())
        {
            m_renderer.Resize(m_window.GetWidth(), m_window.GetHeight());
            m_window.ClearSizeDirty();
        }

        m_clock.Tick();

        m_renderer.BeginFrame(0.1f, 0.15f, 0.25f);
        m_imgui.BeginFrame();
        OnUpdate();
        if (m_window.IsQuitRequested()) break;
        OnPostProcess();
        m_imgui.EndFrame();
        m_renderer.Present();

        fpsTimer += m_clock.GetDeltaTime();
        if (fpsTimer >= 1.0f)
        {
            if (m_titleStats)
            {
                wchar_t title[256];
                swprintf_s(title, L"%s  |  %.1f fps  |  %.2f ms",
                           m_titleBase.c_str(),
                           m_clock.GetFPS(),
                           m_clock.GetDeltaTime() * 1000.0f);
                SetWindowTextW(m_window.GetHandle(), title);
            }
            fpsTimer = 0.0f;
        }
    }

    SE_LOG_INFO("Exiting main loop — %llu frames, %.2fs total",
                m_clock.GetFrameCount(), m_clock.GetTotalTime());
}

void Engine::SetTitleBase(const std::wstring& base)
{
    m_titleBase = base;
    // With stats on, the run loop shows the new base within a second.
    if (!m_titleStats && m_window.IsOpen())
        SetWindowTextW(m_window.GetHandle(), m_titleBase.c_str());
}

void Engine::SetTitleStats(bool enabled)
{
    m_titleStats = enabled;
    if (m_window.IsOpen())
        SetWindowTextW(m_window.GetHandle(), m_titleBase.c_str());
}

void Engine::Shutdown()
{
    m_imgui.Shutdown();
    m_renderer.Shutdown();
    m_window.Close();
    Logger::Get().Shutdown();
}

} // namespace SE
