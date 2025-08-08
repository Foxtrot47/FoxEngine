// FoxEngine.cpp : Defines the entry point for the application.  
//  

#include "FoxEngine.h"

int CALLBACK WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nCmdShow
)
{
	Timer timer;
	ImGuiManager imGuiManager;
	Window wnd(1920, 1080, L"FoxEngine Window", nCmdShow);
	Camera droneCam(wnd.Gfx());
	PointLight light(wnd.Gfx());

	std::mt19937 rng(std::random_device{}());
	std::uniform_real_distribution<float> angularDistribution(0.0f, 3.1415f * 2.0f);
	std::uniform_real_distribution<float> deltaDistribution(0.0f, 3.1415f * 2.0f);
	std::uniform_real_distribution<float> orbitalDistribution(0.0f, 3.1415f * 0.3f);
	std::uniform_real_distribution<float> radiusDistribution(6.0f, 20.0f);

	std::vector<std::unique_ptr<class Box>> boxes;

	for (auto index = 0; index < 80; index++)
	{
		boxes.push_back(
			std::make_unique<Box>(
				wnd.Gfx(),
				rng,
				angularDistribution,
				deltaDistribution,
				orbitalDistribution,
				radiusDistribution
			)
		);
	}
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

		if (wnd.kbd.KeyIsPressed(VK_SPACE))
		{
			wnd.Gfx().DisableImGui();
		}
		else
		{
			wnd.Gfx().EnableImGui();
		}

		wnd.Gfx().BeginFrame(0.0f, 0.0f, 0.0f); // Clear the back buffer to black
		wnd.Gfx().SetCamera(droneCam.GetViewMatrix());
		light.Bind(wnd.Gfx());
		droneCam.Bind(wnd.Gfx());

		for (auto& box : boxes)
		{
			box->Update(deltaTime);
			box->Draw(wnd.Gfx());
		}
		light.DrawSphere(wnd.Gfx());

		droneCam.CreateControlWindow();
		light.SpawnControlWindow();

		wnd.Gfx().EndFrame(); // End the frame, which will present the back buffer to the front buffer
	}
	return msg.wParam;
}