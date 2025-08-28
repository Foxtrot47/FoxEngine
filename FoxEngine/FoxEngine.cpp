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
	FPVCamera cam(wnd.GetHandle(), wnd.Gfx(), wnd.kbd, wnd.mouse);

	wnd.Gfx().SetProjection(cam.GetProjectionMatrix());
	SceneManager scene(wnd.Gfx(), cam);

	bool shouldExit = false;
	// Enter main loop
	MSG msg = { 0 };
	while (true)
	{
		// Check for messages
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);

			if (msg.message == WM_QUIT)
			{
				shouldExit = true;
				break;
			}
		}
		if (shouldExit)
		{
			break;
		}
		auto deltaTime = timer.Mark();

		cam.HandleInput();

		wnd.Gfx().BeginFrame(0.0f, 0.0f, 0.0f); // Clear the back buffer to black
		wnd.Gfx().SetCamera(cam.GetViewMatrix());
		cam.Update(deltaTime);
		cam.Bind(wnd.Gfx());

		scene.Draw(wnd.Gfx());

		scene.DrawSceneGraph(wnd.Gfx());
		wnd.Gfx().EndFrame(); // End the frame, which will present the back buffer to the front buffer
	}
	return msg.wParam;
}