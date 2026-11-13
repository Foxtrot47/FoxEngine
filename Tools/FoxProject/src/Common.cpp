#include "Common.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include "Engine/Core/Project.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace fs = std::filesystem;

namespace FoxProject {

namespace {

const char* SeverityName(Severity severity)
{
    return severity == Severity::Error ? "error" : "warning";
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(std::move(current));
            current.clear();
        }
        else if (c != '\r')
            current += c;
    }
    if (!current.empty() || lines.empty())
        lines.push_back(std::move(current));
    while (lines.size() > 1 && lines.back().empty())
        lines.pop_back();
    return lines;
}

std::string TrimLeft(const std::string& text)
{
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    return text.substr(start);
}

std::string Trim(const std::string& text)
{
    std::string result = TrimLeft(text);
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())))
        result.pop_back();
    return result;
}

struct LuaLocation
{
    std::string path;
    int line = 0;
    std::string message;
};

// Lua reports "<chunk>.lua:<line>: <message>".
bool ParseLuaLocation(const std::string& text, LuaLocation& out)
{
    const size_t extension = text.find(".lua:");
    if (extension == std::string::npos)
        return false;
    const size_t digitsStart = extension + 5;
    size_t position = digitsStart;
    while (position < text.size() && position - digitsStart < 9 &&
           std::isdigit(static_cast<unsigned char>(text[position])))
        ++position;
    if (position == digitsStart || position >= text.size() || text[position] != ':')
        return false;
    out.path = text.substr(0, extension + 4);
    out.line = std::atoi(text.substr(digitsStart, position - digitsStart).c_str());
    out.message = TrimLeft(text.substr(position + 1));
    return true;
}

// The first message line without its location, for duplicate detection.
std::string CoreMessage(const std::string& text)
{
    const std::string first = SplitLines(text).front();
    LuaLocation location;
    return Trim(ParseLuaLocation(first, location) ? location.message : first);
}

bool IsEngineSource(const std::string& file)
{
    return EndsWith(file, ".cpp") || EndsWith(file, ".h") || EndsWith(file, ".hpp");
}

void Emit(FILE* stream, const std::string& text)
{
    if (stream == stderr)
        std::fflush(stdout);
    std::fputs(text.c_str(), stream);
    std::fputc('\n', stream);
    if (stream == stderr)
        std::fflush(stderr);
}

} // namespace

std::string FormatDiagnostic(Severity severity, const std::string& defaultPath, int defaultLine,
                             const std::string& message)
{
    const std::vector<std::string> lines = SplitLines(message);
    std::string head;
    LuaLocation location;
    if (ParseLuaLocation(lines.front(), location))
        head = location.path + "(" + std::to_string(location.line) + "): " +
               SeverityName(severity) + ": " + location.message;
    else
    {
        head = defaultPath.empty() ? std::string("FoxProject") : defaultPath;
        if (defaultLine > 0)
            head += "(" + std::to_string(defaultLine) + ")";
        head += std::string(": ") + SeverityName(severity) + ": " + lines.front();
    }
    for (size_t i = 1; i < lines.size(); ++i)
        head += "\n    " + lines[i];
    return head;
}

void PrintDiagnostic(Severity severity, const std::string& path, const std::string& message, FILE* stream)
{
    Emit(stream, FormatDiagnostic(severity, path, 0, message));
}

int UsageError(const std::string& command, const std::string& message)
{
    std::fflush(stdout);
    std::fprintf(stderr, "FoxProject %s: %s\nRun 'FoxProject help' for usage.\n",
                 command.c_str(), message.c_str());
    return k_exitUsage;
}

// --- ConsoleLogSink ------------------------------------------------------------

ConsoleLogSink::ConsoleLogSink()
{
    m_id = SE::Logger::Get().AddSink([this](SE::LogLevel level, const char* file, int line, const char* message) {
        Write(level, file, line, message);
    });
}

ConsoleLogSink::~ConsoleLogSink()
{
    SE::Logger::Get().RemoveSink(m_id);
}

void ConsoleLogSink::BeginCapture()
{
    std::lock_guard lock(m_mutex);
    m_capturing = true;
    m_captured.clear();
}

std::vector<CapturedLog> ConsoleLogSink::EndCapture()
{
    std::lock_guard lock(m_mutex);
    m_capturing = false;
    return std::move(m_captured);
}

