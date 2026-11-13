#include "Engine/Core/Logger.h"
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <share.h>

namespace SE {

static const char* k_LevelTag[] = { "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL" };

// Console colours: dark grey, white, yellow, red, bright red.
static const WORD k_LevelColour[] = { 8, 15, 14, 12, 12 };

Logger& Logger::Get()
{
    static Logger s_instance;
    return s_instance;
}

void Logger::Initialize(const char* logFilePath)
{
#ifdef SE_DEBUG
    if (AllocConsole())
    {
        freopen_s(reinterpret_cast<FILE**>(stdout), "CONOUT$", "w", stdout);
        freopen_s(reinterpret_cast<FILE**>(stderr), "CONOUT$", "w", stderr);
        m_hasConsole = true;

        // Give the console window a sensible title.
        SetConsoleTitleA("FoxEngine — Debug Console");

        // Widen the console buffer so long lines don't wrap awkwardly.
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hOut, &csbi))
        {
            csbi.dwSize.X = 200;
            SetConsoleScreenBufferSize(hOut, csbi.dwSize);
        }
    }
#endif

    if (logFilePath)
        m_logFile = _fsopen(logFilePath, "w", _SH_DENYNO);

    SE_LOG_INFO("Logger initialised");
}

void Logger::Shutdown()
{
    SE_LOG_INFO("Logger shutting down");

    if (m_logFile)
    {
        fclose(m_logFile);
        m_logFile = nullptr;
    }

#ifdef SE_DEBUG
    if (m_hasConsole)
    {
        FreeConsole();
        m_hasConsole = false;
    }
#endif
}

void Logger::Log(LogLevel level, const char* file, int line, const char* fmt, ...)
{
    // Format the user message; long messages (script tracebacks) spill to the heap.
    char stackMsg[1024];
    va_list args;
    va_start(args, fmt);
    va_list sizeArgs;
    va_copy(sizeArgs, args);
    const int needed = vsnprintf(stackMsg, sizeof(stackMsg), fmt, sizeArgs);
    va_end(sizeArgs);

    std::string heapMsg;
    const char* msg = stackMsg;
    if (needed >= static_cast<int>(sizeof(stackMsg)))
    {
        heapMsg.resize(static_cast<size_t>(needed));
        vsnprintf(heapMsg.data(), heapMsg.size() + 1, fmt, args);
        msg = heapMsg.c_str();
    }
    va_end(args);

    Write(level, file, line, msg);
}

void Logger::Write(LogLevel level, const char* file, int line, const char* msg)
{
    std::string lineText = "[";
    lineText += k_LevelTag[static_cast<int>(level)];
    lineText += "] ";
    lineText += file;
    lineText += "(" + std::to_string(line) + "): ";
    lineText += msg;
    lineText += '\n';

    std::lock_guard lock(m_mutex);

    // VS Output window.
    OutputDebugStringA(lineText.c_str());

    // Console with colour.
    if (m_hasConsole)
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hOut, k_LevelColour[static_cast<int>(level)]);
        fputs(lineText.c_str(), stdout);
        // Reset to white.
        SetConsoleTextAttribute(hOut, 15);
    }

    // Log file (no colour codes).
    if (m_logFile)
    {
        fputs(lineText.c_str(), m_logFile);
        fflush(m_logFile);
    }

    for (const auto& entry : m_sinks)
        entry.second(level, file, line, msg);
}

LogSinkId Logger::AddSink(LogSink sink)
{
    std::lock_guard lock(m_mutex);
    const LogSinkId id = m_nextSinkId++;
    m_sinks.emplace_back(id, std::move(sink));
    return id;
}

void Logger::RemoveSink(LogSinkId id)
{
    std::lock_guard lock(m_mutex);
    for (auto it = m_sinks.begin(); it != m_sinks.end(); ++it)
    {
        if (it->first == id)
        {
            m_sinks.erase(it);
            return;
        }
    }
}

} // namespace SE
