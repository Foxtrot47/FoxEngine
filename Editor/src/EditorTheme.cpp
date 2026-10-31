#include "EditorTheme.h"
#include "EditorIcons.h"
#include <imgui.h>
#include <cstdint>
#include <filesystem>

namespace Editor {
namespace {
ImVec4 Hex(uint32_t rgb, float alpha = 1.0f)
{
    return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                 (rgb & 255) / 255.0f, alpha);
}

void ApplyColors(ImGuiStyle& style)
{
    // Neutral controls, dark recessed inputs, blue only for selection/focus.
    // Avoid making every passive button look like a primary action.
    ImGui::StyleColorsDark(&style);
    auto* c = style.Colors;
    c[ImGuiCol_Text] = Hex(0xD8D8D8);
    c[ImGuiCol_TextDisabled] = Hex(0x858585);
    c[ImGuiCol_WindowBg] = Hex(0x202020);
    c[ImGuiCol_ChildBg] = Hex(0x1B1B1B);
    c[ImGuiCol_PopupBg] = Hex(0x252525);
    c[ImGuiCol_Border] = Hex(0x101010);
    c[ImGuiCol_FrameBg] = Hex(0x141414);
    c[ImGuiCol_FrameBgHovered] = Hex(0x303030);
    c[ImGuiCol_FrameBgActive] = Hex(0x393939);
    c[ImGuiCol_TitleBg] = Hex(0x171717);
    c[ImGuiCol_TitleBgActive] = Hex(0x282828);
    c[ImGuiCol_TitleBgCollapsed] = Hex(0x171717);
    c[ImGuiCol_MenuBarBg] = Hex(0x262626);
    c[ImGuiCol_ScrollbarBg] = Hex(0x181818);
    c[ImGuiCol_ScrollbarGrab] = Hex(0x454545);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x606060);
    c[ImGuiCol_ScrollbarGrabActive] = Hex(0x757575);
    c[ImGuiCol_Button] = Hex(0x303030);
    c[ImGuiCol_ButtonHovered] = Hex(0x454545);
    c[ImGuiCol_ButtonActive] = Hex(0x254F78);
    c[ImGuiCol_Header] = Hex(0x303030);
    c[ImGuiCol_HeaderHovered] = Hex(0x3C4854);
    c[ImGuiCol_HeaderActive] = Hex(0x285B88);
    c[ImGuiCol_CheckMark] = Hex(0x4DA6EF);
    c[ImGuiCol_SliderGrab] = Hex(0x4D85B0);
    c[ImGuiCol_SliderGrabActive] = Hex(0x65B4F4);
    c[ImGuiCol_Separator] = Hex(0x111111);
    c[ImGuiCol_SeparatorHovered] = Hex(0x448BC3);
    c[ImGuiCol_SeparatorActive] = Hex(0x65B4F4);
    c[ImGuiCol_ResizeGrip] = Hex(0x484848, 0.4f);
    c[ImGuiCol_ResizeGripHovered] = Hex(0x448BC3);
    c[ImGuiCol_ResizeGripActive] = Hex(0x65B4F4);
    c[ImGuiCol_Tab] = Hex(0x171717);
    c[ImGuiCol_TabHovered] = Hex(0x383838);
    c[ImGuiCol_TabSelected] = Hex(0x303030);
    c[ImGuiCol_TabSelectedOverline] = Hex(0x4DA6EF);
    c[ImGuiCol_TabDimmed] = Hex(0x181818);
    c[ImGuiCol_TabDimmedSelected] = Hex(0x282828);
    c[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x545454);
    c[ImGuiCol_DockingPreview] = Hex(0x448BC3, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = Hex(0x111111);
    c[ImGuiCol_TableHeaderBg] = Hex(0x292929);
    c[ImGuiCol_TableBorderStrong] = Hex(0x111111);
    c[ImGuiCol_TableBorderLight] = Hex(0x303030);
    c[ImGuiCol_TableRowBg] = Hex(0xFFFFFF, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextSelectedBg] = Hex(0x326A99, 0.65f);
    c[ImGuiCol_DragDropTarget] = Hex(0x65B4F4);
    c[ImGuiCol_NavCursor] = Hex(0x65B4F4);
    c[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.6f);
}

void ApplyGeometry(ImGuiStyle& style)
{
    style.WindowRounding = 2;
    style.ChildRounding = 0;
    style.FrameRounding = 2;
    style.PopupRounding = 3;
    style.ScrollbarRounding = 3;
    style.GrabRounding = 2;
    style.TabRounding = 3;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarOverlineSize = 2;
    style.WindowPadding = { 8, 6 };
    style.FramePadding = { 6, 3 };
    style.CellPadding = { 5, 2 };
    style.ItemSpacing = { 6, 4 };
    style.ItemInnerSpacing = { 5, 3 };
    style.IndentSpacing = 16;
    style.ScrollbarSize = 11;
    style.GrabMinSize = 9;
    style.WindowTitleAlign = { 0, 0.5f };
}

ImFont* s_largeIcons = nullptr;
void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    const char* font = "C:/Windows/Fonts/segoeui.ttf";
    if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(font, 16.0f);
    else io.Fonts->AddFontDefault();
    const char* icons = "ThirdParty/fonts/MaterialIcons-Regular.ttf";
    if (!std::filesystem::exists(icons)) return;
    static const ImWchar ranges[] = { k_iconRangeMin, k_iconRangeMax, 0 };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = 16.0f;
    cfg.GlyphOffset = { 0, 3 };
    io.Fonts->AddFontFromFileTTF(icons, 16.0f, &cfg, ranges);
    ImFontConfig big;
    big.PixelSnapH = true;
    s_largeIcons = io.Fonts->AddFontFromFileTTF(icons, 44.0f, &big, ranges);
}
}
ImFont* LargeIconFont() { return s_largeIcons; }
void ApplyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ApplyColors(style);
    ApplyGeometry(style);
    LoadFonts();
}
} // namespace Editor
