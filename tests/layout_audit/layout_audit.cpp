#include "assets/font_assets.hpp"
#include "layout_audit.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <vector>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <png.h>

#include "src/core/lv_obj_class_private.h"
#include "src/misc/lv_text_private.h"
#include "src/widgets/label/lv_label_private.h"

LV_FONT_DECLARE(font_noto_sans_semibold_12);
LV_FONT_DECLARE(font_noto_sans_semibold_14);
LV_FONT_DECLARE(font_noto_sans_clock_16);
LV_FONT_DECLARE(font_noto_sans_sc_semibold_10);
LV_FONT_DECLARE(font_noto_sans_sc_semibold_12);
LV_FONT_DECLARE(font_noto_sans_sc_semibold_14);
LV_FONT_DECLARE(font_noto_sans_jp_semibold_10);
LV_FONT_DECLARE(font_noto_sans_jp_semibold_12);
LV_FONT_DECLARE(font_noto_sans_jp_semibold_14);
LV_FONT_DECLARE(font_noto_sans_mono_semibold_12);
LV_FONT_DECLARE(font_noto_sans_mono_semibold_14);
LV_FONT_DECLARE(font_montserrat_medium_14);

namespace files::layout_audit {
namespace {
using nlohmann::json;
using Color = std::array<int, 3>;
struct Frame {
    int width, height;
    std::vector<uint16_t> pixels;
};
using Render = std::function<Frame()>;
using Selected = std::function<bool(const lv_obj_t*)>;

struct Rect {
    int x, y, w, h;

    explicit Rect(const lv_area_t& a)
        : x(a.x1), y(a.y1), w(std::max(0, a.x2 - a.x1 + 1)), h(std::max(0, a.y2 - a.y1 + 1)) {}
    Rect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}
    json data() const { return {x, y, w, h}; }
};

Rect intersect(Rect a, Rect b)
{
    const int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0, std::min(a.y + a.h, b.y + b.h) - y)};
}

Color rgb(lv_color_t color)
{
    const lv_color32_t c = lv_color_to_32(color, LV_OPA_COVER);
    return {c.red, c.green, c.blue};
}

Color blend(Color top, Color base, int opacity)
{
    for (size_t i = 0; i < top.size(); ++i) {
        top[i] = (top[i] * opacity + base[i] * (255 - opacity) + 127) / 255;
    }
    return top;
}

std::string hex(Color c)
{
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", c[0], c[1], c[2]);
    return text;
}

const char* type_name(lv_obj_t* obj)
{
    const auto* clazz = lv_obj_get_class(obj);
    if (clazz == &lv_label_class) return "label";
    if (clazz == &lv_image_class) return "image";
    if (clazz == &lv_button_class) return "button";
    if (clazz == &lv_textarea_class) return "textarea";
    if (clazz == &lv_bar_class) return "bar";
#if LV_USE_SLIDER
    if (clazz == &lv_slider_class) return "slider";
#endif
    return clazz->name ? clazz->name : "object";
}

json font_data(const lv_font_t* font)
{
    struct Face { const lv_font_t* font; const char* name; int size; };
    const Face faces[] = {
        {files::uiKeyBitmapFont(), "Files key bitmap", 12},
        {&font_noto_sans_semibold_12, "Noto Sans SemiBold", 12},
        {&font_noto_sans_semibold_14, "Noto Sans SemiBold", 14},
        {&font_noto_sans_clock_16, "Noto Sans SemiBold", 16},
        {&font_noto_sans_sc_semibold_10, "Noto Sans SC SemiBold", 10},
        {&font_noto_sans_sc_semibold_12, "Noto Sans SC SemiBold", 12},
        {&font_noto_sans_sc_semibold_14, "Noto Sans SC SemiBold", 14},
        {&font_noto_sans_jp_semibold_10, "Noto Sans JP SemiBold", 10},
        {&font_noto_sans_jp_semibold_12, "Noto Sans JP SemiBold", 12},
        {&font_noto_sans_jp_semibold_14, "Noto Sans JP SemiBold", 14},
        {&font_noto_sans_mono_semibold_12, "Noto Sans Mono SemiBold", 12},
        {&font_noto_sans_mono_semibold_14, "Noto Sans Mono SemiBold", 14},
        {&font_montserrat_medium_14, "Montserrat Medium", 14},
#if LV_FONT_MONTSERRAT_12
        {&lv_font_montserrat_12, "LVGL Montserrat", 12},
#endif
#if LV_FONT_MONTSERRAT_18
        {&lv_font_montserrat_18, "LVGL Montserrat", 18},
#endif
#if LV_FONT_MONTSERRAT_20
        {&lv_font_montserrat_20, "LVGL Montserrat", 20},
#endif
    };
    for (const auto& face : faces) {
        // Files copies the font descriptors to attach fallbacks. The bitmap descriptor is shared.
        if (font == face.font || (font->dsc && font->dsc == face.font->dsc)) {
            return {{"name", face.name}, {"size", face.size}, {"line_height", font->line_height},
                    {"fallback", font->fallback ? font_data(font->fallback) : json(nullptr)}};
        }
    }
    return {{"name", "LVGL font"}, {"size", nullptr}, {"line_height", font->line_height}};
}

