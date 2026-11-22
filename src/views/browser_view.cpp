#include "views/browser_view.hpp"

#include "assets/assets.h"
#include "core/files_language.hpp"
#include "views/magic_view.hpp"
#include "views/ui_style.hpp"
#ifdef FILES_LAYOUT_AUDIT
#include "layout_audit/layout_audit.hpp"
#endif

#include <algorithm>
#include <array>
#include <filesystem>

namespace files {
namespace {

constexpr int rowHeight = 26;
constexpr int visibleRows = 5;
constexpr int rowWidth = 312;
constexpr int metadataWidth = 84;

const lv_image_dsc_t* entryIcon(const FileEntry& entry)
{
    if (entry.icon == "folder") return &image_icon_folder;
    if (entry.icon == "audio") return &image_icon_audio;
    if (entry.icon == "video") return &image_icon_video;
    if (entry.icon == "image") return &image_icon_image;
    if (entry.icon == "text") return &image_icon_text;
    if (entry.icon == "scripts") return &image_icon_scripts;
    return &image_icon_default;
}

struct RowData {
    std::string name;
    std::string metadata;
    const lv_image_dsc_t* icon = nullptr;
    bool directory = false;
    bool checked = false;
};

RowData actionRow(BrowserAction action, const FileEntry* target)
{
    switch (action) {
        case BrowserAction::Open: return {local("Open", "Otevřít"), {}, target && !target->directory ? &image_icon_open_file : &image_icon_folder};
        case BrowserAction::Copy: return {local("Copy", "Kopírovat"), {}, &image_icon_copy};
        case BrowserAction::Cut: return {local("Cut", "Vyjmout"), {}, &image_icon_cut};
        case BrowserAction::Paste: return {local("Paste", "Vložit"), {}, &image_icon_paste};
        case BrowserAction::Rename: return {local("Rename", "Přejmenovat"), {}, &image_icon_rename};
        case BrowserAction::Info: return {local("Details", "Podrobnosti"), {}, &image_icon_info};
        case BrowserAction::Delete: return {local("Delete", "Smazat"), {}, &image_icon_delete};
        case BrowserAction::Places: return {local("Places", "Umístění"), {}, &image_icon_folder};
        case BrowserAction::Eject: return {local("E  Eject", "E  Vysunout"), {}, &image_icon_eject};
        case BrowserAction::NewFolder: return {local("New folder", "Nová složka"), {}, &image_icon_new_folder};
        case BrowserAction::Language: return {local("Language", "Jazyk"), {}, &image_icon_language};
        case BrowserAction::SystemLanguage: return {local("System language", "Systémový jazyk"), {}, &image_icon_language, false, languagePreference() == Language::System};
        case BrowserAction::English: return {"English", {}, nullptr, false, languagePreference() == Language::English};
        case BrowserAction::Czech: return {"Čeština", {}, nullptr, false, languagePreference() == Language::Czech};
    }
    return {};
}

lv_group_t* keyboardGroup()
{
    for (auto* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_group(indev)) {
            return lv_indev_get_group(indev);
        }
    }
    return nullptr;
}

uint32_t toneColor(DialogTone tone)
{
    switch (tone) {
        case DialogTone::Warning: return ui::warning;
        case DialogTone::Error: return ui::error;
        case DialogTone::Success: return ui::success;
        default: return ui::white;
    }
}

const char* toneSymbol(DialogTone tone)
{
    switch (tone) {
        case DialogTone::Warning: return "!";
        case DialogTone::Error: return LV_SYMBOL_CLOSE;
        case DialogTone::Success: return LV_SYMBOL_OK;
        default: return "";
    }
}

}  // namespace

