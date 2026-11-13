#include <cstdio>
#include "Common.h"
#include "Engine/Core/Project.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace FoxProject {

namespace {

void PrintIndented(const std::string& text)
{
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() || end < text.size())
            std::printf("    %s\n", line.c_str());
        start = end + 1;
    }
}

} // namespace

int CommandTest(ToolContext& context, const std::vector<std::string>& args)
{
    std::string filter;
    for (const std::string& arg : args)
    {
        if (!arg.empty() && arg[0] == '-')
            return UsageError("test", "unknown option '" + arg + "'");
        if (!filter.empty())
            return UsageError("test", "expected at most one filter");
        filter = arg;
    }

    std::vector<std::string> files;
    for (const std::string& file : ListFilesRecursive(k_scriptsDirectory))
        if (IsTestScript(file))
            files.push_back(file);

    // project.json problems are reported by `check`; tests still run with
    // whatever settings parsed.
    const SE::ProjectLoadResult project = SE::Project::Load();
    SE::ScriptSystem scripts;
    if (!StartHeadlessScripts(scripts, project.project))
        return k_exitFailure;

    context.log->BeginCapture();
    const std::vector<SE::ScriptTestResult> results = scripts.RunTests(files, filter);
    const std::vector<CapturedLog> captured = context.log->EndCapture();

    size_t passed = 0;
    size_t failed = 0;
    std::vector<std::string> failures;
    for (const SE::ScriptTestResult& result : results)
    {
        std::printf("%s %s: %s\n", result.passed ? "PASS" : "FAIL", result.file.c_str(), result.name.c_str());
        if (result.passed)
        {
            ++passed;
            continue;
        }
        ++failed;
        PrintIndented(result.message);
        failures.push_back(result.message);
    }
    // Errors outside any reported case (a test file that failed to load).
    const size_t scriptErrors = PrintCapturedErrors(captured, failures, stdout);
    scripts.Stop();

    if (scriptErrors > 0)
        std::printf("%zu passed, %zu failed, %zu script error(s)\n", passed, failed, scriptErrors);
    else
        std::printf("%zu passed, %zu failed\n", passed, failed);

    if (results.empty() && !filter.empty())
    {
        std::printf("test: no test matched '%s'\n", filter.c_str());
        return k_exitFailure;
    }
    return failed > 0 || scriptErrors > 0 ? k_exitFailure : k_exitSuccess;
}

} // namespace FoxProject
