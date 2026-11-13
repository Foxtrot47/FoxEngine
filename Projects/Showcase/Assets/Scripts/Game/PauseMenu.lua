-- Pause menu, graphics settings, level select, FPS overlay and window title,
-- ported from the old Player game host.
--
-- The pause action toggles the menu. While paused fox.time.scale is 0 (the
-- simulation holds, the scene keeps rendering), the cursor is released and
-- Game/PlayerController.lua ignores movement input.

local PauseMenu = fox.script {
    properties = {
        { name = "show_fps", type = "boolean", default = false,
          tooltip = "Draw the frame rate in the top-left corner." },
        { name = "menu_width", type = "number", default = 380, min = 200, max = 1000,
          tooltip = "Width of the pause menu in pixels." },
        { name = "dim_alpha", type = "number", default = 0.55, min = 0, max = 1, step = 0.01,
          tooltip = "How much the scene darkens behind the menu." },
    },
}

-- fox.graphics.aa_mode values and their menu labels, in menu order.
local AA_MODES = { "off", "fxaa", "taa", "fsr_native", "fsr_quality", "fsr_balanced", "fsr_performance" }
local AA_LABELS = {
    "Off", "FXAA", "TAA",
    "FSR 3.1 Native AA", "FSR 3.1 Quality", "FSR 3.1 Balanced", "FSR 3.1 Performance",
}

local FPS_COLOR = fox.ui.rgba(1, 1, 1, 220 / 255)
local FPS_SHADOW = fox.ui.rgba(0, 0, 0, 200 / 255)

local function aa_index(mode)
    for i, name in ipairs(AA_MODES) do
        if name == mode then return i end
    end
    return 1
end

local function file_name(path)
    return path:match("[^/\\]+$") or path
end

local function same_path(a, b)
    return (a:gsub("\\", "/")) == (b:gsub("\\", "/"))
end

function PauseMenu:on_start()
    self.paused = self.paused or false
    self.scene_ready = self.scene_ready or false
    self.scenes = fox.scene.list()
end

function PauseMenu:on_scene_loaded(scene)
    self.scene_ready = true
    self.scenes = fox.scene.list()
    self:set_paused(false)
    fox.window.set_title(fox.app.project_name() .. " - " .. scene.name)
end

function PauseMenu:on_disable()
    -- Never leave the game frozen when this script stops.
    if self.paused then
        self:set_paused(false)
    end
end

function PauseMenu:set_paused(paused)
    self.paused = paused
    fox.time.scale = paused and 0 or 1
    if paused then
        fox.input.capture_cursor(false)
    end
end

function PauseMenu:on_update(dt)
    if not self.scene_ready or fox.scene.is_loading() then
        return
    end
    if fox.input.action_pressed("pause") then
        self:set_paused(not self.paused)
    end
end

function PauseMenu:on_gui()
    if self.paused and self.scene_ready then
        fox.ui.dim(self.dim_alpha)
    end
    if self.show_fps then
        self:draw_fps()
    end
    if self.paused and self.scene_ready then
        self:draw_menu()
    end
end

function PauseMenu:draw_fps()
    local text = string.format("%.0f FPS  %.2f ms", fox.time.fps, fox.time.unscaled_dt * 1000)
    fox.ui.draw_text(11, 11, text, FPS_SHADOW)
    fox.ui.draw_text(10, 10, text, FPS_COLOR)
end

function PauseMenu:draw_menu()
    local width = self.menu_width
    if fox.ui.begin_window("##PauseMenu", { center = true, width = width, no_decoration = true, auto_resize = true }) then
        local content = fox.ui.content_width()

        fox.ui.set_cursor_x((width - fox.ui.calc_text_width("PAUSED")) * 0.5)
        fox.ui.text("PAUSED")
        fox.ui.spacing()

        if fox.ui.button("Resume", content) then
            self:set_paused(false)
        end

        fox.ui.spacing()
        fox.ui.separator_text("Graphics")
        self:draw_graphics_settings()

        fox.ui.spacing()
        fox.ui.separator_text("Level")
        self:draw_level_select()

        fox.ui.spacing()
        fox.ui.separator()
        fox.ui.spacing()
        if fox.ui.button("Quit", content) then
            fox.app.quit()
        end
    end
    fox.ui.end_window()
end

function PauseMenu:draw_graphics_settings()
    local graphics = fox.graphics
    local mode = graphics.aa_mode
    local index, changed = fox.ui.combo("Anti-Aliasing", aa_index(mode), AA_LABELS)
    if changed then
        mode = AA_MODES[index]
        graphics.aa_mode = mode
    end

    if mode:sub(1, 4) == "fsr_" then
        local sharpen, sharpen_changed = fox.ui.checkbox("Sharpening", graphics.fsr_sharpening)
        if sharpen_changed then graphics.fsr_sharpening = sharpen end
        if sharpen then
            local sharpness, sharpness_changed =
                fox.ui.slider_float("Sharpness", graphics.fsr_sharpness, 0, 1, "%.2f")
            if sharpness_changed then graphics.fsr_sharpness = sharpness end
        end
    end

    local bloom, bloom_changed = fox.ui.checkbox("Bloom", graphics.bloom)
    if bloom_changed then graphics.bloom = bloom end
    local ao, ao_changed = fox.ui.checkbox("Ambient Occlusion", graphics.ambient_occlusion)
    if ao_changed then graphics.ambient_occlusion = ao end
    local fog, fog_changed = fox.ui.checkbox("Volumetric Fog", graphics.volumetric_fog)
    if fog_changed then graphics.volumetric_fog = fog end
    self.show_fps = fox.ui.checkbox("Show FPS", self.show_fps)
end

function PauseMenu:draw_level_select()
    local current = fox.scene.path()
    for _, path in ipairs(self.scenes) do
        local active = same_path(path, current)
        -- "##path" keeps ids unique when two folders hold the same file name.
        if fox.ui.selectable(file_name(path) .. "##" .. path, active) and not active then
            fox.scene.load(path)
            self:set_paused(false)
        end
    end
end

return PauseMenu
