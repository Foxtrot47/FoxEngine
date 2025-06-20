// FoxEngine.cpp : Defines the entry point for the application.  
//  

#include "framework.h"  
#include "FoxEngine.h"  

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
	wc.lpfnWndProc = DefWindowProc;				// Default window procedure
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

	while (true) {}
	return 0;
}