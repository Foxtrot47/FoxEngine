#include "EditorConsole.h"

#include "Engine/Scripting/ScriptSystem.h"
#include <cstdio>
#include <utility>

namespace Editor {

namespace {

constexpr ImVec4 k_warningColor = { 1.0f, 0.85f, 0.3f, 1.0f };
constexpr ImVec4 k_errorColor   = { 1.0f, 0.4f, 0.4f, 1.0f };
constexpr ImVec4 k_echoColor    = { 0.55f, 0.75f, 1.0f, 1.0f };

bool IsError(SE::LogLevel level)
{
    return level == SE::LogLevel::Error || level == SE::LogLevel::Fatal;
}

} // namespace

Console::~Console()
{
    Detach();
}

void Console::Attach()
{
    if (m_sinkId != 0)
        return;
    m_sinkId = SE::Logger::Get().AddSink(
        [this](SE::LogLevel level, const char* file, int line, const char* message) {
            Append(level, file, line, message);
        });
}

void Console::Detach()
{
    if (m_sinkId == 0)
        return;
    SE::Logger::Get().RemoveSink(m_sinkId);
    m_sinkId = 0;
}

// Multi-line messages (Lua tracebacks) are split so every stored line has
// the same height, which keeps the list clipper exact.
void Console::Append(SE::LogLevel level, const char* file, int line, const char* message,
                     bool echo)
{
    std::string prefix;
    if (file && file[0] != '\0')
    {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s(%d): ", file, line);
        prefix = buf;
    }
    const std::string text = message ? message : "";

    std::lock_guard lock(m_mutex);
    size_t start = 0;
    bool first = true;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string part = text.substr(start, end - start);
        if (!part.empty() && part.back() == '\r')
            part.pop_back();
        // Drop a trailing empty line from messages that end in '\n'.
        if (!(end == text.size() && part.empty() && !first))
        {
            m_lines.push_back({ level, first ? prefix : std::string(), std::move(part), echo });
            ++m_appended;
            if (m_lines.size() > k_maxLines)
                m_lines.pop_front();
        }
        first = false;
        start = end + 1;
    }
}

std::vector<Console::LogEntry> Console::Snapshot(uint64_t since) const
{
    std::lock_guard lock(m_mutex);
    std::vector<LogEntry> out;
    const uint64_t first = m_appended - m_lines.size();
    for (size_t i = since > first ? size_t(since - first) : 0; i < m_lines.size(); ++i)
    {
        const Line& line = m_lines[i];
        out.push_back({ first + i, line.level, line.prefix + line.text });
    }
    return out;
}

uint64_t Console::NextSeq() const
{
    std::lock_guard lock(m_mutex);
    return m_appended;
}

bool Console::PassesFilter(const Line& line) const
{
    if (IsError(line.level))
    {
        if (!m_showErrors) return false;
    }
    else if (line.level == SE::LogLevel::Warning)
    {
        if (!m_showWarnings) return false;
    }
    else if (!m_showInfo)
        return false;
    if (!m_filter.IsActive())
        return true;
    return m_filter.PassFilter(line.text.c_str()) || m_filter.PassFilter(line.prefix.c_str());
}

void Console::DrawToolbar()
{
    if (ImGui::Button("Clear"))
    {
        std::lock_guard lock(m_mutex);
        m_lines.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Info", &m_showInfo);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, k_warningColor);
    ImGui::Checkbox("Warnings", &m_showWarnings);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, k_errorColor);
    ImGui::Checkbox("Errors", &m_showErrors);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter", "Filter messages...", m_filter.InputBuf,
                                 IM_ARRAYSIZE(m_filter.InputBuf)))
        m_filter.Build();
}