void ConsoleLogSink::Write(SE::LogLevel level, const char* file, int line, const char* message)
{
    const std::string source = file ? file : "";
    const std::string text = message ? message : "";
    const bool fromScript = !IsEngineSource(source);

    switch (level)
    {
    case SE::LogLevel::Debug:
    case SE::LogLevel::Info:
        if (fromScript)
            Emit(stdout, text);
        return;
    case SE::LogLevel::Warning:
        if (fromScript)
            Emit(stderr, FormatDiagnostic(Severity::Warning, source, line, text));
        return;
    case SE::LogLevel::Error:
    case SE::LogLevel::Fatal:
        break;
    }

    std::lock_guard lock(m_mutex);
    if (m_capturing)
        m_captured.push_back({ source, line, text });
    else
        Emit(stderr, FormatDiagnostic(Severity::Error, source, line, text));
}

size_t PrintCapturedErrors(const std::vector<CapturedLog>& captured,
                           const std::vector<std::string>& alreadyReported, FILE* stream)
{
    size_t printed = 0;
    for (const CapturedLog& entry : captured)
    {
        const std::string core = CoreMessage(entry.message);
        const bool duplicate = std::any_of(alreadyReported.begin(), alreadyReported.end(),
            [&](const std::string& reported) {
                const std::string other = CoreMessage(reported);
                return !core.empty() && !other.empty() &&
                       (core.find(other) != std::string::npos || other.find(core) != std::string::npos);
            });
        if (duplicate)
            continue;
        Emit(stream, FormatDiagnostic(Severity::Error, entry.file, entry.line, entry.message));
        ++printed;
    }
    return printed;
}

bool StartHeadlessScripts(SE::ScriptSystem& scripts, const SE::ProjectSettings& project)
{
    SE::ScriptHost host;
    host.projectName = project.name;
    host.inputActions = project.inputActions;
    host.scenes = SE::Project::SceneList(project);
    host.hotReload = false;
    if (scripts.Start(host))
        return true;
    PrintDiagnostic(Severity::Error, "FoxProject", "could not create the Lua VM", stderr);
    return false;
}

// --- Paths and files -------------------------------------------------------------

bool EndsWith(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const std::string& text, const std::string& prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

std::string ToLower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string NormalizePath(const std::string& path)
{
    return SE::LevelDependencyPlan::NormalizePath(path);
}

bool IsRegularFile(const std::string& path)
{
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec);
}

std::vector<std::string> ListFilesRecursive(const std::string& directory)
{
    std::vector<std::string> files;
    std::error_code ec;
    if (!fs::is_directory(directory, ec))
        return files;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code entryError;
        if (it->is_regular_file(entryError))
            files.push_back(NormalizePath(it->path().generic_string()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool IsTestScript(const std::string& path)
{
    return EndsWith(path, k_testSuffix);
}

std::string ModuleName(const std::string& scriptPath)
{
    const std::string prefix = std::string(k_scriptsDirectory) + "/";
    if (!StartsWith(scriptPath, prefix) || !EndsWith(scriptPath, ".lua"))
        return {};
    std::string module = scriptPath.substr(prefix.size(), scriptPath.size() - prefix.size() - 4);
    if (module.empty() || module.find('.') != std::string::npos)
        return {};
    std::replace(module.begin(), module.end(), '/', '.');
    return module;
}

bool ReadTextFile(const std::string& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& content, std::string* error)
{
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty())
        fs::create_directories(parent, ec);
    if (ec)
    {
        if (error) *error = "cannot create directory '" + parent.generic_string() + "': " + ec.message();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        if (error) *error = "cannot open for writing";
        return false;
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file)
    {
        if (error) *error = "write failed";
        return false;
    }
    return true;
}

ApiStubState CheckApiStubs(std::string* generated)
{
    std::string stubs = SE::ScriptSystem::GenerateApiStubs();
    std::string existing;
    const ApiStubState state = !ReadTextFile(SE::EnginePath(SE::ScriptSystem::k_apiStubPath).generic_string(), existing) ? ApiStubState::Missing
                             : existing == stubs ? ApiStubState::UpToDate
                                                 : ApiStubState::Stale;
    if (generated)
        *generated = std::move(stubs);
    return state;
}

} // namespace FoxProject
