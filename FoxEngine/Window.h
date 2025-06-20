#pragma once
#include "framework.h"

class Window
{
private:
	// Singleton manages registration/cleanup of window class
	class WindowClass
	{
	public:
		static const LPCWSTR GetName();
		static HINSTANCE GetInstance();
	private:
		WindowClass();
		~WindowClass();
		WindowClass(const WindowClass&) = delete;				// Remove copy constructor
		WindowClass& operator=(const WindowClass&) = delete;	// Remove assignment operator
		static constexpr const LPCWSTR wndClassName = L"FoxEngineWindowClass";
		static WindowClass wndClass;
		HINSTANCE hInstance;
	};
public:
	Window(int width, int height, const LPCWSTR name, int nCmdShow);
	~Window();
	Window(const Window&) = delete;				// Remove copy constructor
	Window& operator=(const Window&) = delete;	// Remove assignment operator
private:
	static LRESULT CALLBACK HandleMsgSetup(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);	// Initial message setup
	static LRESULT CALLBACK HandleMsgThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);	// Thunk to call member function
	LRESULT HandleMsg(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);						// Handle messages for this window
private:
	int width;
	int height;
	HWND hWnd;
};

