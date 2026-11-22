#pragma once

#include "core/files_types.hpp"
#include <lvgl.h>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

namespace files {

int textWidth(std::string_view text, const lv_font_t* font);
size_t textLength(std::string_view text);
std::string fitText(std::string text, int width, const lv_font_t* font,
                    size_t limit = std::string::npos, bool preserveExtension = false);
std::string fitPath(std::string_view path, int width, const lv_font_t* font);
std::string browserRowText(const FileEntry& entry, int width);
inline constexpr std::string_view keyGap = " ";

std::string keyHints(std::initializer_list<std::string_view> primary,
                     std::initializer_list<std::string_view> optional, int width = 304);
std::string browserKeyHint(bool places, bool eject, bool usb, bool selected, bool canMove, bool menu);

}  // namespace files