const char* long_mode(lv_label_long_mode_t mode)
{
    switch (mode) {
        case LV_LABEL_LONG_MODE_WRAP: return "wrap";
        case LV_LABEL_LONG_MODE_DOTS: return "dots";
        case LV_LABEL_LONG_MODE_SCROLL: return "scroll";
        case LV_LABEL_LONG_MODE_SCROLL_CIRCULAR: return "scroll_circular";
        case LV_LABEL_LONG_MODE_CLIP: return "clip";
        default: return "unknown";
    }
}

const char* text_align(lv_text_align_t align)
{
    if (align == LV_TEXT_ALIGN_CENTER) return "center";
    if (align == LV_TEXT_ALIGN_RIGHT) return "right";
    return "left";
}

void add_text(json& data, lv_obj_t* obj, Rect content, Rect clip, Rect fixed_clip, Color bg, int opacity)
{
    const char* displayed = lv_label_get_text(obj);
    if (!displayed) return;
    const auto* label = reinterpret_cast<const lv_label_t*>(obj);
    std::string source(displayed);
    if (label->dot_begin != std::numeric_limits<uint32_t>::max() && !label->static_txt) {
        source.assign(displayed, label->dot_begin);
        size_t overwritten = 0;
        while (overwritten < sizeof(label->dot) && label->dot[overwritten] != '\0') {
            source.push_back(label->dot[overwritten++]);
        }
        if (overwritten == sizeof(label->dot)) {
            source.append(displayed + label->dot_begin + overwritten);
        }
    }
    if (label->recolor) {
        for (size_t i = 0; (i = source.find('#', i)) != std::string::npos;) {
            const auto close = source.find('#', i + 1);
            if (close == std::string::npos || close < i + 8 || source[i + 7] != ' ') {
                ++i;
                continue;
            }
            source.erase(close, 1);
            source.erase(i, 8);
            i = close - 8;
        }
        displayed = source.c_str();
    }
    const char* text = source.c_str();

    const auto* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    const int letter_space = lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN);
    const int line_space = lv_obj_get_style_text_line_space(obj, LV_PART_MAIN);
    const auto mode = lv_label_get_long_mode(obj);
    const auto align = lv_obj_get_style_text_align(obj, LV_PART_MAIN);
    lv_point_t natural{}, laid{};
    lv_text_get_size(&natural, text, font, letter_space, line_space, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    lv_text_get_size(&laid, text, font, letter_space, line_space, std::max(1, content.w),
                     (mode == LV_LABEL_LONG_MODE_WRAP || mode == LV_LABEL_LONG_MODE_DOTS) ?
                         LV_TEXT_FLAG_NONE : LV_TEXT_FLAG_EXPAND);
    lv_point_t drawn{};
    lv_text_get_size(&drawn, displayed, font, letter_space, line_space, std::max(1, content.w),
                     (mode == LV_LABEL_LONG_MODE_WRAP || mode == LV_LABEL_LONG_MODE_DOTS) ?
                         LV_TEXT_FLAG_NONE : LV_TEXT_FLAG_EXPAND);
    const bool moving = natural.x > content.w &&
                        (mode == LV_LABEL_LONG_MODE_SCROLL || mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    const int ink_width = std::max(0, moving ? content.w : std::min(drawn.x, content.w));
    const int x_offset = align == LV_TEXT_ALIGN_CENTER ? (content.w - ink_width) / 2 :
                         align == LV_TEXT_ALIGN_RIGHT ? content.w - ink_width : 0;
    Rect ink{content.x + x_offset, content.y, ink_width, std::max(0, drawn.y)};
    const int fg_opacity = lv_obj_get_style_text_opa(obj, LV_PART_MAIN) * opacity / 255;

    std::set<uint32_t> missing;
    uint32_t pos = 0;
    while (text[pos] != '\0') {
        const uint32_t ch = lv_text_encoded_next(text, &pos);
        if (ch == '\n' || ch == '\r' || ch == '\t') continue;
        lv_font_glyph_dsc_t glyph{};
        if (!lv_font_get_glyph_dsc(font, &glyph, ch, 0) || glyph.is_placeholder) {
            missing.insert(ch);
        }
    }

    if (drawn.y == font->line_height) {
        int top = font->line_height;
        int bottom = 0;
        uint32_t pos = 0;
        while (displayed[pos] != '\0') {
            const auto ch = lv_text_encoded_next(displayed, &pos);
            lv_font_glyph_dsc_t glyph{};
            if (!lv_font_get_glyph_dsc(font, &glyph, ch, 0) || !glyph.box_h) continue;
            const int end = font->line_height - font->base_line - glyph.ofs_y;
            top = std::min(top, end - glyph.box_h);
            bottom = std::max(bottom, end);
        }
        if (top < bottom) {
            ink.y += top + label->offset.y;
            ink.h = bottom - top;
        }
    }
    const auto shown = intersect(ink, clip);
    const auto fixed_visible = intersect(ink, fixed_clip);

    data["text"] = text;
    data["displayed_text"] = displayed;
    data["font"] = font_data(font);
    const auto fg_style = rgb(lv_obj_get_style_text_color(obj, LV_PART_MAIN));
    data["fg_style"] = hex(fg_style);
    data["fg"] = hex(blend(fg_style, bg, fg_opacity));
    data["text_opa"] = fg_opacity;
    data["long_mode"] = long_mode(mode);
    data["align"] = text_align(align);
    data["ink"] = ink.data();
    const bool clipped_h = (mode == LV_LABEL_LONG_MODE_WRAP || mode == LV_LABEL_LONG_MODE_DOTS)
                               ? laid.x > content.w || (shown.w > 0 && shown.w < ink.w)
                               : natural.x > content.w || (shown.w > 0 && shown.w < ink.w);
    const bool clipped_v = laid.y > content.h || (shown.h > 0 && shown.h < ink.h);
    const bool fixed_clip_v = laid.y > content.h || (fixed_visible.h > 0 && fixed_visible.h < ink.h);
    data["metrics"] = {{"natural_width", natural.x}, {"laid_width", laid.x}, {"laid_height", laid.y},
                       {"clipped_h", clipped_h}, {"clipped_v", clipped_v}, {"fixed_clip_v", fixed_clip_v}, {"missing", missing}};
    data["clipped_text"] = clipped_h || clipped_v;
}

void write_png(const std::filesystem::path& path, const Frame& frame)
{
    std::vector<uint8_t> out;
    out.reserve(frame.pixels.size() * 3);
    for (uint16_t pixel : frame.pixels) {
        const unsigned red = pixel >> 11, green = (pixel >> 5) & 63, blue = pixel & 31;
        out.push_back(static_cast<uint8_t>((red << 3) | (red >> 2)));
        out.push_back(static_cast<uint8_t>((green << 2) | (green >> 4)));
        out.push_back(static_cast<uint8_t>((blue << 3) | (blue >> 2)));
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = frame.width;
    png.height = frame.height;
    png.format = PNG_FORMAT_RGB;
    if (!png_image_write_to_file(&png, path.c_str(), 0, out.data(), 0, nullptr)) {
        throw std::runtime_error(path.string() + ": " + png.message);
    }
}
}  // namespace


namespace {
void collect(json& objects, std::vector<lv_obj_t*>& labels, lv_obj_t* obj,
             const std::string& id, const std::string& parent, Rect clip, Rect fixed_clip,
             Color bg, int opacity, bool hidden, bool scroll_clip,
             int layer, int z, bool audited, const Selected& selected, int& next_layer)
{
    lv_area_t a{}, content_a{};
    lv_obj_get_coords(obj, &a);
    lv_obj_get_content_coords(obj, &content_a);
    const Rect rect(a), content(content_a);
    const auto visible = intersect(clip, rect);
    if (lv_obj_has_flag(obj, overlay_flag)) layer = ++next_layer;
    const auto* clazz = lv_obj_get_class(obj);
    const bool focus = lv_obj_has_state(obj, LV_STATE_FOCUSED) ||
                       lv_obj_has_state(obj, LV_STATE_FOCUS_KEY);
    const bool choice = lv_obj_has_state(obj, LV_STATE_CHECKED) || (selected && selected(obj));
    hidden = hidden || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    opacity = opacity * lv_obj_get_style_opa(obj, LV_PART_MAIN) / 255;
    hidden = hidden || opacity == 0;
    const int bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    bg = blend(rgb(lv_obj_get_style_bg_color(obj, LV_PART_MAIN)), bg, bg_opa * opacity / 255);
    const int border_width = lv_obj_get_style_border_width(obj, LV_PART_MAIN);
    const int radius = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    const auto child_count = lv_obj_get_child_count(obj);
    const auto* type = type_name(obj);
    const bool is_label = clazz == &lv_label_class;
    if (is_label && lv_obj_get_style_text_opa(obj, LV_PART_MAIN) == LV_OPA_TRANSP &&
        bg_opa == LV_OPA_TRANSP) hidden = true;
    const bool is_image = clazz == &lv_image_class && lv_image_get_src(obj);
    const bool custom_progress = lv_obj_has_flag(obj, progress_flag);
    const bool is_painted = is_label || is_image || custom_progress || clazz == &lv_bar_class ||
                            clazz == &lv_button_class ||
                            (clazz == &lv_textarea_class && lv_obj_get_user_data(obj) != &keyboard_proxy);
    const auto role = is_painted ? "content" : "surface";
    json data = {{"id", id}, {"parent", parent.empty() ? json(nullptr) : json(parent)},
                 {"type", type}, {"rect", rect.data()}, {"content", content.data()},
                 {"clip", visible.data()}, {"layer", layer}, {"z", z},
                 {"overlay", lv_obj_has_flag(obj, overlay_flag)},
                 {"mono_separator", lv_obj_get_user_data(obj) == &mono_separator},
                 {"mono_cross", lv_obj_get_user_data(obj) == &mono_cross},
                 {"mono_track", lv_obj_get_user_data(obj) == &mono_track},
                 {"mono_toast", lv_obj_get_user_data(obj) == &mono_toast},
                 {"mono_frame", lv_obj_get_user_data(obj) == &mono_frame},
                 {"cover", lv_obj_has_flag(obj, overlay_flag) ||
                           (is_label && bg_opa == LV_OPA_COVER)},
                 {"role", role}, {"hidden", hidden}, {"audited", audited && !hidden},
                 {"scroll_clip", scroll_clip}, {"focused", focus}, {"selected", choice},
                 {"disabled", lv_obj_has_state(obj, LV_STATE_DISABLED)}, {"text", nullptr},
                 {"border_width", border_width}, {"radius", radius == LV_RADIUS_CIRCLE ?
                                                                  std::min(rect.w, rect.h) / 2 : radius},
                 {"border_color", hex(rgb(lv_obj_get_style_border_color(obj, LV_PART_MAIN)))},
                 {"bg", hex(bg)}, {"bg_opa", bg_opa * opacity / 255}};
    if (lv_obj_get_user_data(obj) == &mono_image) {
        const int width = lv_image_get_transformed_width(obj);
        const int height = lv_image_get_transformed_height(obj);
        data["mono_image"] = {rect.x + rect.w / 2 - width / 2, rect.y + rect.h / 2 - height / 2, width, height};
        data["mono_image_clip"] = clip.data();
    }
    if (is_label) {
        labels.push_back(obj);
        add_text(data, obj, content, visible, fixed_clip, bg, opacity);
    }
    if (clazz == &lv_textarea_class) {
        data["value_text"] = lv_textarea_get_text(obj);
        data["font"] = font_data(lv_obj_get_style_text_font(obj, LV_PART_MAIN));
    }
    if (custom_progress) {
        const auto* fill = lv_obj_get_child(obj, 0);
        data["value_range"] = {{"min_value", 0}, {"max_value", rect.w},
                               {"value", lv_obj_get_width(fill)}};
        data["fill_color"] = hex(rgb(lv_obj_get_style_bg_color(fill, LV_PART_MAIN)));
    }
    if (clazz == &lv_bar_class) {
        data["value_range"] = {{"min_value", lv_bar_get_min_value(obj)},
                               {"max_value", lv_bar_get_max_value(obj)},
                               {"value", lv_bar_get_value(obj)}};
    }
#if LV_USE_SLIDER
    if (clazz == &lv_slider_class) {
        data["value_range"] = {{"min_value", lv_slider_get_min_value(obj)},
                               {"max_value", lv_slider_get_max_value(obj)},
                               {"value", lv_slider_get_value(obj)}};
    }
#endif
    objects.push_back(std::move(data));

    const bool overflow_visible = lv_obj_has_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    const bool scrollable = lv_obj_has_flag(obj, viewport_flag) ||
                            (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) &&
                             (lv_obj_get_scroll_top(obj) + lv_obj_get_scroll_bottom(obj) > 0 ||
                              lv_obj_get_scroll_left(obj) + lv_obj_get_scroll_right(obj) > 0));
    const auto next_clip = overflow_visible ? clip : visible;
    // A virtual viewport intentionally clips everything beyond its own bounds.
    const Rect unbounded{-LV_COORD_MAX / 2, -LV_COORD_MAX / 2, LV_COORD_MAX, LV_COORD_MAX};
    const auto next_fixed_clip = scrollable ? unbounded :
                                 overflow_visible ? fixed_clip : intersect(fixed_clip, rect);
    for (uint32_t i = 0; i < child_count; ++i) {
        collect(objects, labels, lv_obj_get_child(obj, i), id + "_" + std::to_string(i), id,
                next_clip, next_fixed_clip, bg, opacity, hidden, scroll_clip || scrollable, layer, static_cast<int>(i),
                audited, selected, next_layer);
    }
}

void suppress_label(lv_event_t* event)
{
    auto* task = static_cast<lv_draw_task_t*>(lv_event_get_param(event));
    if (auto* label = lv_draw_task_get_label_dsc(task)) {
        label->opa = LV_OPA_TRANSP;
    }
}

void dirty_labels(const std::vector<lv_obj_t*>& labels)
{
    for (auto* label : labels) {
        lv_obj_invalidate(label);
    }
}
}  // namespace

