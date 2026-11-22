#pragma once

#include <lvgl.h>

namespace files::layout_audit {

// Test-only metadata for virtual viewports and custom Files selection.
constexpr auto selected_flag = LV_OBJ_FLAG_USER_1;
constexpr auto viewport_flag = LV_OBJ_FLAG_USER_2;
constexpr auto overlay_flag = LV_OBJ_FLAG_USER_3;
constexpr auto progress_flag = LV_OBJ_FLAG_USER_4;
void poll(lv_display_t* display);

}  // namespace files::layout_audit
