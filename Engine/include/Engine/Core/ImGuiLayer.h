#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace SE {

class ImGuiLayer
{
public:
    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Shutdown();
    // width/height: the swap-chain size, used while the window is minimized
    // (client rect 0x0) so ImGui keeps drawing into the back buffer.
    void BeginFrame(uint32_t width, uint32_t height);
    void EndFrame();

    // Passed to Window::SetMessageHook — keeps Window.cpp free of imgui headers.
    static LRESULT WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

} // namespace SE
