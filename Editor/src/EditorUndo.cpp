#include "EditorUndo.h"

namespace Editor {

void UndoStack::Push(Entry entry)
{
    m_entries.resize(m_cursor); // drop any redo tail
    m_entries.push_back(std::move(entry));
    m_cursor = m_entries.size();
}

void UndoStack::Undo()
{
    if (!CanUndo())
        return;
    --m_cursor;
    m_entries[m_cursor].undo();
}

void UndoStack::Redo()
{
    if (!CanRedo())
        return;
    m_entries[m_cursor].redo();
    ++m_cursor;
}

void UndoStack::Clear()
{
    m_entries.clear();
    m_cursor = 0;
}

} // namespace Editor
