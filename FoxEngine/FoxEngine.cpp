// FoxEngine.cpp : Defines the entry point for the application.  
//  

#include "framework.h"  
#include "FoxEngine.h"
#include "Window.h"
#include "Timer.h"

int CALLBACK WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nCmdShow
)
{
	Timer timer;
	Window wnd(1920, 1080, L"FoxEngine Window", nCmdShow);

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
		const float c = sin(timer.Peek()) / 2.0f + 0.5f;
		wnd.Gfx().ClearBuffer(0.0f, 0.5f, 1.0f); // Clear the back buffer to blue
		wnd.Gfx().DrawTriangle(
			timer.Peek(),
			wnd.mouse.GetPosX() / 960.0f - 1.0f, // normalize x position to [-1, 1]
			-wnd.mouse.GetPosY() / 540.0f + 1.0f // normalize y position to [-1, 1] (inverted because screen coordinates have origin at top-left)
		); 
		wnd.Gfx().EndFrame(); // End the frame, which will present the back buffer to the front buffer
	}
	return msg.wParam;
}