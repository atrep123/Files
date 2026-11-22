#pragma once

#include "assets/font_assets.hpp"
#include "views/browser_text.hpp"
#include <algorithm>

namespace files::ui {

constexpr uint32_t black = 0x000000;
constexpr uint32_t white = 0xFFFFFF;
constexpr uint32_t secondary = 0xBDBEBD;
constexpr uint32_t accent = 0xFFCF42;
constexpr uint32_t warning = 0xFFAA42;
constexpr uint32_t error = 0xFF8284;
constexpr uint32_t success = 0x8CE38C;
constexpr uint32_t bar = 0x212421;
constexpr uint32_t dialog = 0x102029;

inline lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color)
{
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

inline lv_obj_t* label(lv_obj_t* parent, int x, int y, int width, const lv_font_t* font, uint32_t color = white)
{
    auto* obj = lv_label_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

inline void hidden(lv_obj_t* obj, bool hide)
{
    if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

inline void line(lv_obj_t* label, const std::string& text, int width, const lv_font_t* font,
                 size_t limit = std::string::npos, bool extension = false)
{
    lv_label_set_text(label, fitText(text, width, font, limit, extension).c_str());
}

inline lv_obj_t* caption(lv_obj_t* parent, int y, const std::string& text, const lv_font_t* font = uiFont12())
{
    auto* strip = box(parent, 0, y, 320, 20, bar);
    auto* content = label(strip, 8, std::max<int>(2, (20 - lv_font_get_line_height(font)) / 2), 304, font);
    line(content, text, 304, font, y == 150 ? 44 : std::string::npos, y == 0);
    return strip;
}

inline void toast(lv_obj_t* label, std::string_view text, int y, uint32_t color)
{
    const auto split = text.find('\n');
    const bool twoLines = split != std::string_view::npos;
    auto first = fitText(std::string(text.substr(0, split)), 296, uiFont12(), 36);
    if (twoLines) first += "\n" + fitText(std::string(text.substr(split + 1)), 296, uiFont12(), 36);
    const int height = twoLines ? 42 : 24;
    lv_obj_set_pos(label, 4, y);
    lv_obj_set_size(label, 312, height);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(dialog), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_hor(label, 8, 0);
    lv_obj_set_style_pad_top(label, (height - (twoLines ? 2 : 1) * lv_font_get_line_height(uiFont12())) / 2, 0);
    lv_label_set_text(label, first.c_str());
}

}  // namespace files::ui