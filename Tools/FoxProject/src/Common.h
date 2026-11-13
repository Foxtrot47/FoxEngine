#pragma once
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "Engine/Core/Logger.h"

namespace SE {
class ScriptSystem;
struct ProjectSettings;
} // namespace SE

namespace FoxProject {

constexpr int k_exitSuccess = 0;
constexpr int k_exitFailure = 1;
constexpr int k_exitUsage   = 2;

constexpr const char* k_scriptsDirectory = "Assets/Scripts";
constexpr const char* k_scenesDirectory  = "Assets/Scenes";
constexpr const char* k_testSuffix       = "_test.lua";

enum class Severity { Warning, Error };

// One compiler-style line, `path(line): error: message`, or `path: error: message`
// without a line. A Lua location prefix in the message ("X.lua:12: msg") takes
// precedence over defaultPath; further message lines are indented.
std::string FormatDiagnostic(Severity severity, const std::string& defaultPath, int defaultLine,
                             const std::string& message);
void PrintDiagnostic(Severity severity, const std::string& path, const std::string& message,
                     FILE* stream = stdout);

// Prints `FoxProject <command>: message` and a usage hint to stderr; returns k_exitUsage.
int UsageError(const std::string& command, const std::string& message);

struct CapturedLog
{
    std::string file;
    int line = 0;
    std::string message;
};

// Routes engine log output to the console: script print/fox.log.info to
// stdout, script warnings and every error to stderr. Engine info and warning
// chatter (MeshCache, AssetManager) is dropped. While capturing, errors are
// buffered so a command can print them once next to the result that failed.
class ConsoleLogSink
{
public:
    ConsoleLogSink();
    ~ConsoleLogSink();
    ConsoleLogSink(const ConsoleLogSink&) = delete;
    ConsoleLogSink& operator=(const ConsoleLogSink&) = delete;

    void BeginCapture();
    std::vector<CapturedLog> EndCapture();

private:
    void Write(SE::LogLevel level, const char* file, int line, const char* message);

    std::mutex m_mutex;
    bool m_capturing = false;
    std::vector<CapturedLog> m_captured;
    SE::LogSinkId m_id = 0;
};

// Prints captured errors whose text is not already part of a reported
// message (the same failure returned and logged). Returns how many it printed.
size_t PrintCapturedErrors(const std::vector<CapturedLog>& captured,
                           const std::vector<std::string>& alreadyReported, FILE* stream);

struct ToolContext
{
    // The shell's working directory before the tool moved to the project root;
    // user-supplied output paths are relative to it.
    std::filesystem::path invocationDirectory;
    ConsoleLogSink* log = nullptr;
};

// A headless VM for validation and tests: project name and input actions, no
// window, input or scene, no hot reload. Prints an error when it fails.
bool StartHeadlessScripts(SE::ScriptSystem& scripts, const SE::ProjectSettings& project);

// --- Paths and files -------------------------------------------------------

bool EndsWith(const std::string& text, const std::string& suffix);
bool StartsWith(const std::string& text, const std::string& prefix);
std::string ToLower(std::string text);
std::string NormalizePath(const std::string& path);
bool IsRegularFile(const std::string& path);

// Every regular file below directory (project-relative, '/' separators, sorted).
std::vector<std::string> ListFilesRecursive(const std::string& directory);

bool IsTestScript(const std::string& path);
// Assets/Scripts/Game/Util.lua -> Game.Util; empty when the file cannot be
// required (outside Assets/Scripts, or a '.' in a path component).
std::string ModuleName(const std::string& scriptPath);

bool ReadTextFile(const std::string& path, std::string& out);
// Binary write (keeps '\n'); creates parent directories.
bool WriteTextFile(const std::string& path, const std::string& content, std::string* error);

// --- API stubs ---------------------------------------------------------------

enum class ApiStubState { UpToDate, Stale, Missing };
ApiStubState CheckApiStubs(std::string* generated = nullptr);

// --- Commands ------------------------------------------------------------------

struct CheckReport
{
    size_t errors = 0;
    size_t warnings = 0;
    // Scenes named by scene-typed script property overrides and defaults.
    std::vector<std::string> referencedScenes;
};

// Runs every check and prints diagnostics plus the summary line.
CheckReport RunCheck(ToolContext& context);

int CommandApi(ToolContext& context, const std::vector<std::string>& args);
int CommandTest(ToolContext& context, const std::vector<std::string>& args);
int CommandCheck(ToolContext& context, const std::vector<std::string>& args);
int CommandEval(ToolContext& context, const std::vector<std::string>& args);
int CommandNew(ToolContext& context, const std::vector<std::string>& args);
int CommandPublish(ToolContext& context, const std::vector<std::string>& args);
int CommandUpgrade(ToolContext& context, const std::vector<std::string>& args);

} // namespace FoxProject
