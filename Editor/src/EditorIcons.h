#pragma once

// UTF-8 codepoints from Google's Material Icons font
// (ThirdParty/fonts/MaterialIcons-Regular.ttf, Apache 2.0). Merged into the
// default UI font by EditorTheme with the k_iconRangeMin/Max glyph range.

#define ICON_MD_PUBLIC            "\xEE\xA0\x8B" // U+E80B globe (level)
#define ICON_MD_VIDEOCAM          "\xEE\x81\x8B" // U+E04B camera
#define ICON_MD_WB_SUNNY          "\xEE\x90\xB0" // U+E430 sun
#define ICON_MD_HIGHLIGHT         "\xEE\x89\x9F" // U+E25F spot light
#define ICON_MD_LIGHTBULB_OUTLINE "\xEE\xA4\x8F" // U+E90F point light
#define ICON_MD_CATEGORY          "\xEE\x95\xB4" // U+E574 static mesh
#define ICON_MD_PANORAMA_FISH_EYE "\xEE\x90\x8C" // U+E40C sphere
#define ICON_MD_CROP_LANDSCAPE    "\xEE\x8F\x85" // U+E3C5 plane
#define ICON_MD_GRAIN             "\xEE\x8F\xAA" // U+E3EA particle emitter
#define ICON_MD_LENS              "\xEE\x8F\xBA" // U+E3FA ball
#define ICON_MD_ADD               "\xEE\x85\x85" // U+E145 plus (add entity)
#define ICON_MD_VISIBILITY        "\xEE\xA3\xB4" // U+E8F4 eye open
#define ICON_MD_VISIBILITY_OFF    "\xEE\xA3\xB5" // U+E8F5 eye closed

namespace Editor {
inline constexpr unsigned short k_iconRangeMin = 0xE000;
inline constexpr unsigned short k_iconRangeMax = 0xF8FF;
} // namespace Editor