namespace {
void capture(const std::filesystem::path& directory, std::string_view state,
             const std::vector<lv_obj_t*>& roots, const Render& render, const Selected& selected)
{
    for (auto* root : roots) lv_obj_update_layout(root);
    const auto original = render();
    const Rect screen{0, 0, original.width, original.height};
    json objects = json::array();
    std::vector<lv_obj_t*> labels;
    int next_layer = static_cast<int>(roots.size());
    for (size_t i = 0; i < roots.size(); ++i) {
        collect(objects, labels, roots[i], "o" + std::to_string(i), "", screen, screen, {0, 0, 0},
                255, false, false, static_cast<int>(i), 0, true, selected, next_layer);
    }
    std::vector<bool> flags;
    for (auto* label : labels) {
        flags.push_back(lv_obj_has_flag(label, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS));
        lv_obj_add_flag(label, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(label, suppress_label, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    }
    dirty_labels(labels);
    const auto underlay = render();
    for (size_t i = 0; i < labels.size(); ++i) {
        lv_obj_remove_event_cb(labels[i], suppress_label);
        if (!flags[i]) lv_obj_remove_flag(labels[i], LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }
    dirty_labels(labels);
    render();

    std::filesystem::create_directories(directory);
    const auto stem = std::string(state);
    write_png(directory / (stem + ".png"), original);
    write_png(directory / (stem + ".pozadi.png"), underlay);
    std::ofstream out(directory / (stem + ".json"));
    out << json{{"state", state}, {"format", "RGB565"}, {"width", original.width},
                {"height", original.height}, {"objects", objects}}
                   .dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    if (!out) throw std::runtime_error("cannot write " + stem + ".json");
}

Frame current_frame(lv_display_t* display)
{
    lv_obj_invalidate(lv_display_get_screen_active(display));
    lv_refr_now(display);
    const auto* draw = lv_display_get_buf_active(display);
    if (!draw || lv_display_get_render_mode(display) == LV_DISPLAY_RENDER_MODE_PARTIAL ||
        lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) {
        throw std::runtime_error("layout audit needs a full-size SDL RGB565 buffer");
    }
    Frame frame{lv_display_get_horizontal_resolution(display),
                lv_display_get_vertical_resolution(display), {}};
    frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height);
    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(frame.pixels.data() + y * frame.width, draw->data + y * draw->header.stride,
                    frame.width * sizeof(uint16_t));
    }
    return frame;
}
}  // namespace

void poll(lv_display_t* display)
{
    const char* directory = std::getenv("FILES_LAYOUT_AUDIT_DIR");
    if (!directory || !*directory) return;
    const auto request = std::filesystem::path(directory) / "request";
    std::ifstream input(request);
    std::string state;
    if (!std::getline(input, state)) return;
    input.close();
    if (state.empty() || state.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
        throw std::runtime_error("invalid layout audit state");
    }
    const std::vector<lv_obj_t*> roots{lv_display_get_screen_active(display),
                                       lv_display_get_layer_top(display),
                                       lv_display_get_layer_sys(display)};
    capture(directory, state, roots, [display] { return current_frame(display); },
            [](const lv_obj_t* obj) {
                return lv_obj_has_flag(obj, selected_flag);
            });
    std::filesystem::remove(request);
}

}  // namespace files::layout_audit
