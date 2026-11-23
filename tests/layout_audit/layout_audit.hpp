#pragma once

#include <lvgl.h>

namespace files::layout_audit {

// Test-only metadata for virtual viewports and custom Files selection.
constexpr auto selected_flag = LV_OBJ_FLAG_USER_1;
constexpr auto viewport_flag = LV_OBJ_FLAG_USER_2;
constexpr auto overlay_flag = LV_OBJ_FLAG_USER_3;
constexpr auto progress_flag = LV_OBJ_FLAG_USER_4;
inline char mono_separator;
inline char mono_cross;
inline char mono_image;
inline char mono_frame;
inline char mono_track;
inline char mono_toast;
inline char keyboard_proxy;
inline void markMonoSeparator(lv_obj_t* obj) { lv_obj_set_user_data(obj, &mono_separator); }
void poll(lv_display_t* display);

}  // namespace files::layout_audit
