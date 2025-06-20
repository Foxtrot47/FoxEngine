
#include "Window.h"

Window::WindowClass Window::WindowClass::wndClass;

Window::WindowClass::WindowClass() : hInstance(GetModuleHandle(NULL))
{
	// Register the window class
	WNDCLASSEX wc;
	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);				// Size of the structure
	wc.style = CS_HREDRAW | CS_VREDRAW;			// Redraw on resize
	wc.lpfnWndProc = DefWindowProc;				// Default window procedure
	wc.hInstance = GetInstance();				// Instance handle
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);	// Load default arrow cursor
	wc.lpszClassName = GetName();

	RegisterClassEx(&wc);
}

Window::WindowClass::~WindowClass()
{
	UnregisterClass(wndClassName, GetInstance());
}

const LPCWSTR Window::WindowClass::GetName()
{
	return wndClassName;
}

HINSTANCE Window::WindowClass::GetInstance()
{
	return wndClass.hInstance;
}

Window::Window(int width, int height, const LPCWSTR name, int nCmdShow)
{
	// Calculate window size based on desired client region size
	RECT wr{};
	wr.left = 100;
	wr.right = width + wr.left;
	wr.top = 100;
	wr.bottom = height + wr.top;
	AdjustWindowRect(&wr, WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU, FALSE);


	// Create the window
	hWnd = CreateWindow(
		WindowClass::GetName(),							// Class name
		name,											// Window title
		WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU,		// Window style
		CW_USEDEFAULT,									// X-position
		CW_USEDEFAULT,									// Y-position
		wr.right - wr.left,								// Width
		wr.bottom - wr.top,								// Height
		NULL,											// Parent window
		NULL,											// Menu
		WindowClass::GetInstance(),						// Instance handle
		this											// Additional data
	);

	// Display the window
	ShowWindow(hWnd, nCmdShow);
}

Window::~Window()
{
	DestroyWindow(hWnd);
}