class BrowserView::Rows {
public:
    Rows(lv_obj_t* parent, uint32_t background)
        : _background(background), _panel(ui::box(parent, 0, 20, 320, 130, background)),
          _thumb(ui::box(_panel, 315, 0, 3, 12, ui::secondary)),
          _empty(ui::label(_panel, 8, 54, 304, uiFont14())),
          _pasteHint(ui::label(_panel, 8, 74, 304, uiFont12(), ui::secondary))
    {
        lv_obj_set_style_text_align(_empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(_pasteHint, LV_TEXT_ALIGN_CENTER, 0);
        ui::hidden(_pasteHint, true);
        for (int i = 0; i < visibleRows; ++i) {
            auto& row = _rows[i];
            row.panel = ui::box(_panel, 0, i * rowHeight, rowWidth, rowHeight, background);
            row.icon = lv_image_create(row.panel);
            row.name = ui::label(row.panel, 32, 4, 276, uiFont14());
            const int baseline = lv_font_get_line_height(uiFont14()) - uiFont14()->base_line;
            const int metadataY = 4 + baseline - (lv_font_get_line_height(uiFont12()) - uiFont12()->base_line);
            row.metadata = ui::label(row.panel, 224, metadataY, metadataWidth, uiFont12(), ui::secondary);
            lv_obj_set_style_text_align(row.metadata, LV_TEXT_ALIGN_RIGHT, 0);
            row.check = ui::label(row.panel, 289, 4, 18, LV_FONT_DEFAULT);
            lv_label_set_text(row.check, LV_SYMBOL_OK);
        }
    }

    ~Rows() { lv_obj_delete(_panel); }

    void setFiles(const std::vector<FileEntry>& entries, int selection, bool places, bool reset)
    {
        if (reset) {
            _first = 0;
            if (_unobstructedFirst) *_unobstructedFirst = 0;
        }
        _files = &entries;
        _places = places;
        select(selection, true);
    }

    void setActions(std::vector<RowData> actions, int selected)
    {
        _actions = std::move(actions);
        _first = 0;
        select(selected, true);
    }

    void show(bool visible) { ui::hidden(_panel, !visible); }
    void showPasteHint(bool visible)
    {
        ui::hidden(_pasteHint, !visible);
        _hasPasteHint = visible;
        alignEmpty(130 - _reservedHeight);
        if (visible) ui::line(_pasteHint, local("Paste via Tab", "Vložit přes Tab"), 304, uiFont12());
    }
    int selectedY() const
    {
        return _selected < 0 ? -1 : 20 + (_selected - (_unobstructedFirst.value_or(_first))) * rowHeight;
    }

    void reserve(int y, int height)
    {
        if (_reservedY == y && _reservedHeight == height) return;
        if (height && !_reservedHeight) _unobstructedFirst = _first;
        if (!height && _unobstructedFirst) {
            _first = *_unobstructedFirst;
            _unobstructedFirst.reset();
        }
        _reservedY = y;
        _reservedHeight = height;
        const int panelHeight = 130 - height;
        _capacity = panelHeight / rowHeight;
        lv_obj_set_y(_panel, 20 + (y == 20 ? height : 0));
        lv_obj_set_height(_panel, panelHeight);
        alignEmpty(panelHeight);
        select(_selected, true);
    }

    void select(int selection, bool rebind = false)
    {
        const int count = _files ? static_cast<int>(_files->size()) : static_cast<int>(_actions.size());
        _selected = count ? std::clamp(selection, 0, count - 1) : -1;
        const auto firstFor = [&](int first, int capacity) {
            return std::clamp(first, std::max(0, _selected - capacity + 1),
                              std::max(0, std::min(_selected, count - capacity)));
        };
        if (_unobstructedFirst) *_unobstructedFirst = firstFor(*_unobstructedFirst, visibleRows);
        const int first = firstFor(_first, _capacity);
        if (first != _first) rebind = true;
        _first = first;
        ui::hidden(_empty, count != 0);
        if (!count) lv_label_set_text(_empty, local("Empty folder", "Prázdná složka"));
        ui::hidden(_thumb, count <= _capacity);
        if (count > _capacity) {
            const int track = _capacity * rowHeight;
            const int height = std::max(12, (track * _capacity + count / 2) / count);
            lv_obj_set_height(_thumb, height);
            lv_obj_set_y(_thumb, (track - height) * _first / (count - _capacity));
        }
        for (int i = 0; i < visibleRows; ++i) {
            auto& row = _rows[i];
            const int index = _first + i;
            ui::hidden(row.panel, index >= count || i >= _capacity);
            if (index >= count || i >= _capacity) continue;
            if (rebind) {
                if (_files) {
                    const auto& entry = (*_files)[index];
                    const auto name = _places && !entry.mountRoot ? localizedMessage(entry.name) : entry.name;
                    const auto metadata = entry.mountRoot ? (entry.freeBytes ? sizeInUnits(*entry.freeBytes) : "–") :
                                          entry.directory ? "" : sizeInUnits(entry.size);
                    bind(row, {name, metadata, entry.mountRoot ? &image_icon_usb : entryIcon(entry),
                               entry.directory});
                } else {
                    bind(row, _actions[index]);
                }
            }
            const bool active = index == _selected;
            const auto foreground = active ? ui::black : ui::white;
            lv_obj_set_style_bg_color(row.panel, lv_color_hex(active ? ui::accent : _background), 0);
            lv_obj_set_style_text_color(row.name, lv_color_hex(foreground), 0);
            lv_obj_set_style_text_color(row.check, lv_color_hex(foreground), 0);
            lv_obj_set_style_text_color(row.metadata, lv_color_hex(active ? ui::black : ui::secondary), 0);
            lv_obj_set_style_image_recolor(row.icon, lv_color_hex(ui::black), 0);
            lv_obj_set_style_image_recolor_opa(row.icon, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
#ifdef FILES_LAYOUT_AUDIT
            if (active) lv_obj_add_flag(row.panel, layout_audit::selected_flag);
            else lv_obj_remove_flag(row.panel, layout_audit::selected_flag);
#endif
        }
    }

private:
    struct Row {
        lv_obj_t* panel;
        lv_obj_t* name;
        lv_obj_t* metadata;
        lv_obj_t* icon;
        lv_obj_t* check;
    };

    void alignEmpty(int panelHeight)
    {
        lv_obj_set_y(_empty, panelHeight / 2 - (_hasPasteHint ? 18 : 9));
        lv_obj_set_y(_pasteHint, panelHeight / 2 + 2);
    }

    void bind(const Row& row, const RowData& data)
    {
        const int used = data.metadata.empty() ? (data.checked ? 24 : 0) : 8 + metadataWidth;
        const int width = 276 - used;
        ui::hidden(row.check, !data.checked);
        lv_obj_set_width(row.name, width);
        ui::line(row.name, data.name, width, uiFont14(), std::string::npos, !data.directory);
        ui::line(row.metadata, data.metadata, metadataWidth, uiFont12());
        ui::hidden(row.metadata, data.metadata.empty());
        ui::hidden(row.icon, !data.icon);
        if (data.icon) {
            lv_image_set_src(row.icon, data.icon);
            const int w = data.icon->header.w;
            const int h = data.icon->header.h;
            const int scale = 16 * LV_SCALE_NONE / std::max(w, h);
            lv_image_set_scale(row.icon, scale);
            // LVGL centers scaled images over the source object's intrinsic area.
            lv_obj_set_pos(row.icon, 8 + (16 - w) / 2, (rowHeight - h) / 2 + (data.icon == &image_icon_paste ? 1 : 0));
            lv_obj_set_style_transform_pivot_x(row.icon, w / 2, 0);
            lv_obj_set_style_transform_pivot_y(row.icon, h / 2, 0);
        }
    }

    uint32_t _background;
    lv_obj_t* _panel;
    lv_obj_t* _thumb;
    lv_obj_t* _empty;
    lv_obj_t* _pasteHint;
    std::array<Row, visibleRows> _rows{};
    const std::vector<FileEntry>* _files = nullptr;
    std::vector<RowData> _actions;
    bool _places = false;
    bool _hasPasteHint = false;
    int _first = 0;
    int _selected = -1;
    int _capacity = visibleRows;
    int _reservedY = 0;
    int _reservedHeight = 0;
    std::optional<int> _unobstructedFirst;
};

class BrowserView::Dialog {
public:
    Dialog(lv_obj_t* parent, BrowserViewModel& vm)
        : _vm(vm), _box(ui::box(parent, 8, 8, 304, 154, ui::dialog)),
          _symbol(ui::label(_box, 8, 8, 16, uiFont14())),
          _title(ui::label(_box, 8, 8, 288, uiFont14())),
          _identity(ui::label(_box, 8, 29, 288, uiFont14())),
          _meta(ui::label(_box, 8, 48, 288, uiFont12(), ui::secondary)),
          _warningSymbol(ui::label(_box, 8, 65, 12, uiFont14(), ui::warning)),
          _warning(ui::label(_box, 24, 65, 272, uiFont12(), ui::warning)),
          _fixed(ui::label(_box, 8, 65, 288, uiFont14(), ui::error)),
          _body(ui::box(_box, 8, 30, 288, 98, ui::dialog)),
          _text(ui::label(_body, 0, 0, 282, uiFont14())),
          _diagnostics(ui::label(_body, 0, 0, 282, uiMonoFont12(), ui::secondary)),
          _thumb(ui::box(_box, 293, 0, 3, 12, ui::secondary)),
          _passwordLine(ui::box(_box, 8, 84, 288, 1, ui::secondary)),
          _passwordCursor(ui::box(_box, 8, 70, 2, 13, ui::white)),
          _masked(ui::label(_box, 8, 67, 288, uiFont14())),
          _progressTrack(ui::box(_box, 8, 72, 288, 6, ui::bar)),
          _progressFill(ui::box(_progressTrack, 0, 0, 0, 4, ui::accent)),
          _hint(ui::label(_box, 8, 130, 288, uiFont12())),
          _scrollUp(ui::label(_box, 8, 130, 18, uiFont12())),
          _scrollDown(ui::label(_box, 26, 130, 18, uiFont12())),
          _input(lv_textarea_create(_box))
    {
#ifdef FILES_LAYOUT_AUDIT
        lv_obj_add_flag(_box, layout_audit::overlay_flag);
        lv_obj_add_flag(_body, layout_audit::viewport_flag);
#endif
        lv_obj_set_style_border_width(_progressTrack, 1, 0);
        lv_obj_set_style_border_color(_progressTrack, lv_color_hex(ui::secondary), 0);
        lv_obj_add_flag(_body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(_body, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(_body, [](lv_event_t* event) {
            static_cast<Dialog*>(lv_event_get_user_data(event))->updateThumb();
        }, LV_EVENT_SCROLL, this);
        lv_label_set_text(_warningSymbol, "!");
        lv_obj_set_style_text_line_space(_text, 19 - lv_font_get_line_height(uiFont14()), 0);
        lv_obj_set_style_text_line_space(_diagnostics, 19 - lv_font_get_line_height(uiMonoFont12()), 0);
        lv_obj_set_style_text_align(_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(_scrollUp, "F↑");
        lv_label_set_text(_scrollDown, "X↓");
        lv_obj_remove_style_all(_input);
        lv_obj_set_size(_input, 288, 24);
        lv_obj_set_pos(_input, 8, 54);
        lv_obj_set_style_bg_color(_input, lv_color_hex(ui::black), 0);
        lv_obj_set_style_bg_opa(_input, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_input, 1, 0);
        lv_obj_set_style_border_color(_input, lv_color_hex(ui::accent), 0);
        lv_obj_set_style_pad_hor(_input, 6, 0);
        lv_obj_set_style_pad_top(_input, 2, 0);
        lv_obj_set_style_pad_top(_input, -2, LV_PART_CURSOR);
        lv_obj_set_style_pad_bottom(_input, -3, LV_PART_CURSOR);
        lv_obj_set_style_pad_left(_input, -3, LV_PART_CURSOR);
        lv_obj_set_style_text_color(_input, lv_color_hex(ui::secondary), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_text_font(_input, uiFont12(), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_text_font(_input, uiFont14(), 0);
        lv_obj_set_style_text_color(_input, lv_color_hex(ui::white), 0);
        lv_obj_set_style_border_side(_input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_border_width(_input, 2, LV_PART_CURSOR);
        lv_obj_set_style_border_color(_input, lv_color_hex(ui::accent), LV_PART_CURSOR);
        lv_obj_set_style_text_color(_input, lv_color_hex(ui::white), LV_PART_CURSOR);
        lv_obj_set_scrollbar_mode(_input, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(_input, [](lv_event_t* event) {
            if (auto* animation = static_cast<lv_anim_t*>(lv_event_get_param(event))) {
                lv_anim_set_duration(animation, 1);
            }
        }, LV_EVENT_SCROLL_BEGIN, nullptr);
        lv_textarea_set_one_line(_input, true);
        lv_textarea_set_max_length(_input, 255);
        lv_obj_add_event_cb(_input, [](lv_event_t* event) {
            auto* self = static_cast<Dialog*>(lv_event_get_user_data(event));
            if (!self->_setting) self->_vm.setPendingRenameName(lv_textarea_get_text(self->_input));
        }, LV_EVENT_VALUE_CHANGED, this);
        ui::hidden(_input, true);
        ui::hidden(_box, true);
    }

    ~Dialog()
    {
        edit(false);
        lv_obj_delete(_box);
    }

    void set(const PackageDialog& package, const PendingDeleteFile& deletion, const PendingRenameFile& rename)
    {
        const bool visible = package.mode != PackageDialogMode::Closed || deletion.active || rename.active;
        ui::hidden(_box, !visible);
        edit(rename.active);
        if (!visible) {
            _mode = PackageDialogMode::Closed;
            return;
        }
        lv_obj_move_foreground(_box);
        PackageDialog dialog = package;
        if (deletion.active) {
            dialog = {PackageDialogMode::Result,
                      deletion.file.directory ? local("Delete folder?", "Smazat složku?") :
                                                local("Delete file?", "Smazat soubor?"),
                      deletion.file.name + (deletion.file.directory ?
                          local("\nAll contents will also be deleted.", "\nSložka bude smazána včetně obsahu.") : ""),
                      false, DialogTone::Warning};
        } else if (rename.active) {
            dialog = {PackageDialogMode::Result,
                      rename.creating ? local("New folder", "Nová složka") : local("Rename", "Přejmenovat"),
                      {}, false};
        }
        const bool previousMode = dialog.mode == _mode;
        const bool previousTitle = dialog.title == _last_title;
        _mode = dialog.mode;
        _last_title = dialog.title;

        const bool symbol = dialog.tone != DialogTone::Normal;
        ui::hidden(_symbol, !symbol);
        lv_obj_set_style_text_font(_symbol, dialog.tone == DialogTone::Warning ? uiFont14() : LV_FONT_DEFAULT, 0);
        lv_label_set_text(_symbol, toneSymbol(dialog.tone));
        for (auto* label : {_symbol, _title}) lv_obj_set_style_text_color(label, lv_color_hex(toneColor(dialog.tone)), 0);
        lv_obj_set_x(_title, symbol ? 28 : 8);
        lv_obj_set_width(_title, symbol ? 268 : 288);
        ui::line(_title, dialog.title, symbol ? 268 : 288, uiFont14());

        ui::hidden(_identity, dialog.identity.empty());
        ui::hidden(_meta, dialog.metadata.empty());
        ui::hidden(_warning, !dialog.rootScripts);
        ui::hidden(_warningSymbol, !dialog.rootScripts);
        ui::line(_identity, dialog.identity, 288, uiFont14(), std::string::npos, true);
        ui::line(_meta, dialog.metadata, 288, uiFont12());
        lv_label_set_text(_warning, local("Install scripts will run as root", "Při instalaci poběží skripty jako root"));
        const bool password = _mode == PackageDialogMode::Password;
        const bool installing = _mode == PackageDialogMode::Installing;
        const int fixedY = dialog.rootScripts ? 82 : !dialog.metadata.empty() ? 66 : 50;
        ui::hidden(_fixed, dialog.fixedStatus.empty());
        lv_obj_set_y(_fixed, fixedY);
        ui::line(_fixed, dialog.fixedStatus, 288, uiFont14());
        ui::hidden(_passwordLine, !password);
        ui::hidden(_passwordCursor, !password || !dialog.maskedPassword.empty());
        ui::hidden(_masked, !password || dialog.maskedPassword.empty());
        ui::line(_masked, dialog.maskedPassword, 288, uiFont14());
        const bool transferring = _mode == PackageDialogMode::Transferring;
        _indeterminate = installing || (transferring && !dialog.progress);
        ui::hidden(_progressTrack, !transferring && !installing);
        lv_obj_set_y(_progressTrack, installing ? 80 : 72);
        if (dialog.progress) {
            lv_obj_set_x(_progressFill, 0);
            lv_obj_set_width(_progressFill, 286 * *dialog.progress / 100);
            ui::hidden(_progressFill, *dialog.progress == 0);
        } else if (_indeterminate) {
            lv_obj_set_width(_progressFill, 52);
            ui::hidden(_progressFill, false);
        }
#ifdef FILES_LAYOUT_AUDIT
        if (password) lv_obj_add_flag(_passwordLine, layout_audit::selected_flag);
        else lv_obj_remove_flag(_passwordLine, layout_audit::selected_flag);
#endif
        if (rename.active) {
            lv_textarea_set_placeholder_text(_input, rename.creating ? local("Folder name", "Název složky") : "");
        }
        const int top = rename.active ? 85 : password ? 91 : installing ? 61 : !dialog.fixedStatus.empty() ? fixedY + 19 :
                        dialog.rootScripts ? 83 : !dialog.metadata.empty() ? 66 : !dialog.identity.empty() ? 50 : 31;
        lv_obj_set_y(_body, top);
        const int lineHeight = 19;
        const int height = std::max(lineHeight, ((130 - top) / lineHeight) * lineHeight);
        lv_obj_set_height(_body, installing ? lineHeight : height);
        auto body = dialog.text;
        if (const auto first = body.find_first_not_of("\n"); first != std::string::npos) body.erase(0, first);
        if (installing) {
            while (!body.empty() && body.back() == '\n') body.pop_back();
            if (const auto last = body.rfind('\n'); last != std::string::npos) body.erase(0, last + 1);
            body = fitText(std::move(body), 282, uiFont14());
        }
        const std::string marker = std::string(local("Details", "Podrobnosti")) + "\n";
        std::string diagnostics;
        if (dialog.tone == DialogTone::Error) {
            const auto begin = body.rfind(marker, 0) == 0 ? 0 : body.find("\n" + marker);
            if (begin != std::string::npos) {
                const auto end = begin + (begin == 0 ? 0 : 1) + marker.size();
                diagnostics = body.substr(end);
                if (end == marker.size()) body.clear();
                else body.resize(end - 1);
            }
        }
        while (!body.empty() && body.back() == '\n') body.pop_back();
        while (!diagnostics.empty() && diagnostics.back() == '\n') diagnostics.pop_back();
        lv_label_set_text(_text, body.c_str());
        lv_label_set_text(_diagnostics, diagnostics.c_str());
        ui::hidden(_diagnostics, diagnostics.empty());
        if (!diagnostics.empty()) {
            lv_obj_update_layout(_box);
            lv_obj_set_y(_diagnostics, body.empty() ? 0 : (lv_obj_get_height(_text) + 18) / 19 * 19);
        }
        lv_obj_set_style_text_color(_text, lv_color_hex(password && dialog.tone == DialogTone::Warning ?
                                                        ui::warning : ui::white), 0);
        if (!previousMode || !previousTitle) lv_obj_scroll_to_y(_body, 0, LV_ANIM_OFF);
        lv_obj_update_layout(_box);
        updateThumb();
        const bool canScroll = !rename.active && !deletion.active &&
                               lv_obj_get_scroll_top(_body) + lv_obj_get_scroll_bottom(_body) > 0 &&
                               (_mode == PackageDialogMode::Review || _mode == PackageDialogMode::Result);
        const auto cancel = local("Esc Cancel", "Esc Zrušit");
        const auto back = local("Esc Back", "Esc Zpět");
        std::string hint;
        if (rename.active || deletion.active) {
            hint = keyHints({rename.active ? (rename.creating ? local("Enter Create", "Enter Vytvořit") :
                                                               local("Enter Save", "Enter Uložit")) :
                                             local("Enter Delete", "Enter Smazat"), cancel}, {}, 288);
        } else {
            switch (_mode) {
                case PackageDialogMode::Review:
                    hint = dialog.allowInstall ? keyHints({canScroll ? "F↑ X↓" : "", local("Enter Install", "Enter Instalovat"), cancel}, {}, 288)
                                               : keyHints({canScroll ? "F↑ X↓" : "", back}, {}, 288);
                    break;
                case PackageDialogMode::ConfirmEject:
                    hint = keyHints({local("Enter Eject", "Enter Vysunout"), cancel}, {}, 288);
                    break;
                case PackageDialogMode::ConfirmTransfer:
                    hint = keyHints({dialog.move ? local("Enter Move", "Enter Přesunout") : local("Enter Copy", "Enter Kopírovat"),
                                     cancel}, {}, 288);
                    break;
                case PackageDialogMode::Password:
                    hint = keyHints({local("Enter Continue", "Enter Pokračovat"), cancel}, {}, 288);
                    break;
                case PackageDialogMode::Transferring: if (!dialog.cancelling) hint = cancel; break;
                case PackageDialogMode::Result: hint = keyHints({canScroll ? "F↑ X↓" : "", back}, {}, 288); break;
                default: break;
            }
        }
        _resultScroll = canScroll && _mode == PackageDialogMode::Result;
        lv_obj_set_x(_hint, 8);
        lv_obj_set_width(_hint, 288);
        lv_obj_set_style_text_align(_hint, LV_TEXT_ALIGN_CENTER, 0);
        ui::hidden(_scrollUp, true);
        ui::hidden(_scrollDown, true);
        _editAction = rename.creating ? local("Enter Create", "Enter Vytvořit") : local("Enter Save", "Enter Uložit");
        if (rename.active) {
            _editError.clear();
            refreshEditHint();
        } else {
            if (_resultScroll) updateResultHint();
            else ui::keyLegend(_hint, hint);
        }
    }

    void setName(const std::string& name)
    {
        if (!_editing) return;
        if (name != lv_textarea_get_text(_input)) {
            _setting = true;
            lv_textarea_set_text(_input, name.c_str());
            lv_textarea_set_cursor_pos(_input, LV_TEXTAREA_CURSOR_LAST);
            _setting = false;
        }
        _editError.clear();
        lv_label_set_text(_text, "");
        refreshEditHint();
    }

    void error(const std::string& message)
    {
        _editError = message;
        lv_label_set_text(_text, message.c_str());
        lv_obj_set_style_text_color(_text, lv_color_hex(ui::error), 0);
        refreshEditHint();
    }

    void scroll(int offset)
    {
        if (_mode == PackageDialogMode::Review || (_mode == PackageDialogMode::Result && !_editing)) {
            lv_obj_scroll_by_bounded(_body, 0, -offset, LV_ANIM_OFF);
            if (_resultScroll) updateResultHint();
        }
    }

    void tick(uint32_t nowMs)
    {
        if (_indeterminate && (_mode == PackageDialogMode::Installing || _mode == PackageDialogMode::Transferring)) {
            constexpr int travel = 286 - 52;
            const int phase = (nowMs / 5) % (2 * travel);
            lv_obj_set_x(_progressFill, phase <= travel ? phase : 2 * travel - phase);
        }
    }

private:
    void updateResultHint()
    {
        // Reserve both slots as the scroll position changes.
        const auto up = lv_obj_get_scroll_top(_body) > 0;
        const auto down = lv_obj_get_scroll_bottom(_body) > 0;
        const auto* back = local("Esc Back", "Esc Zpět");
        const auto* font = uiFont12();
        const int upWidth = textWidth("F↑", font) + 1;
        const int downWidth = textWidth("X↓", font) + 1;
        const int backWidth = textWidth(back, font) + 2;
        constexpr int gap = 8;
        const int x = (304 - upWidth - downWidth - backWidth - 2 * gap) / 2;
        lv_obj_set_x(_scrollUp, x);
        lv_obj_set_width(_scrollUp, upWidth);
        lv_obj_set_x(_scrollDown, x + upWidth + gap);
        lv_obj_set_width(_scrollDown, downWidth);
        lv_obj_set_x(_hint, x + upWidth + downWidth + 2 * gap);
        lv_obj_set_width(_hint, backWidth);
        lv_obj_set_style_text_align(_hint, LV_TEXT_ALIGN_LEFT, 0);
        ui::hidden(_scrollUp, !up);
        ui::hidden(_scrollDown, !down);
        ui::keyLegend(_hint, back);
    }

    void updateThumb()
    {
        const int top = lv_obj_get_scroll_top(_body);
        const int range = top + lv_obj_get_scroll_bottom(_body);
        ui::hidden(_thumb, range <= 0 || (_mode != PackageDialogMode::Review && _mode != PackageDialogMode::Result));
        if (range <= 0) return;
        const int track = lv_obj_get_height(_body);
        const int height = std::max(12, track * track / (track + range));
        lv_obj_set_height(_thumb, height);
        lv_obj_set_y(_thumb, lv_obj_get_y(_body) + (track - height) * top / range);
    }

    void refreshEditHint()
    {
        const bool ready = _editError.empty() && *lv_textarea_get_text(_input);
        ui::keyLegend(_hint, keyHints({ready ? _editAction : "", local("Esc Cancel", "Esc Zrušit")}, {}, 288));
    }

    void edit(bool editing)
    {
        if (editing == _editing) return;
        _editing = editing;
        ui::hidden(_input, !editing);
        lv_obj_set_style_text_color(_text, lv_color_hex(ui::white), 0);
        if (!editing) {
            if (lv_obj_get_group(_input)) lv_group_remove_obj(_input);
        } else if (auto* group = keyboardGroup()) {
            lv_group_add_obj(group, _input);
            lv_group_focus_obj(_input);
            lv_group_set_editing(group, true);
        }
    }

    BrowserViewModel& _vm;
    lv_obj_t* _box;
    lv_obj_t* _symbol;
    lv_obj_t* _title;
    lv_obj_t* _identity;
    lv_obj_t* _meta;
    lv_obj_t* _warningSymbol;
    lv_obj_t* _warning;
    lv_obj_t* _fixed;
    lv_obj_t* _body;
    lv_obj_t* _text;
    lv_obj_t* _diagnostics;
    lv_obj_t* _thumb;
    lv_obj_t* _passwordLine;
    lv_obj_t* _passwordCursor;
    lv_obj_t* _masked;
    lv_obj_t* _progressTrack;
    lv_obj_t* _progressFill;
    lv_obj_t* _hint;
    lv_obj_t* _scrollUp;
    lv_obj_t* _scrollDown;
    lv_obj_t* _input;
    PackageDialogMode _mode = PackageDialogMode::Closed;
    std::string _last_title;
    std::string _editAction;
    std::string _editError;
    bool _indeterminate = false;
    bool _resultScroll = false;
    bool _setting = false;
    bool _editing = false;
};

BrowserView::BrowserView(BrowserViewModel& vm) : _vm(vm) {}
BrowserView::~BrowserView() { destroy(); }

void BrowserView::onEnter(lv_obj_t* parent)
{
    _root = ui::box(parent, 0, 0, 320, 170, ui::black);
    auto* header = ui::box(_root, 0, 0, 320, 20, ui::bar);
    _heading = ui::label(header, 8, 2, 96, uiFont14());
    _path = ui::label(header, 108, 2, 204, uiFont12(), ui::secondary);
    _free = ui::label(header, 224, 2, 84, uiFont12(), ui::secondary);
    lv_obj_set_style_text_align(_free, LV_TEXT_ALIGN_RIGHT, 0);
    auto* footer = ui::box(_root, 0, 150, 320, 20, ui::bar);
    _keys = ui::label(footer, 8, 2, 304, uiFont12());
    _files = std::make_unique<Rows>(_root, ui::black);
    _actions = std::make_unique<Rows>(_root, ui::dialog);
    _actions->reserve(0, rowHeight);
    auto* menuNote = ui::box(_root, 0, 124, 320, 26, ui::dialog);
    _menu_exit = ui::label(menuNote, 8, 5, 304, uiFont12(), ui::secondary);
    ui::hidden(menuNote, true);
    _no_usb = ui::label(_root, 8, 102, 304, uiFont14(), ui::secondary);
    lv_obj_set_style_text_align(_no_usb, LV_TEXT_ALIGN_CENTER, 0);
    ui::hidden(_no_usb, true);
    auto* effects = ui::box(_root, 0, 0, 320, 150, ui::black);
    lv_obj_set_style_bg_opa(effects, LV_OPA_TRANSP, 0);
    _magic = std::make_unique<MagicView>(effects);
    lv_obj_move_foreground(header);
    lv_obj_move_foreground(footer);
    _toast = ui::label(_root, 4, 126, 312, uiFont12());
#ifdef FILES_LAYOUT_AUDIT
    lv_obj_add_flag(_toast, layout_audit::overlay_flag);
#endif
    ui::hidden(_toast, true);
    _dialog = std::make_unique<Dialog>(_root, _vm);
    _magic_seen = _vm.magic().get();

    _vm.currentDirectory().observe(this, [](void* ctx, const std::string&) {
        auto& self = *static_cast<BrowserView*>(ctx);
        self._files->setFiles(self._vm.entries().get(), self._vm.selectedIndex().get(), self._vm.inPlaces(), true);
        self.updateChrome();
    });
    _vm.entries().observe(this, [](void* ctx, const std::vector<FileEntry>& entries) {
        auto& self = *static_cast<BrowserView*>(ctx);
        self._files->setFiles(entries, self._vm.selectedIndex().get(), self._vm.inPlaces(), false);
        self.updateChrome();
    });
    _vm.selectedIndex().observe(this, [](void* ctx, const int& selection) {
        auto& self = *static_cast<BrowserView*>(ctx);
        self._files->select(selection);
        self.updateChrome();
        self.placeToast();
    });
    _vm.status().observe(this, [](void* ctx, const std::string& status) {
        static_cast<BrowserView*>(ctx)->updateStatus(status);
    });
    _vm.actionMenuOpen().observe(this, [](void* ctx, const bool& open) {
        static_cast<BrowserView*>(ctx)->updateMenu(open);
    });
    _vm.actionMenuSelectedIndex().observe(this, [](void* ctx, const int& index) {
        static_cast<BrowserView*>(ctx)->_actions->select(index);
    });
    _vm.pendingDelete().observe(this, [](void* ctx, const PendingDeleteFile&) {
        static_cast<BrowserView*>(ctx)->updateDialog();
    });
    _vm.pendingRename().observe(this, [](void* ctx, const PendingRenameFile&) {
        static_cast<BrowserView*>(ctx)->updateDialog();
    });
    _vm.pendingRenameName().observe(this, [](void* ctx, const std::string& name) {
        static_cast<BrowserView*>(ctx)->_dialog->setName(name);
    });
    _vm.packageDialog().observe(this, [](void* ctx, const PackageDialog&) {
        static_cast<BrowserView*>(ctx)->updateDialog();
    });
    _vm.packageScroll().observe(this, [](void* ctx, const int& offset) {
        static_cast<BrowserView*>(ctx)->_dialog->scroll(offset);
    });
    _vm.magic().observe(this, [](void* ctx, const uint32_t& serial) {
        auto& self = *static_cast<BrowserView*>(ctx);
        if (serial && serial != self._magic_seen) self._magic->generate(serial);
        else if (!serial) self._magic->stop();
        self._magic_seen = serial;
        self.updateChrome();
    });
}

void BrowserView::onExit() { destroy(); }

void BrowserView::destroy()
{
    if (!_root) return;
    _vm.currentDirectory().removeObserver();
    _vm.entries().removeObserver();
    _vm.selectedIndex().removeObserver();
    _vm.status().removeObserver();
    _vm.actionMenuOpen().removeObserver();
    _vm.actionMenuSelectedIndex().removeObserver();
    _vm.pendingDelete().removeObserver();
    _vm.pendingRename().removeObserver();
    _vm.pendingRenameName().removeObserver();
    _vm.packageDialog().removeObserver();
    _vm.packageScroll().removeObserver();
    _vm.magic().removeObserver();
    _dialog.reset();
    _magic.reset();
    _actions.reset();
    _files.reset();
    lv_obj_delete(_root);
    _root = nullptr;
    _toast_duration = 0;
    _external_y = _external_height = 0;
}

void BrowserView::tick(uint32_t nowMs)
{
    if (_usb != _vm.hasRemovableVolumes() || _usb_action != _vm.volumeActionShown()) updateChrome();
    if (_toast_duration && lv_tick_elaps(_toast_started) >= _toast_duration) {
        _toast_duration = 0;
        placeToast();
    }
    _magic->tick(nowMs);
    if (_magic_seen && !_magic->active()) _vm.magic().set(0);
    _dialog->tick(nowMs);
}

void BrowserView::updateChrome()
{
    _usb = _vm.hasRemovableVolumes();
    const bool menu = _vm.actionMenuOpen().get();
    const bool places = _vm.inPlaces();
    const std::string title = menu ? (_vm.languageMenu() ? local("Language", "Jazyk") : local("Menu", "Nabídka")) :
                              places ? local("Places", "Umístění") : _vm.directoryTitle();
    _usb_action = _vm.volumeActionShown();
    const auto context = menu ? _vm.menuContext() : "";
    const auto path = !menu && !places ? _vm.directoryCaption() : "";
    const bool hasContext = !path.empty() || !context.empty();
    const int width = hasContext ? std::min(textWidth(title, uiFont14()), 158) : places ? 204 : 304;
    lv_obj_set_width(_heading, width);
    ui::line(_heading, title, width, uiFont14());
    if (hasContext) {
        lv_obj_set_x(_path, 16 + width);
        lv_obj_set_width(_path, 296 - width);
        const auto caption = context.empty() ? fitPath(path, 296 - width, uiFont12()) :
            fitText(context, 296 - width, uiFont12(), std::string::npos, true);
        lv_label_set_text(_path, caption.c_str());
    }
    ui::hidden(_path, !hasContext);
    _files->showPasteHint(!places && _vm.hasClipboard() && _vm.entries().get().empty());
    ui::hidden(_free, !places || menu || !_usb);
    lv_label_set_text(_free, local("Free", "Volno"));
    const bool movement = menu ? _vm.actionCount() > 1 : _vm.entries().get().size() > 1;
    const auto keys = _magic_seen ? local("Esc Back", "Esc Zpět") :
        browserKeyHint(places, _vm.canEject() && !_vm.languageMenu(), _usb && !_usb_action,
                       _vm.selectedEntry() != nullptr, movement, menu);
    ui::keyLegend(_keys, keys);
    lv_label_set_text(_menu_exit, local("Hold Esc · Exit", "Podržet Esc · Ukončit"));
    lv_label_set_text(_no_usb, local("No USB drive detected", "Žádný USB disk"));
    ui::hidden(lv_obj_get_parent(_menu_exit), !menu || _vm.packageDialogOpen());
    ui::hidden(_no_usb, !places || _usb || _vm.modalInputActive() || _external_height || _toast_duration);
}

void BrowserView::updateMenu(bool open)
{
    if (open) {
        std::vector<RowData> rows;
        for (const auto action : _vm.availableActions()) {
            auto row = actionRow(action, _vm.selectedEntry());
            if (action == BrowserAction::Eject && _vm.canEject() && !_vm.inPlaces()) {
                row.name += " " + _vm.directoryTitle();
            }
            rows.push_back(std::move(row));
        }
        _actions->setActions(std::move(rows), _vm.actionMenuSelectedIndex().get());
    }
    const bool modal = _vm.pendingDelete().get().active || _vm.pendingRename().get().active || _vm.packageDialogOpen();
    _actions->show(open && !modal);
    _files->show(!open && !modal);
    placeToast();
    updateChrome();
}

void BrowserView::updateDialog()
{
    _dialog->set(_vm.packageDialog().get(), _vm.pendingDelete().get(), _vm.pendingRename().get());
    const bool modal = _vm.pendingDelete().get().active || _vm.pendingRename().get().active || _vm.packageDialogOpen();
    ui::hidden(lv_obj_get_parent(_heading), modal);
    ui::hidden(lv_obj_get_parent(_keys), modal);
    const bool menu = _vm.actionMenuOpen().get();
    _files->show(!modal && !menu);
    _actions->show(!modal && menu);
    updateChrome();
    placeToast();
}

void BrowserView::updateStatus(const std::string& status)
{
    if (status.empty() || status == "Ready" || status == "Empty folder") {
        _toast_duration = 0;
        placeToast();
        return;
    }
    const auto message = localizedMessage(status);
    if (_vm.pendingRename().get().active) {
        _dialog->error(message);
        return;
    }
    const bool success = status.rfind("Copied ", 0) == 0 || status.rfind("Moved ", 0) == 0 ||
                         status.rfind("Renamed ", 0) == 0 || status.rfind("Deleted ", 0) == 0 ||
                         status.rfind("Created ", 0) == 0;
    auto display = message;
    if (status.rfind("Copied ", 0) == 0) {
        display = local("Copied: ", "Zkopírováno: ") + status.substr(7);
    } else if (status.rfind("Created copy ", 0) == 0) {
        display = local("Created copy: ", "Vytvořena kopie: ") + status.substr(13);
    } else if (status.rfind("Deleted ", 0) == 0) {
        display = local("Deleted “", "Smazáno „") + status.substr(8) + local("”", "“");
    } else if (status.rfind("Created ", 0) == 0 && status.rfind("Created copy ", 0) != 0) {
        display = inCzech() ? "Složka „" + status.substr(8) + "“ vytvořena" : "Created folder “" + status.substr(8) + "”";
    }
    ui::toast(_toast, display, 126, success ? ui::success : ui::white);
    updateChrome();
    _toast_started = lv_tick_get();
    _toast_duration = message.find('\n') == std::string::npos ? 3000 : 5000;
    placeToast();
}

int BrowserView::toastY(int height) const
{
    return _files && _files->selectedY() >= 0 && _files->selectedY() + rowHeight > 150 - height ? 20 : 150 - height;
}

void BrowserView::setExternalToast(int y, int height)
{
    if (!_root || (_external_y == y && _external_height == height)) return;
    _external_y = y;
    _external_height = height;
    placeToast();
}

void BrowserView::placeToast()
{
    if (!_files) return;
    ui::hidden(_no_usb, !_vm.inPlaces() || _usb || _vm.modalInputActive() || _external_height || _toast_duration);
    const bool visible = _toast_duration && !_vm.modalInputActive() && !_external_height;
    ui::hidden(_toast, !visible);
    if (!visible) {
        _files->reserve(_external_y, _external_height);
        return;
    }
    lv_obj_update_layout(_root);
    const int height = lv_obj_get_height(_toast);
    const int y = toastY(height);
    _files->reserve(y, height);
    lv_obj_set_y(_toast, y);
    lv_obj_move_foreground(_toast);
}

}  // namespace files
