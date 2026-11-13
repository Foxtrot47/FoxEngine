#include <cstdio>
#include <string>
#include <system_error>
#include <vector>
#include "Common.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace {

void PrintUsage()
{
    std::printf(
        "Usage: FoxProject <command> [options]\n"
        "\n"
        "Project tool for FoxEngine games (project.json at the project root).\n"
        "Diagnostics read 'path(line): error: message'; exit codes: 0 success,\n"
        "1 failures, 2 usage errors.\n"
        "\n"
        "Commands:\n"
        "  api [--check]          write the LuaLS API reference %s;\n"
        "                         --check fails when it is missing or stale\n"
        "  test [filter]          run Assets/Scripts/**/*_test.lua; filter matches\n"
        "                         a substring of 'file: case name'\n"
        "  check                  validate project.json, every script and every scene\n"
        "                         (script property overrides included)\n"
        "  eval <lua code...>     evaluate Lua in a headless session and print the result\n"
        "  new <Name> [--dir <subdir>]\n"
        "                         create Assets/Scripts[/<subdir>]/<Name>.lua and <Name>_test.lua\n"
        "  publish [--out <dir>] [--clean]\n"
        "                         check, then copy a standalone build to\n"
        "                         out/Publish/<ProjectName>/ (incremental unless --clean)\n"
        "  help                   show this text\n",
        SE::ScriptSystem::k_apiStubPath);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h")
    {
        PrintUsage();
        return FoxProject::k_exitSuccess;
    }
    const std::string command = args[0];
    args.erase(args.begin());

    using Handler = int (*)(FoxProject::ToolContext&, const std::vector<std::string>&);
    struct Command
    {
        const char* name;
        Handler handler;
    };
    static const Command k_commands[] = {
        { "api", &FoxProject::CommandApi },
        { "test", &FoxProject::CommandTest },
        { "check", &FoxProject::CommandCheck },
        { "eval", &FoxProject::CommandEval },
        { "new", &FoxProject::CommandNew },
        { "publish", &FoxProject::CommandPublish },
    };
    Handler handler = nullptr;
    for (const Command& candidate : k_commands)
        if (command == candidate.name)
            handler = candidate.handler;
    if (!handler)
    {
        std::fprintf(stderr, "FoxProject: unknown command '%s'\n\n", command.c_str());
        PrintUsage();
        return FoxProject::k_exitUsage;
    }

    FoxProject::ToolContext context;
    std::error_code ec;
    context.invocationDirectory = std::filesystem::current_path(ec);
    if (!SE::SetProjectRootCwd())
    {
        std::fprintf(stderr, "FoxProject: project root (Engine/ + Assets/) not found above the executable\n");
        return FoxProject::k_exitFailure;
    }

    FoxProject::ConsoleLogSink logSink;
    context.log = &logSink;
    const int result = handler(context, args);
    std::fflush(stdout);
    return result;
}
