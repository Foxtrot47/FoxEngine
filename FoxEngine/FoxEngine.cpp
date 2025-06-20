// FoxEngine.cpp : Defines the entry point for the application.  
//  

#include "framework.h"  
#include "FoxEngine.h"

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
		break;
	}
	return DefWindowProc(hwnd, message, wParam, lParam);
}

int CALLBACK WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nCmdShow
)
{
	const auto pClassName = L"FoxEngine";

	// Register the window class
	WNDCLASSEX wc;
	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);				// Size of the structure
	wc.style = CS_HREDRAW | CS_VREDRAW;			// Redraw on resize
	wc.lpfnWndProc = WindowProc;				// Default window procedure
	wc.hInstance = hInstance;					// Instance handle
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);	// Load default arrow cursor
	wc.lpszClassName = pClassName;

	RegisterClassEx(&wc);

	// Create the window
	HWND hWnd = CreateWindowEx(
		0,                              // Extended style
		pClassName,						// Class name
		pClassName,						// Window title
		WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU,            // Window style
		300,                            // X-position
		300,                            // Y-position
		1920,							// Width
		1080,							// Height
		NULL,                           // Parent window
		NULL,                           // Menu
		hInstance,                      // Instance handle
		NULL                            // Additional data
	);

	// Display the window
	ShowWindow(hWnd, nCmdShow);

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
	}
	return msg.wParam;
}