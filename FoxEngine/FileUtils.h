#pragma once
#include "framework.h"
#include <filesystem>

inline std::wstring GetExecutableDirectory()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);

	std::filesystem::path exePath(path);
	return exePath.parent_path().wstring(); // Strip the exe filename
}

inline std::wstring GetShaderPath(const std::wstring& shaderName)
{
	return GetExecutableDirectory() + L"\\" + shaderName;
}