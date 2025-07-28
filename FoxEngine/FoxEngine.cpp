// FoxEngine.cpp : Defines the entry point for the application.  
//  

#include "framework.h"  
#include "FoxEngine.h"
#include "Window.h"
#include "Timer.h"
#include "Box.h"
#include <memory>

int CALLBACK WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nCmdShow
)
{
	Timer timer;
	Window wnd(1920, 1080, L"FoxEngine Window", nCmdShow);

	std::mt19937 rng(std::random_device{}());
	std::uniform_real_distribution<float> angularDistribution(0.0f, 3.1415f * 2.0f);
	std::uniform_real_distribution<float> deltaDistribution(0.0f, 3.1415f * 2.0f);
	std::uniform_real_distribution<float> orbitalDistribution(0.0f, 3.1415f * 0.3f);
	std::uniform_real_distribution<float> radiusDistribution(6.0f, 20.0f);

	auto box = std::make_unique<Box>(
		wnd.Gfx(),
		rng,
		angularDistribution,
		deltaDistribution,
		orbitalDistribution,
		radiusDistribution
	);

	wnd.Gfx().SetProjection(DirectX::XMMatrixPerspectiveLH(1.0f, 9.0f / 16.0f, 0.5f, 40.0f));

	// Enter main loop
	MSG msg = { 0 };
	while (true)
	{
		// Check for messages
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);

			if (msg.message == WM_QUIT)
				break;
		}
		auto deltaTime = timer.Mark();
		wnd.Gfx().ClearBuffer(0.0f, 0.5f, 1.0f); // Clear the back buffer to blue

		box->Update(deltaTime);
		box->Draw(wnd.Gfx());

		wnd.Gfx().EndFrame(); // End the frame, which will present the back buffer to the front buffer
	}
	return msg.wParam;
}