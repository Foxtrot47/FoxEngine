#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>
#include <imgui.h>
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"

namespace SE::Scripting {

namespace {

void RequireGui(const ScriptContext& ctx, const char* function)
{
    if (!ctx.inGui)
        Raise("fox.ui.%s can only be called from on_gui", function);
}

ImVec2 ViewPoint(const ScriptContext& ctx, double x, double y)
{
    return ImVec2(ctx.view.x + static_cast<float>(x), ctx.view.y + static_cast<float>(y));
}

bool OptBool(const sol::table& options, const char* key)
{
    return options.get_or(key, false);
}

} // namespace

void BindUi(ScriptApi& api, ScriptContext& ctx)
{
    ApiDocs::Block& windowOptions = api.DocClass("fox.WindowOptions", "Options for fox.ui.begin_window.");
    windowOptions.fields = {
        { "x", "number?", "Left edge relative to the game view (fixed every frame)." },
        { "y", "number?", "Top edge relative to the game view." },
        { "width", "number?", "Fixed width (0 or nil: automatic)." },
        { "height", "number?", "Fixed height (0 or nil: automatic)." },
        { "center", "boolean?", "Centre the window in the game view." },
        { "no_decoration", "boolean?", "No title bar, resize grip or scrollbars; not movable." },
        { "no_background", "boolean?", "Transparent window background." },
        { "auto_resize", "boolean?", "Resize to fit the contents every frame." },
        { "no_inputs", "boolean?", "Click-through window (overlays)." },
    };

    api.Module("ui",
        "Immediate-mode UI (Dear ImGui), valid only inside on_gui. Positions are\n"
        "pixels relative to the game view. Widgets return their new value first:\n"
        "`self.volume = fox.ui.slider_float(\"Volume\", self.volume, 0, 1)`.\n"
        "Colours are integers from fox.ui.rgba. Windows are never docked or saved;\n"
        "a script error or missing end_window is recovered and logged.")
        .Function("begin_window", "(id: string, options?: fox.WindowOptions) -> boolean",
            "Begins a top-level window; always pair with end_window, even when this\n"
            "returns false (collapsed). The id is also the title of decorated windows.",
            [&ctx](const std::string& id, sol::optional<sol::table> options) {
                RequireGui(ctx, "begin_window");
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoCollapse;
                if (options)
                {
                    const sol::table& o = *options;
                    const sol::optional<double> x = o["x"], y = o["y"];
                    const double width = o.get_or("width", 0.0), height = o.get_or("height", 0.0);
                    if (OptBool(o, "center"))
                        ImGui::SetNextWindowPos(ViewPoint(ctx, ctx.view.width * 0.5, ctx.view.height * 0.5),
                                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    else if (x || y)
                        ImGui::SetNextWindowPos(ViewPoint(ctx, x.value_or(0.0), y.value_or(0.0)), ImGuiCond_Always);
                    if (width > 0.0 || height > 0.0)
                        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), static_cast<float>(height)),
                                                 ImGuiCond_Always);
                    if (OptBool(o, "no_decoration")) flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove;
                    if (OptBool(o, "no_background")) flags |= ImGuiWindowFlags_NoBackground;
                    if (OptBool(o, "auto_resize"))   flags |= ImGuiWindowFlags_AlwaysAutoResize;
                    if (OptBool(o, "no_inputs"))     flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
                }
                const bool visible = ImGui::Begin(id.c_str(), nullptr, flags);
                ++ctx.uiWindowDepth;
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                    ctx.uiWantsMouseNext = true;
                return visible;
            })
        .Function("end_window", "() -> nil", "Ends the window started by begin_window.",
            [&ctx] {
                RequireGui(ctx, "end_window");
                if (ctx.uiWindowDepth <= 0)
                    Raise("fox.ui.end_window called without a matching begin_window");
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    if (ImGui::IsAnyItemActive())
                        ctx.uiWantsMouseNext = true;
                    if (ImGui::GetIO().WantTextInput)
                        ctx.uiWantsKeyboardNext = true;
                }
                ImGui::End();
                --ctx.uiWindowDepth;
            })
        .Function("text", "(text: string) -> nil", "A line of text.",
            [&ctx](const std::string& text) {
                RequireGui(ctx, "text");
                ImGui::TextUnformatted(text.c_str());
            })
        .Function("text_colored", "(color: integer, text: string) -> nil", "Text in a colour from fox.ui.rgba.",
            [&ctx](lua_Integer color, const std::string& text) {
                RequireGui(ctx, "text_colored");
                ImGui::PushStyleColor(ImGuiCol_Text, static_cast<ImU32>(color));
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopStyleColor();
            })
        .Function("button", "(label: string, width?: number, height?: number) -> boolean",
            "A button; true on the frame it is clicked. width -1 fills the line.",
            [&ctx](const std::string& label, sol::optional<double> width, sol::optional<double> height) {
                RequireGui(ctx, "button");
                return ImGui::Button(label.c_str(), ImVec2(static_cast<float>(width.value_or(0.0)),
                                                           static_cast<float>(height.value_or(0.0))));
            })
        .Function("checkbox", "(label: string, value: boolean) -> boolean, boolean",
            "Returns the new value and whether it changed this frame.",
            [&ctx](const std::string& label, bool value) {
                RequireGui(ctx, "checkbox");
                const bool changed = ImGui::Checkbox(label.c_str(), &value);
                return std::make_tuple(value, changed);
            })
        .Function("slider_float", "(label: string, value: number, min: number, max: number, format?: string) -> number, boolean",
            "Returns the new value and whether it changed. format is printf-style (\"%.2f\").",
            [&ctx](const std::string& label, double value, double min, double max, sol::optional<std::string> format) {
                RequireGui(ctx, "slider_float");
                float v = static_cast<float>(value);
                const bool changed = ImGui::SliderFloat(label.c_str(), &v, static_cast<float>(min),
                                                        static_cast<float>(max), format ? format->c_str() : "%.3f");
                return std::make_tuple(changed ? static_cast<double>(v) : value, changed);
            })
        .Function("slider_int", "(label: string, value: integer, min: integer, max: integer) -> integer, boolean",
            "Returns the new value and whether it changed.",
            [&ctx](const std::string& label, lua_Integer value, lua_Integer min, lua_Integer max) {
                RequireGui(ctx, "slider_int");
                int v = static_cast<int>(value);
                const bool changed = ImGui::SliderInt(label.c_str(), &v, static_cast<int>(min), static_cast<int>(max));
                return std::make_tuple(static_cast<lua_Integer>(v), changed);
            })
        .Function("combo", "(label: string, index: integer, items: string[]) -> integer, boolean",
            "Drop-down over items; index is 1-based. Returns the new index and whether it changed.",
            [&ctx](const std::string& label, lua_Integer index, sol::table items) {
                RequireGui(ctx, "combo");
                std::vector<std::string> names;
                for (size_t i = 1; i <= items.size(); ++i)
                    names.push_back(items.get_or(i, std::string()));
                const int count = static_cast<int>(names.size());
                int current = static_cast<int>(index) - 1;
                const char* preview = current >= 0 && current < count ? names[current].c_str() : "";
                bool changed = false;
                if (ImGui::BeginCombo(label.c_str(), preview))
                {
                    for (int i = 0; i < count; ++i)
                    {
                        ImGui::PushID(i);
                        if (ImGui::Selectable(names[i].c_str(), i == current) && i != current)
                        {
                            current = i;
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                return std::make_tuple(static_cast<lua_Integer>(current + 1), changed);
            })
        .Function("selectable", "(label: string, selected?: boolean) -> boolean",
            "A selectable row; true on the frame it is clicked.",
            [&ctx](const std::string& label, sol::optional<bool> selected) {
                RequireGui(ctx, "selectable");
                return ImGui::Selectable(label.c_str(), selected.value_or(false));
            })
        .Function("input_text", "(label: string, text: string) -> string, boolean",
            "Single-line text field (up to 1023 bytes). Returns the text and whether it changed.",
            [&ctx](const std::string& label, const std::string& text) {
                RequireGui(ctx, "input_text");
                char buffer[1024];
                const size_t length = std::min(text.size(), sizeof(buffer) - 1);
                text.copy(buffer, length);
                buffer[length] = '\0';
                const bool changed = ImGui::InputText(label.c_str(), buffer, sizeof(buffer));
                return std::make_tuple(std::string(buffer), changed);
            })
        .Function("separator", "() -> nil", "A horizontal line.",
            [&ctx] { RequireGui(ctx, "separator"); ImGui::Separator(); })
        .Function("separator_text", "(text: string) -> nil", "A horizontal line with a caption.",
            [&ctx](const std::string& text) { RequireGui(ctx, "separator_text"); ImGui::SeparatorText(text.c_str()); })
        .Function("spacing", "() -> nil", "Vertical spacing.",
            [&ctx] { RequireGui(ctx, "spacing"); ImGui::Spacing(); })
        .Function("same_line", "(offset?: number, spacing?: number) -> nil", "Places the next widget on the same line.",
            [&ctx](sol::optional<double> offset, sol::optional<double> spacing) {
                RequireGui(ctx, "same_line");
                ImGui::SameLine(static_cast<float>(offset.value_or(0.0)), static_cast<float>(spacing.value_or(-1.0)));
            })
        .Function("content_width", "() -> number", "Remaining content width of the current window.",
            [&ctx] { RequireGui(ctx, "content_width"); return static_cast<double>(ImGui::GetContentRegionAvail().x); })
        .Function("calc_text_width", "(text: string) -> number", "Width of text in the current font.",
            [&ctx](const std::string& text) {
                RequireGui(ctx, "calc_text_width");
                return static_cast<double>(ImGui::CalcTextSize(text.c_str()).x);
            })
        .Function("set_cursor_x", "(x: number) -> nil", "Moves the layout cursor within the current window.",
            [&ctx](double x) { RequireGui(ctx, "set_cursor_x"); ImGui::SetCursorPosX(static_cast<float>(x)); })
        .Function("draw_text", "(x: number, y: number, text: string, color?: integer) -> nil",
            "Draws text on the overlay behind script windows (default white).",
            [&ctx](double x, double y, const std::string& text, sol::optional<lua_Integer> color) {
                RequireGui(ctx, "draw_text");
                ctx.overlay->AddText(ViewPoint(ctx, x, y), color ? static_cast<ImU32>(*color) : IM_COL32_WHITE,
                                     text.c_str());
            })
        .Function("draw_rect", "(x: number, y: number, width: number, height: number, color: integer, filled?: boolean, rounding?: number) -> nil",
            "Draws a rectangle on the overlay.",
            [&ctx](double x, double y, double width, double height, lua_Integer color, sol::optional<bool> filled,
                   sol::optional<double> rounding) {
                RequireGui(ctx, "draw_rect");
                const ImVec2 min = ViewPoint(ctx, x, y);
                const ImVec2 max = ViewPoint(ctx, x + width, y + height);
                const float r = static_cast<float>(rounding.value_or(0.0));
                if (filled.value_or(false))
                    ctx.overlay->AddRectFilled(min, max, static_cast<ImU32>(color), r);
                else
                    ctx.overlay->AddRect(min, max, static_cast<ImU32>(color), r);
            })
        .Function("draw_line", "(x1: number, y1: number, x2: number, y2: number, color: integer, thickness?: number) -> nil",
            "Draws a line on the overlay.",
            [&ctx](double x1, double y1, double x2, double y2, lua_Integer color, sol::optional<double> thickness) {
                RequireGui(ctx, "draw_line");
                ctx.overlay->AddLine(ViewPoint(ctx, x1, y1), ViewPoint(ctx, x2, y2), static_cast<ImU32>(color),
                                     static_cast<float>(thickness.value_or(1.0)));
            })
        .Function("draw_circle", "(x: number, y: number, radius: number, color: integer, filled?: boolean) -> nil",
            "Draws a circle on the overlay.",
            [&ctx](double x, double y, double radius, lua_Integer color, sol::optional<bool> filled) {
                RequireGui(ctx, "draw_circle");
                if (filled.value_or(false))
                    ctx.overlay->AddCircleFilled(ViewPoint(ctx, x, y), static_cast<float>(radius), static_cast<ImU32>(color));
                else
                    ctx.overlay->AddCircle(ViewPoint(ctx, x, y), static_cast<float>(radius), static_cast<ImU32>(color));
            })
        .Function("dim", "(alpha: number) -> nil", "Darkens the whole game view (0..1), e.g. behind a pause menu.",
            [&ctx](double alpha) {
                RequireGui(ctx, "dim");
                const int a = static_cast<int>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
                ctx.overlay->AddRectFilled(ViewPoint(ctx, 0.0, 0.0), ViewPoint(ctx, ctx.view.width, ctx.view.height),
                                           IM_COL32(0, 0, 0, a));
            })
        .Function("rgba", "(r: number, g: number, b: number, a?: number) -> integer",
            "Packs a colour from 0..1 components (alpha defaults to 1).",
            [](double r, double g, double b, sol::optional<double> a) {
                const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(static_cast<float>(r), static_cast<float>(g),
                                                                          static_cast<float>(b),
                                                                          static_cast<float>(a.value_or(1.0))));
                return static_cast<lua_Integer>(color);
            })
        .Function("screen_size", "() -> number, number", "Width and height of the game view.",
            [&ctx] { return std::make_tuple(static_cast<double>(ctx.view.width), static_cast<double>(ctx.view.height)); })
        .Function("wants_mouse", "() -> boolean",
            "True while a script window is hovered or in use (skip mouse-look clicks).",
            [&ctx] { return ctx.uiWantsMouse; })
        .Function("wants_keyboard", "() -> boolean", "True while a script text field has keyboard focus.",
            [&ctx] { return ctx.uiWantsKeyboard; });
}

} // namespace SE::Scripting
