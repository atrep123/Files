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
    const char* symbol = nullptr;
    bool directory = false;
};

RowData actionRow(BrowserAction action)
{
    switch (action) {
        case BrowserAction::Open: return {local("Open", "Otevřít"), {}, &image_icon_folder};
        case BrowserAction::Copy: return {local("Copy", "Kopírovat"), {}, &image_icon_copy};
        case BrowserAction::Cut: return {local("Cut", "Vyjmout"), {}, &image_icon_cut};
        case BrowserAction::Paste: return {local("Paste", "Vložit"), {}, &image_icon_paste};
        case BrowserAction::Rename: return {local("Rename", "Přejmenovat"), {}, &image_icon_rename};
        case BrowserAction::Info: return {local("Details", "Podrobnosti"), {}, &image_icon_info};
        case BrowserAction::Delete: return {local("Delete", "Smazat"), {}, &image_icon_delete};
        case BrowserAction::Places: return {local("Places", "Umístění"), {}, &image_icon_folder};
        case BrowserAction::Eject: return {local("E  Eject", "E  Vysunout"), {}, nullptr, LV_SYMBOL_EJECT};
        case BrowserAction::NewFolder: return {local("New folder", "Nová složka"), {}, &image_icon_folder};
        case BrowserAction::Language: return {local("Language", "Jazyk"), {}, nullptr, LV_SYMBOL_SETTINGS};
        case BrowserAction::Exit: return {local("Hold Esc · Exit", "Podržet Esc · Ukončit"), {}, nullptr, LV_SYMBOL_POWER};
        case BrowserAction::SystemLanguage: return {local("System language", "Systémový jazyk"), {}, nullptr, LV_SYMBOL_SETTINGS};
        case BrowserAction::English: return {"English", {}, nullptr, LV_SYMBOL_RIGHT};
        case BrowserAction::Czech: return {"Čeština", {}, nullptr, LV_SYMBOL_RIGHT};
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
        case DialogTone::Error: return "×";
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
          _empty(ui::label(_panel, 8, 54, 304, uiFont14(), ui::secondary))
    {
        lv_obj_set_style_text_align(_empty, LV_TEXT_ALIGN_CENTER, 0);
        for (int i = 0; i < visibleRows; ++i) {
            auto& row = _rows[i];
            row.panel = ui::box(_panel, 0, i * rowHeight, rowWidth, rowHeight, background);
            row.icon = lv_image_create(row.panel);
            lv_obj_set_style_transform_pivot_x(row.icon, 0, 0);
            lv_obj_set_style_transform_pivot_y(row.icon, 0, 0);
            row.symbol = ui::label(row.panel, 8, 4, 16, LV_FONT_DEFAULT, ui::accent);

            row.name = ui::label(row.panel, 32, 4, 276, uiFont14());
            const int baseline = lv_font_get_line_height(uiFont14()) - uiFont14()->base_line;
            const int metadataY = 4 + baseline - (lv_font_get_line_height(uiFont12()) - uiFont12()->base_line);
            row.metadata = ui::label(row.panel, 224, metadataY, metadataWidth, uiFont12(), ui::secondary);
            lv_obj_set_style_text_align(row.metadata, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    ~Rows() { lv_obj_delete(_panel); }

    void setFiles(const std::vector<FileEntry>& entries, int selection, bool places, bool reset)
    {
        if (reset) _first = 0;
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
    int selectedY() const { return _selected < 0 ? -1 : 20 + (_selected - _first) * rowHeight; }

    void select(int selection, bool rebind = false)
    {
        const int count = _files ? static_cast<int>(_files->size()) : static_cast<int>(_actions.size());
        _selected = count ? std::clamp(selection, 0, count - 1) : -1;
        const int first = std::clamp(_first, std::max(0, _selected - visibleRows + 1),
                                      std::max(0, std::min(_selected, count - visibleRows)));
        if (first != _first) rebind = true;
        _first = first;
        ui::hidden(_empty, count != 0);
        if (!count) lv_label_set_text(_empty, local("Empty folder", "Prázdná složka"));
        ui::hidden(_thumb, count <= visibleRows);
        if (count > visibleRows) {
            const int height = std::max(12, (130 * visibleRows + count / 2) / count);
            lv_obj_set_height(_thumb, height);
            lv_obj_set_y(_thumb, (130 - height) * _first / (count - visibleRows));
        }
        for (int i = 0; i < visibleRows; ++i) {
            auto& row = _rows[i];
            const int index = _first + i;
            ui::hidden(row.panel, index >= count);
            if (index >= count) continue;
            if (rebind) {
                if (_files) {
                    const auto& entry = (*_files)[index];
                    const auto name = _places && !entry.mountRoot ? localizedMessage(entry.name) : entry.name;
                    const auto metadata = entry.mountRoot ? (entry.freeBytes ? sizeInUnits(*entry.freeBytes) : "–") :
                                          entry.directory ? "" : sizeInUnits(entry.size);
                    bind(row, {name, metadata, entryIcon(entry), entry.mountRoot ? LV_SYMBOL_USB : nullptr,
                               entry.directory});
                } else {
                    bind(row, _actions[index]);
                }
            }
            const bool active = index == _selected;
            const auto foreground = active ? ui::black : ui::white;
            lv_obj_set_style_bg_color(row.panel, lv_color_hex(active ? ui::accent : _background), 0);
            lv_obj_set_style_text_color(row.name, lv_color_hex(foreground), 0);
            lv_obj_set_style_text_color(row.metadata, lv_color_hex(active ? ui::black : ui::secondary), 0);
            lv_obj_set_style_text_color(row.symbol, lv_color_hex(active ? ui::black : ui::accent), 0);
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
        lv_obj_t* symbol;
    };

    void bind(const Row& row, const RowData& data)
    {
        const int width = data.metadata.empty() ? 276 : 276 - metadataWidth - 8;
        lv_obj_set_width(row.name, width);
        ui::line(row.name, data.name, width, uiFont14(), std::string::npos, !data.directory);
        ui::line(row.metadata, data.metadata, metadataWidth, uiFont12());
        ui::hidden(row.metadata, data.metadata.empty());
        ui::hidden(row.icon, data.symbol || !data.icon);
        ui::hidden(row.symbol, !data.symbol);
        if (data.symbol) {
            const auto* font = std::string_view(data.symbol) == LV_SYMBOL_USB ? &lv_font_montserrat_12 : LV_FONT_DEFAULT;
            lv_obj_set_style_text_font(row.symbol, font, 0);
            lv_obj_set_y(row.symbol, (rowHeight - lv_font_get_line_height(font)) / 2);
            lv_label_set_text(row.symbol, data.symbol);
        } else if (data.icon) {
            lv_image_set_src(row.icon, data.icon);
            const int w = data.icon->header.w;
            const int h = data.icon->header.h;
            const int scale = 16 * LV_SCALE_NONE / std::max(w, h);
            lv_image_set_scale(row.icon, scale);
            lv_obj_set_pos(row.icon, 8 + (16 - w * scale / LV_SCALE_NONE) / 2,
                            (rowHeight - h * scale / LV_SCALE_NONE) / 2);
        }
    }

    uint32_t _background;
    lv_obj_t* _panel;
    lv_obj_t* _thumb;
    lv_obj_t* _empty;
    std::array<Row, visibleRows> _rows{};
    const std::vector<FileEntry>* _files = nullptr;
    std::vector<RowData> _actions;
    bool _places = false;
    int _first = 0;
    int _selected = -1;
};

class BrowserView::Dialog {
public:
    Dialog(lv_obj_t* parent, BrowserViewModel& vm)
        : _vm(vm), _box(ui::box(parent, 8, 8, 304, 154, ui::dialog)),
          _symbol(ui::label(_box, 8, 8, 16, uiFont14())),
          _title(ui::label(_box, 8, 8, 288, uiFont14())),
          _identity(ui::label(_box, 8, 29, 288, uiFont14())),
          _meta(ui::label(_box, 8, 48, 288, uiFont12(), ui::secondary)),
          _warning(ui::label(_box, 8, 65, 288, uiFont12(), ui::warning)),
          _body(ui::box(_box, 8, 30, 288, 98, ui::dialog)),
          _text(ui::label(_body, 0, 0, 282, uiFont14())),
          _hint(ui::label(_box, 8, 130, 288, uiFont12())),
          _input(lv_textarea_create(_box))
    {
#ifdef FILES_LAYOUT_AUDIT
        lv_obj_add_flag(_box, layout_audit::overlay_flag);
        lv_obj_add_flag(_body, layout_audit::viewport_flag);
#endif
        lv_obj_add_flag(_body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(_body, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(_body, 3, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(_body, lv_color_hex(ui::secondary), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(_body, LV_OPA_COVER, LV_PART_SCROLLBAR);
        lv_obj_set_style_text_line_space(_text, 19 - lv_font_get_line_height(uiFont14()), 0);
        lv_obj_set_style_text_align(_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_style_all(_input);
        lv_obj_set_size(_input, 288, 29);
        lv_obj_set_pos(_input, 8, 54);
        lv_obj_set_style_bg_color(_input, lv_color_hex(ui::black), 0);
        lv_obj_set_style_bg_opa(_input, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_input, 1, 0);
        lv_obj_set_style_border_color(_input, lv_color_hex(ui::accent), 0);
        lv_obj_set_style_pad_hor(_input, 6, 0);
        lv_obj_set_style_pad_top(_input, 4, 0);
        lv_obj_set_style_text_font(_input, uiFont14(), 0);
        lv_obj_set_style_text_color(_input, lv_color_hex(ui::white), 0);
        lv_obj_set_style_border_side(_input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_border_width(_input, 2, LV_PART_CURSOR);
        lv_obj_set_style_border_color(_input, lv_color_hex(ui::accent), LV_PART_CURSOR);
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
        if (!visible) return;
        lv_obj_move_foreground(_box);
        PackageDialog dialog = package;
        if (deletion.active) {
            dialog = {PackageDialogMode::Result,
                      deletion.file.directory ? local("Delete folder?", "Smazat složku?") :
                                                local("Delete file?", "Smazat soubor?"),
                      deletion.file.name, false, DialogTone::Warning};
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
        lv_obj_set_style_text_font(_symbol, dialog.tone == DialogTone::Success ? LV_FONT_DEFAULT : uiFont14(), 0);
        lv_label_set_text(_symbol, toneSymbol(dialog.tone));
        for (auto* label : {_symbol, _title}) lv_obj_set_style_text_color(label, lv_color_hex(toneColor(dialog.tone)), 0);
        lv_obj_set_x(_title, symbol ? 28 : 8);
        lv_obj_set_width(_title, symbol ? 268 : 288);
        ui::line(_title, dialog.title, symbol ? 268 : 288, uiFont14());

        ui::hidden(_identity, dialog.identity.empty());
        ui::hidden(_meta, dialog.metadata.empty());
        ui::hidden(_warning, !dialog.rootScripts);
        ui::line(_identity, dialog.identity, 288, uiFont14());
        ui::line(_meta, dialog.metadata, 288, uiFont12());
        lv_label_set_text(_warning, local("! Install scripts run as root", "! Instalační skripty běží jako root"));

        const int top = rename.active ? 88 : dialog.rootScripts ? 83 : !dialog.metadata.empty() ? 66 :
                        !dialog.identity.empty() ? 50 : 31;
        lv_obj_set_y(_body, top);
        lv_obj_set_height(_body, ((128 - top) / 19) * 19);
        lv_label_set_text(_text, dialog.text.c_str());
        if (!previousMode || !previousTitle) lv_obj_scroll_to_y(_body, 0, LV_ANIM_OFF);
        lv_obj_update_layout(_box);
        if (_mode == PackageDialogMode::Installing) {
            lv_obj_scroll_by(_body, 0, -lv_obj_get_scroll_bottom(_body), LV_ANIM_OFF);
        }
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
                case PackageDialogMode::Transferring: hint = cancel; break;
                case PackageDialogMode::Result: hint = keyHints({canScroll ? "F↑ X↓" : "", back}, {}, 288); break;
                default: break;
            }
        }
        lv_label_set_text(_hint, hint.c_str());
    }

    void setName(const std::string& name)
    {
        if (!_editing || name == lv_textarea_get_text(_input)) return;
        _setting = true;
        lv_textarea_set_text(_input, name.c_str());
        lv_textarea_set_cursor_pos(_input, LV_TEXTAREA_CURSOR_LAST);
        _setting = false;
    }

    void error(const std::string& message)
    {
        lv_label_set_text(_text, message.c_str());
        lv_obj_set_style_text_color(_text, lv_color_hex(ui::error), 0);
    }

    void scroll(int offset) { lv_obj_scroll_by_bounded(_body, 0, -offset, LV_ANIM_OFF); }

private:
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
    lv_obj_t* _warning;
    lv_obj_t* _body;
    lv_obj_t* _text;
    lv_obj_t* _hint;
    lv_obj_t* _input;
    PackageDialogMode _mode = PackageDialogMode::Closed;
    std::string _last_title;
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
    _magic = std::make_unique<MagicView>(_root);
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
        self._magic_seen = serial;
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
}

void BrowserView::tick(uint32_t nowMs)
{
    if (_usb != _vm.hasRemovableVolumes()) updateChrome();
    if (_toast_duration && lv_tick_elaps(_toast_started) >= _toast_duration) {
        _toast_duration = 0;
        ui::hidden(_toast, true);
    }
    _magic->tick(nowMs);
}

void BrowserView::updateChrome()
{
    _usb = _vm.hasRemovableVolumes();
    const bool menu = _vm.actionMenuOpen().get();
    const bool places = _vm.inPlaces();
    const std::string title = menu ? (_vm.languageMenu() ? local("Language", "Jazyk") : local("Menu", "Nabídka")) :
                              places ? local("Places", "Umístění") : _vm.directoryTitle();
    const auto path = !menu && !places ? _vm.directoryCaption() : "";
    const int width = path.empty() ? 204 : std::clamp(textWidth(title, uiFont14()), 34, 158);
    lv_obj_set_width(_heading, width);
    ui::line(_heading, title, width, uiFont14());
    lv_obj_set_x(_path, 16 + width);
    lv_obj_set_width(_path, 296 - width);
    ui::line(_path, path, 296 - width, uiFont12());
    ui::hidden(_path, path.empty());
    ui::hidden(_free, !places || menu || !_usb);
    lv_label_set_text(_free, local("Free", "Volno"));
    const bool movement = menu ? _vm.actionCount() > 1 : _vm.entries().get().size() > 1;
    const auto keys = browserKeyHint(places, _vm.canEject() && !_vm.languageMenu(), _usb,
                                     _vm.selectedEntry() != nullptr, movement, menu);
    lv_label_set_text(_keys, keys.c_str());
}

void BrowserView::updateMenu(bool open)
{
    if (open) {
        std::vector<RowData> rows;
        for (const auto action : _vm.availableActions()) rows.push_back(actionRow(action));
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
    placeToast();
}

void BrowserView::updateStatus(const std::string& status)
{
    if (status.empty() || status == "Ready" || status == "Empty folder") return;
    const auto message = localizedMessage(status);
    if (_vm.pendingRename().get().active) {
        _dialog->error(message);
        return;
    }
    const bool success = status.rfind("Copied ", 0) == 0 || status.rfind("Moved ", 0) == 0 ||
                         status.rfind("Renamed ", 0) == 0 || status.rfind("Deleted ", 0) == 0 ||
                         status.rfind("Created ", 0) == 0 || status.rfind("Cut ", 0) == 0;
    ui::toast(_toast, message, 126, success ? ui::success : ui::white);
    _toast_started = lv_tick_get();
    _toast_duration = message.find('\n') == std::string::npos ? 3000 : 5000;
    placeToast();
}

int BrowserView::toastY(int height) const
{
    return _files && _files->selectedY() >= 0 && _files->selectedY() + rowHeight > 150 - height ? 20 : 150 - height;
}

void BrowserView::placeToast()
{
    const bool visible = _toast_duration && !_vm.modalInputActive();
    ui::hidden(_toast, !visible);
    if (!visible) return;
    lv_obj_update_layout(_root);
    const int height = lv_obj_get_height(_toast);
    lv_obj_set_y(_toast, toastY(height));
    lv_obj_move_foreground(_toast);
}

}  // namespace files
