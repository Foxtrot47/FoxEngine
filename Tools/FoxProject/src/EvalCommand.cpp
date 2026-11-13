#include <cstdio>
#include "Common.h"
#include "Engine/Core/Project.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace FoxProject {

int CommandEval(ToolContext& context, const std::vector<std::string>& args)
{
    std::string code;
    for (const std::string& arg : args)
    {
        if (!code.empty())
            code += ' ';
        code += arg;
    }
    if (code.empty())
        return UsageError("eval", "expected Lua code, e.g. FoxProject eval \"vec3(1, 2, 3):length()\"");

    const SE::ProjectLoadResult project = SE::Project::Load();
    SE::ScriptSystem scripts;
    if (!StartHeadlessScripts(scripts, project.project))
        return k_exitFailure;

    context.log->BeginCapture();
    const SE::ScriptEvalResult result = scripts.Execute(code);
    const std::vector<CapturedLog> captured = context.log->EndCapture();

    std::vector<std::string> reported;
    if (result.ok)
        std::printf("%s\n", result.text.c_str());
    else
    {
        PrintDiagnostic(Severity::Error, "eval", result.text.empty() ? "evaluation failed" : result.text, stderr);
        reported.push_back(result.text);
    }
    // Errors the code logged (fox.log.error) without failing the evaluation.
    PrintCapturedErrors(captured, reported, stderr);
    scripts.Stop();
    return result.ok ? k_exitSuccess : k_exitFailure;
}

} // namespace FoxProject
