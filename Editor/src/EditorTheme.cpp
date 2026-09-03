#include "EditorTheme.h"
#include "EditorIcons.h"

#include <imgui.h>
#include <cstdint>
#include <filesystem>

namespace Editor {

namespace {

ImVec4 Hex(uint32_t rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8)  & 0xFF) / 255.0f,
                  ( rgb        & 0xFF) / 255.0f, a);
}

void ApplyColors(ImGuiStyle& style)
{
    ImVec4* c = style.Colors;

    // VSCode Dark+ reference values:
    //   editor bg #1E1E1E, sidebar #252526, titlebar #323233, input #3C3C3C,
    //   border #454545, text #CCCCCC, accent #007ACC, button #0E639C,
    //   list selection #094771, list hover #2A2D2E, tab inactive #2D2D2D.
    c[ImGuiCol_Text]                  = Hex(0xCCCCCC);
    c[ImGuiCol_TextDisabled]          = Hex(0x808080);
    c[ImGuiCol_WindowBg]              = Hex(0x252526);
    c[ImGuiCol_ChildBg]               = Hex(0x1E1E1E);
    c[ImGuiCol_PopupBg]               = Hex(0x252526);
    c[ImGuiCol_Border]                = Hex(0x454545);
    c[ImGuiCol_BorderShadow]          = Hex(0x000000, 0.0f);
    c[ImGuiCol_FrameBg]               = Hex(0x3C3C3C);
    c[ImGuiCol_FrameBgHovered]        = Hex(0x464647);
    c[ImGuiCol_FrameBgActive]         = Hex(0x4D4D4E);
    c[ImGuiCol_TitleBg]               = Hex(0x323233);
    c[ImGuiCol_TitleBgActive]         = Hex(0x3C3C3C);
    c[ImGuiCol_TitleBgCollapsed]      = Hex(0x323233);
    c[ImGuiCol_MenuBarBg]             = Hex(0x333333);
    c[ImGuiCol_ScrollbarBg]           = Hex(0x1E1E1E, 0.0f);
    c[ImGuiCol_ScrollbarGrab]         = Hex(0x797979, 0.40f);
    c[ImGuiCol_ScrollbarGrabHovered]  = Hex(0x797979, 0.60f);
    c[ImGuiCol_ScrollbarGrabActive]   = Hex(0x797979, 0.80f);
    c[ImGuiCol_CheckMark]             = Hex(0x1C97EA);
    c[ImGuiCol_SliderGrab]            = Hex(0x007ACC);
    c[ImGuiCol_SliderGrabActive]      = Hex(0x1C97EA);
    c[ImGuiCol_Button]                = Hex(0x0E639C);
    c[ImGuiCol_ButtonHovered]         = Hex(0x1177BB);
    c[ImGuiCol_ButtonActive]          = Hex(0x0A4F80);
    c[ImGuiCol_Header]                = Hex(0x094771);
    c[ImGuiCol_HeaderHovered]         = Hex(0x2A2D2E);
    c[ImGuiCol_HeaderActive]          = Hex(0x094771);
    c[ImGuiCol_Separator]             = Hex(0x454545);
    c[ImGuiCol_SeparatorHovered]      = Hex(0x007FD4);
    c[ImGuiCol_SeparatorActive]       = Hex(0x007FD4);
    c[ImGuiCol_ResizeGrip]            = Hex(0x007ACC, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = Hex(0x007ACC, 0.60f);
    c[ImGuiCol_ResizeGripActive]      = Hex(0x007ACC, 0.90f);
    c[ImGuiCol_Tab]                   = Hex(0x2D2D2D);
    c[ImGuiCol_TabHovered]            = Hex(0x3E3E40);
    c[ImGuiCol_TabSelected]           = Hex(0x1E1E1E);
    c[ImGuiCol_TabSelectedOverline]   = Hex(0x007ACC);
    c[ImGuiCol_TabDimmed]             = Hex(0x2D2D2D);
    c[ImGuiCol_TabDimmedSelected]     = Hex(0x252526);
    c[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x007ACC, 0.35f);
    c[ImGuiCol_DockingPreview]        = Hex(0x007ACC, 0.55f);
    c[ImGuiCol_DockingEmptyBg]        = Hex(0x1E1E1E);
    c[ImGuiCol_PlotLines]             = Hex(0x007ACC);
    c[ImGuiCol_PlotLinesHovered]      = Hex(0x1C97EA);
    c[ImGuiCol_PlotHistogram]         = Hex(0x1177BB);
    c[ImGuiCol_PlotHistogramHovered]  = Hex(0x1C97EA);
    c[ImGuiCol_TableHeaderBg]         = Hex(0x252526);
    c[ImGuiCol_TableBorderStrong]     = Hex(0x454545);
    c[ImGuiCol_TableBorderLight]      = Hex(0x333333);
    c[ImGuiCol_TableRowBg]            = Hex(0x000000, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = Hex(0xFFFFFF, 0.03f);
    c[ImGuiCol_TextSelectedBg]        = Hex(0x264F78);
    c[ImGuiCol_DragDropTarget]        = Hex(0x007FD4);
    c[ImGuiCol_NavCursor]             = Hex(0x007FD4);
    c[ImGuiCol_NavWindowingHighlight] = Hex(0x007FD4, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = Hex(0x000000, 0.50f);
    c[ImGuiCol_ModalWindowDimBg]      = Hex(0x000000, 0.50f);
}

// Fluent-style geometry: soft rounding on floating chrome, compact spacing.
void ApplyGeometry(ImGuiStyle& style)
{
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;
    style.TabBarOverlineSize = 2.0f; // VSCode active-tab accent line

    style.WindowPadding     = ImVec2(10.0f, 8.0f);
    style.FramePadding      = ImVec2(8.0f, 4.0f);
    style.CellPadding       = ImVec2(6.0f, 3.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
}

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    // Segoe UI (Windows system font), rasterized through FreeType. Falls back
    // to the bundled ProggyClean when the font file is unavailable.
    const char* segoe = "C:/Windows/Fonts/segoeui.ttf";
    if (std::filesystem::exists(segoe))
        io.Fonts->AddFontFromFileTTF(segoe, 17.0f);
    else
        io.Fonts->AddFontDefault();

    // Material Icons merged into the UI font (cwd is the project root).
    const char* icons = "ThirdParty/fonts/MaterialIcons-Regular.ttf";
    if (std::filesystem::exists(icons))
    {
        static const ImWchar ranges[] = { k_iconRangeMin, k_iconRangeMax, 0 };
        ImFontConfig cfg;
        cfg.MergeMode        = true;
        cfg.PixelSnapH       = true;
        cfg.GlyphMinAdvanceX = 17.0f;
        cfg.GlyphOffset      = ImVec2(0.0f, 3.0f); // sit icons on the text baseline
        io.Fonts->AddFontFromFileTTF(icons, 17.0f, &cfg, ranges);
    }
}

} // namespace

void ApplyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ApplyColors(style);
    ApplyGeometry(style);
    LoadFonts();
}

} // namespace Editor
