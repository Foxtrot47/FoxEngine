#pragma once

// The engine's only sol2 include. sol2 is template-heavy and not warning
// clean under /W4 /WX, so its headers compile with warnings off here.
// Bindings report Lua errors by throwing ScriptError (see ScriptApi.h);
// sol2's call trampolines turn that into lua_error, so exceptions never
// leave the Scripting translation units.
#ifndef SOL_PRINT_ERRORS
#define SOL_PRINT_ERRORS 0
#endif

#pragma warning(push, 0)
#include <sol/sol.hpp>
#pragma warning(pop)
