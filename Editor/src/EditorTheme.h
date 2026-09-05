#pragma once

struct ImFont;

namespace Editor {

// VSCode Dark+ palette with Fluent geometry and Segoe UI fonts.
// Call once after ImGui context creation, before the first frame.
void ApplyTheme();

// Standalone large Material Icons font for content-browser tiles
// (nullptr when the icon font is unavailable).
ImFont* LargeIconFont();

} // namespace Editor
