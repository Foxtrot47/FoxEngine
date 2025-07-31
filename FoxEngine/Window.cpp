
#include "Window.h"
#include <imgui_impl_win32.h>

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

Window::Window(int width, int height, const LPCWSTR name, int nCmdShow) : width(width), height(height)
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

	// Init ImGui Win32 Impl
	ImGui_ImplWin32_Init(hWnd);

	pGfx = std::make_unique<Graphics>(hWnd);
}

Window::~Window()
{
	ImGui_ImplWin32_Shutdown();
	DestroyWindow(hWnd);
}

Graphics& Window::Gfx()
{
	return *pGfx;
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
	// commented out in imgui_impl_win32 to avoid conflicts 
	extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true; 

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
		// syskey commands need to be handled to track ALT key (VK_MENU) and Function keys
	case WM_SYSKEYDOWN:
		// Check if the key is already pressed or if autorepeat is enabled
		if (!(lParam & 0x40000000) || kbd.AutorepeatIsEnabled())
		{
			kbd.OnKeyPressed(static_cast<unsigned char>(wParam));
		}
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		kbd.OnKeyReleased(static_cast<unsigned char>(wParam));
		break;
	case WM_CHAR:
		kbd.OnChar(static_cast<unsigned char>(wParam));
		break;

		// mouse messages

	case WM_MOUSEMOVE:
		const POINTS pt = MAKEPOINTS(lParam);
		// in client region -> log move and log enter + capture mouse (if not previously in window)
		if (pt.x >= 0 && pt.x < width && pt.y >= 0 && pt.y < height)
		{
			mouse.OnMouseMove(pt.x, pt.y);
			if (!mouse.IsInWindow())
			{
				mouse.OnMouseEnter();
				SetCapture(hWnd); // Capture mouse input
			}
			else {
				if (wParam & (MK_LBUTTON | MK_RBUTTON))
				{
					// If mouse is pressed, log move event
					mouse.OnMouseMove(pt.x, pt.y);
				}
				else {
					ReleaseCapture(); // Release mouse capture if no buttons are pressed
					mouse.OnMouseLeave();
				}
			}
		}
		break;

	case WM_LBUTTONDOWN:
		const POINTS ptLeft = MAKEPOINTS(lParam);
		mouse.OnLeftButtonPressed(ptLeft.x, ptLeft.y);
		break;
	case WM_RBUTTONDOWN:
		const POINTS ptRight = MAKEPOINTS(lParam);
		mouse.OnRightButtonPressed(ptRight.x, ptRight.y);
		break;
	case WM_LBUTTONUP:
		const POINTS ptLeftUp = MAKEPOINTS(lParam);
		mouse.OnLeftButtonReleased(ptLeftUp.x, ptLeftUp.y);
		// release mouse if outside of window
		if (ptLeftUp.x < 0 || ptLeftUp.x >= width || ptLeftUp.y < 0 || ptLeftUp.y >= height)
		{
			ReleaseCapture();
			mouse.OnMouseLeave();
		}
		break;
	case WM_RBUTTONUP:
		const POINTS ptRightUp = MAKEPOINTS(lParam);
		mouse.OnRightButtonReleased(ptRightUp.x, ptRightUp.y);
		// release mouse if outside of window
		if (ptRightUp.x < 0 || ptRightUp.x >= width || ptRightUp.y < 0 || ptRightUp.y >= height)
		{
			ReleaseCapture();
			mouse.OnMouseLeave();
		}
		break;
	case WM_MOUSEWHEEL:
		const POINTS ptWheel = MAKEPOINTS(lParam);
		const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
		mouse.OnWheelDelta(ptWheel.x, ptWheel.y, delta);
		break;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}
