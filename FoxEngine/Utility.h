#pragma once
#include <string>
#include <vector>
#include <memory>
#include "framework.h"

class Utility
{
public:
	static std::string convertToUTF8(const std::wstring& wstr);
	static std::wstring convertToUTF16(const std::string& str);
};

