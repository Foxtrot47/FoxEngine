#include <cstdio>
#include "Common.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace FoxProject {

int CommandApi(ToolContext& /*context*/, const std::vector<std::string>& args)
{
    bool checkOnly = false;
    for (const std::string& arg : args)
    {
        if (arg == "--check")
            checkOnly = true;
        else
            return UsageError("api", "unexpected argument '" + arg + "'");
    }

    const char* path = SE::ScriptSystem::k_apiStubPath;
    std::string stubs;
    const ApiStubState state = CheckApiStubs(&stubs);
    if (state == ApiStubState::UpToDate)
    {
        std::printf("api: %s is up to date\n", path);
        return k_exitSuccess;
    }
    if (checkOnly)
    {
        PrintDiagnostic(Severity::Error, path,
                        state == ApiStubState::Missing ? "API stubs are missing; run 'FoxProject api'"
                                                       : "API stubs are stale; run 'FoxProject api'");
        return k_exitFailure;
    }

    std::string error;
    if (!WriteTextFile(path, stubs, &error))
    {
        PrintDiagnostic(Severity::Error, path, error);
        return k_exitFailure;
    }
    std::printf("api: wrote %s (%zu bytes)\n", path, stubs.size());
    return k_exitSuccess;
}

} // namespace FoxProject
