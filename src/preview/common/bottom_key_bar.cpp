#include "preview/common/bottom_key_bar.hpp"

#include "assets/assets.h"
#include "core/files_language.hpp"
#include "views/ui_style.hpp"

namespace files {
namespace {

const char* keyLabel(const void* icon)
{
    if (icon == &image_icon_fullscreen) return local("Full", "Celá");
    if (icon == &image_icon_zoom_out) return "−";
    if (icon == &image_icon_zoom_in) return "+";
    if (icon == &image_icon_reset) return local("Rotate", "Otočit");
    if (icon == &image_icon_playback_replay) return "−10 s";
    if (icon == &image_icon_playback_forward) return "+10 s";
    if (icon == &image_icon_playback_play) return local("Play", "Hrát");
    if (icon == &image_icon_playback_pause) return local("Pause", "Pauza");
    if (icon == &image_icon_playback_2x) return "2×";
    if (icon == &image_icon_playback_5x) return "5×";
    return "1×";
}

}  // namespace

BottomKeyBar::BottomKeyBar(lv_obj_t* parent)
    : _bar(ui::caption(parent, 150, "")), _label(lv_obj_get_child(_bar, 0))
{
    lv_obj_set_width(_label, 252);
}

BottomKeyBar::~BottomKeyBar() { lv_obj_delete(_bar); }

void BottomKeyBar::setItems(std::initializer_list<Item> items)
{
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) text += "  ";
        text += static_cast<char>(item.key);
        text += ' ';
        text += keyLabel(item.image);
    }
    const auto keys = keyHints({text, local("Esc Back", "Esc Zpět")}, {}, 252);
    lv_label_set_text(_label, keys.c_str());
    ui::hidden(_bar, items.size() == 0);
}

}  // namespace files