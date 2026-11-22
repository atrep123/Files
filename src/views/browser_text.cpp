#include "views/browser_text.hpp"

#include "assets/font_assets.hpp"
#include "core/files_language.hpp"
#include "src/misc/lv_text_private.h"
#include <algorithm>
#include <filesystem>
#include <vector>

namespace files {
namespace {

std::vector<size_t> characterEnds(const std::string& text)
{
    std::vector<size_t> ends{0};
    for (uint32_t offset = 0; offset < text.size();) {
        lv_text_encoded_next(text.c_str(), &offset);
        ends.push_back(offset);
    }
    return ends;
}

bool fits(std::string_view text, int width, size_t limit, const lv_font_t* font)
{
    return textLength(text) <= limit && textWidth(text, font) <= width;
}

}  // namespace

int textWidth(std::string_view text, const lv_font_t* font)
{
    lv_point_t size{};
    const std::string value(text);
    lv_text_get_size(&size, value.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

size_t textLength(std::string_view text)
{
    return std::count_if(text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xC0) != 0x80; });
}

std::string fitText(std::string text, int width, const lv_font_t* font, size_t limit, bool preserveExtension)
{
    std::replace_if(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; }, ' ');
    if (fits(text, width, limit, font)) return text;
    const std::string ellipsis = "…";
    if (!fits(ellipsis, width, limit, font)) return {};
    const auto ends = characterEnds(text);
    size_t left = 0;
    size_t right = ends.size() - 1;

    const auto build = [&](size_t first, size_t last) {
        return text.substr(0, ends[first]) + ellipsis + text.substr(ends[last]);
    };
    if (preserveExtension) {
        const auto suffix = std::filesystem::path(text).extension().string();
        const auto stem = text.size() - suffix.size();
        const auto boundary = std::lower_bound(ends.begin(), ends.end(), stem);
        const auto candidate = static_cast<size_t>(boundary - ends.begin());
        if (candidate > 1 && fits(build(1, candidate), width, limit, font)) right = candidate;
    }
    const int leftBudget = std::max(0, (width - textWidth(build(0, right), font)) * 2 / 3);
    while (left + 1 < right && fits(build(left + 1, right), width, limit, font) &&
           (left == 0 || textWidth(text.substr(0, ends[left + 1]), font) <= leftBudget)) ++left;
    while (right > left + 1 && fits(build(left, right - 1), width, limit, font)) --right;
    while (left + 1 < right && fits(build(left + 1, right), width, limit, font)) ++left;
    return build(left, right);
}

std::string fitPath(std::string_view path, int width, const lv_font_t* font)
{
    if (textWidth(path, font) <= width) return std::string(path);
    for (size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
        const auto tail = "…" + std::string(path.substr(slash));
        if (textWidth(tail, font) <= width) return tail;
    }
    const auto slash = path.rfind('/');
    return fitText(std::string(slash == std::string_view::npos ? path : path.substr(slash + 1)), width, font);
}

std::string browserRowText(const FileEntry& entry, int width)
{
    const auto name = entry.name.empty() ? local("(unnamed)", "(bez názvu)") : entry.name;
    return fitText(name, width, uiFont14(), std::string::npos, !entry.directory);
}

std::string keyHints(std::initializer_list<std::string_view> primary,
                     std::initializer_list<std::string_view> optional, int width)
{
    std::string text;
    const auto add = [&](std::string_view part) {
        if (part.empty()) return;
        auto candidate = text;
        if (!candidate.empty()) candidate += keyGap;
        candidate += part;
        if (fits(candidate, width, 44, uiFont12())) text = std::move(candidate);
    };
    for (const auto part : primary) add(part);
    for (const auto part : optional) add(part);
    return text;
}

std::string browserKeyHint(bool places, bool eject, bool usb, bool selected, bool canMove, bool menu)
{
    const auto* open = local("Enter Open", "Enter Otevřít");
    const auto* back = local("Esc Back", "Esc Zpět");
    const auto* actions = local("Tab Menu", "Tab Nabídka");
    const auto* ejectHint = eject ? local("E Eject", "E Vysunout") : "";
    const auto* movement = canMove ? "F↑ X↓" : "";
    const auto* usbHint = usb ? "U USB" : "";
    if (menu) return keyHints({movement, local("Enter Select", "Enter Vybrat"), back}, {ejectHint, usbHint});
    if (places) {
        if (eject) {
            return keyHints({movement, inCzech() ? "Enter ↵" : open, ejectHint, actions}, {usbHint});
        }
        return keyHints({movement, selected ? open : "", actions}, {usbHint});
    }
    if (eject) {
        const auto* driveMovement = canMove && inCzech() ? "F↑X↓" : movement;
        return keyHints({driveMovement, selected ? (inCzech() ? "Enter ↵" : open) : "", ejectHint, actions, back}, {});
    }
    auto keys = keyHints({movement, selected ? open : "", actions, back}, {usbHint});
    if (usb && keys.find(usbHint) == std::string::npos) {
        keys = keyHints({movement, selected ? "Enter ↵" : "", actions, back}, {usbHint});
    }
    return keys;
}

}  // namespace files