void Console::DrawLines()
{
    const float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (!ImGui::BeginChild("##lines", { 0.0f, -footer }, ImGuiChildFlags_None,
                           ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::EndChild();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.0f, 1.0f });
    {
        std::lock_guard lock(m_mutex);
        std::vector<const Line*> visible;
        visible.reserve(m_lines.size());
        for (const Line& line : m_lines)
            if (PassesFilter(line))
                visible.push_back(&line);

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Line& line = *visible[static_cast<size_t>(i)];
                if (!line.prefix.empty())
                {
                    ImGui::TextDisabled("%s", line.prefix.c_str());
                    ImGui::SameLine(0.0f, 0.0f);
                }
                if (line.echo)
                    ImGui::TextColored(k_echoColor, "%s", line.text.c_str());
                else if (IsError(line.level))
                    ImGui::TextColored(k_errorColor, "%s", line.text.c_str());
                else if (line.level == SE::LogLevel::Warning)
                    ImGui::TextColored(k_warningColor, "%s", line.text.c_str());
                else
                    ImGui::TextUnformatted(line.text.c_str());
            }
        }
        clipper.End();
    }
    ImGui::PopStyleVar();

    // Follow new output only while the view is already at the bottom, so
    // scrolling up to read history is not yanked away.
    const bool grew = m_appended != m_seenAppended;
    m_seenAppended = m_appended;
    if (m_autoScroll && grew && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

int Console::InputCallback(ImGuiInputTextCallbackData* data)
{
    auto* self = static_cast<Console*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->m_history.empty())
        return 0;
    const int count = static_cast<int>(self->m_history.size());
    const int previous = self->m_historyPos;
    if (data->EventKey == ImGuiKey_UpArrow)
        self->m_historyPos = previous < 0 ? count - 1 : (previous > 0 ? previous - 1 : 0);
    else if (data->EventKey == ImGuiKey_DownArrow && previous >= 0)
        self->m_historyPos = previous + 1 < count ? previous + 1 : -1;
    if (previous == self->m_historyPos)
        return 0;
    data->DeleteChars(0, data->BufTextLen);
    if (self->m_historyPos >= 0)
        data->InsertChars(0, self->m_history[static_cast<size_t>(self->m_historyPos)].c_str());
    return 0;
}

void Console::Run(SE::ScriptSystem& scripts, const std::string& code)
{
    if (m_history.empty() || m_history.back() != code)
        m_history.push_back(code);
    m_historyPos = -1;
    Append(SE::LogLevel::Info, nullptr, 0, ("> " + code).c_str(), true);
    // Not under m_mutex: evaluation can log (print, errors) through the sink.
    const SE::ScriptEvalResult result = scripts.Execute(code);
    if (!result.text.empty() || !result.ok)
        Append(result.ok ? SE::LogLevel::Info : SE::LogLevel::Error, nullptr, 0,
               result.text.empty() ? "error" : result.text.c_str());
}

void Console::DrawInput(SE::ScriptSystem& scripts, bool enabled)
{
    ImGui::Separator();
    ImGui::BeginDisabled(!enabled);
    ImGui::SetNextItemWidth(-FLT_MIN);
    // Keep the line active after Enter. Refocusing it takes a frame or two,
    // and keys typed in between would be dropped.
    ImGuiIO& io = ImGui::GetIO();
    const bool keepActive = io.ConfigInputTextEnterKeepActive;
    io.ConfigInputTextEnterKeepActive = true;
    const bool entered = ImGui::InputTextWithHint(
        "##lua", enabled ? "Lua (Enter to run, Up/Down for history)"
                         : "Press Play (F5) to run Lua in the game session",
        m_input, sizeof(m_input),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
        &Console::InputCallback, this);
    io.ConfigInputTextEnterKeepActive = keepActive;
    ImGui::EndDisabled();
    if (!entered || !enabled)
        return;
    const std::string code = m_input;
    m_input[0] = '\0';
    if (code.find_first_not_of(" \t") != std::string::npos)
        Run(scripts, code);
}

void Console::Draw(SE::ScriptSystem& scripts, bool playing)
{
    if (!ImGui::Begin("Console"))
    {
        ImGui::End();
        return;
    }
    DrawToolbar();
    ImGui::Separator();
    DrawLines();
    DrawInput(scripts, playing && scripts.IsRunning());
    ImGui::End();
}

} // namespace Editor
