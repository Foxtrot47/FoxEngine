#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Editor {

// Linear undo/redo stack of closure pairs. Entries capture raw pointers into
// the loaded scene, so the stack must be cleared whenever a scene (re)loads.
class UndoStack
{
public:
    struct Entry
    {
        std::string           label;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    void Push(Entry entry);
    void Undo();
    void Redo();
    void Clear();

    bool CanUndo() const { return m_cursor > 0; }
    bool CanRedo() const { return m_cursor < m_entries.size(); }
    const char* UndoLabel() const { return CanUndo() ? m_entries[m_cursor - 1].label.c_str() : ""; }
    const char* RedoLabel() const { return CanRedo() ? m_entries[m_cursor].label.c_str() : ""; }

private:
    std::vector<Entry> m_entries;
    size_t             m_cursor = 0;
};

} // namespace Editor
