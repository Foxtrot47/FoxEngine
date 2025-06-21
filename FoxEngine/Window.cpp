
#include "Window.h"

Window::WindowClass Window::WindowClass::wndClass;

Window::WindowClass::WindowClass() : hInstance(GetModuleHandle(NULL))
{
	// Register the window class
	WNDCLASSEX wc;
	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);				// Size of the structure
	wc.style = CS_HREDRAW | CS_VREDRAW;			// Redraw on resize
	wc.lpfnWndProc = HandleMsgSetup;			// Default window procedure
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

LRESULT CALLBACK Window::HandleMsgSetup(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// if the message is not WM_NCCREATE, forward it to the default window procedure
	if (msg != WM_NCCREATE)
		return DefWindowProc(hWnd, msg, wParam, lParam);

	// use create parameter passed in from CreateWindow to store window class pointer at WinAPI side
	// Extract pointer to window class from creation data
	const CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
	Window* const pWnd = static_cast<Window*>(pCreate->lpCreateParams);

	// Set the window pointer in the user data
	SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWnd));

	// Set message proc to normal handler now that setup is finished
	SetWindowLongPtr(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::HandleMsgThunk));

	// forward message to window class handler
	return pWnd->HandleMsg(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK Window::HandleMsgThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Retrieve pointer to window class from user data
	Window* const pWnd = reinterpret_cast<Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
	// Forward message to window class handler
	return pWnd->HandleMsg(hWnd, msg, wParam, lParam);
}

LRESULT Window::HandleMsg(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;

		// clear keystate when window looses focus to prevent input getting stuck
	case WM_KILLFOCUS:
		kbd.ClearState();
		break;

		// keyboard messages
	case WM_KEYDOWN:
		// Check if the key is already pressed or if autorepeat is enabled
		if (!(lParam & 0x40000000) || kbd.AutorepeatIsEnabled())
		{
			kbd.OnKeyPressed(static_cast<unsigned char>(wParam));
		}
		break;
	case WM_KEYUP:
		kbd.OnKeyReleased(static_cast<unsigned char>(wParam));
		break;
	case WM_CHAR:
		kbd.OnChar(static_cast<unsigned char>(wParam));
		break;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}
