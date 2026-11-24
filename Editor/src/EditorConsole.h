#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <imgui.h>

#include "Engine/Core/Logger.h"

namespace SE {
class ScriptSystem;
}

namespace Editor {

// "Console" panel: every SE::Logger message (engine and script errors) in a
// bounded ring buffer with level/text filters, plus a Lua input line that
// evaluates code in the play session.
class Console
{
public:
    Console() = default;
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Registers / removes the logger sink. The sink captures this object.
    void Attach();
    void Detach();

    // scripts is the play session; the input line is enabled only while
    // playing (and the session runs).
    void Draw(SE::ScriptSystem& scripts, bool playing);

    struct LogEntry
    {
        uint64_t     seq;
        SE::LogLevel level;
        std::string  text; // prefix + text
    };
    // Retained lines with seq >= since, oldest first. seq counts every line
    // ever appended, from 0, so callers can poll for new output.
    std::vector<LogEntry> Snapshot(uint64_t since) const;
    uint64_t NextSeq() const;

private:
    struct Line
    {
        SE::LogLevel level = SE::LogLevel::Info;
        std::string  prefix;   // "file(line): " on a message's first line
        std::string  text;
        bool         echo = false;
    };

    static constexpr size_t k_maxLines = 2000;

    void Append(SE::LogLevel level, const char* file, int line, const char* message,
                bool echo = false);
    bool PassesFilter(const Line& line) const;
    void DrawToolbar();
    void DrawLines();
    void DrawInput(SE::ScriptSystem& scripts, bool enabled);
    void Run(SE::ScriptSystem& scripts, const std::string& code);
    static int InputCallback(ImGuiInputTextCallbackData* data);

    mutable std::mutex m_mutex; // the sink runs on logging threads
    std::deque<Line> m_lines;
    uint64_t         m_appended = 0;
    uint64_t         m_seenAppended = 0;
    SE::LogSinkId    m_sinkId = 0;

    bool            m_showInfo = true;
    bool            m_showWarnings = true;
    bool            m_showErrors = true;
    bool            m_autoScroll = true;
    ImGuiTextFilter m_filter;

    char                     m_input[1024] = {};
    std::vector<std::string> m_history;
    int                      m_historyPos = -1; // -1: editing a new line
};

} // namespace Editor
