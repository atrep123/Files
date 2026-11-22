#include "preview/info/info_preview.hpp"

#include "core/files_language.hpp"
#include "views/ui_style.hpp"
#ifdef FILES_LAYOUT_AUDIT
#include "layout_audit/layout_audit.hpp"
#endif

#include <ctime>
#include <iomanip>
#include <sstream>

namespace files {
namespace {

std::string modifiedText(int64_t unixSec)
{
    const auto time = static_cast<std::time_t>(unixSec);
    std::tm value{};
    if (unixSec <= 0 || !localtime_r(&time, &value)) return "–";
    std::ostringstream text;
    text << std::put_time(&value, "%Y-%m-%d %H:%M");
    return text.str();
}

const char* kindText(FileKind kind)
{
    switch (kind) {
        case FileKind::Directory: return local("Folder", "Složka");
        case FileKind::Image: return local("Image", "Obrázek");
        case FileKind::Audio: return local("Audio", "Zvuk");
        case FileKind::Video: return "Video";
        case FileKind::Text: return "Text";
        case FileKind::Document: return local("Document", "Dokument");
        case FileKind::Archive: return local("Archive", "Archiv");
        case FileKind::Binary: return local("Binary", "Binární");
        default: return local("Unknown", "Neznámý");
    }
}

class InfoPreviewPage : public PreviewPage {
public:
    explicit InfoPreviewPage(FileEntry file) : _file(std::move(file)), _title(local("Details", "Podrobnosti")) {}
    const std::string& title() const override { return _title; }

    void attach(lv_obj_t* parent) override
    {
        _root = ui::box(parent, 0, 0, 320, 170, ui::black);
        ui::caption(_root, 0, _title, uiFont14());
        _body = ui::box(_root, 0, 20, 320, 130, ui::black);
        lv_obj_add_flag(_body, LV_OBJ_FLAG_SCROLLABLE);
#ifdef FILES_LAYOUT_AUDIT
        lv_obj_add_flag(_body, layout_audit::viewport_flag);
#endif
        lv_obj_set_scroll_dir(_body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(_body, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(_body, 3, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(_body, lv_color_hex(ui::secondary), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(_body, LV_OPA_COVER, LV_PART_SCROLLBAR);

        auto* name = ui::label(_body, 8, 4, 300, uiFont14());
        lv_label_set_text(name, _file.name.c_str());
        lv_obj_update_layout(_body);
        const int y = lv_obj_get_height(name) + 10;
        auto* fields = ui::label(_body, 8, y, 107, uiFont12(), ui::secondary);
        lv_label_set_text(fields, local("Kind\nSize\nModified\nHidden\nPath", "Typ\nVelikost\nZměněno\nSkrytý\nCesta"));
        auto* values = ui::label(_body, 116, y, 191, uiMonoFont12());
        const std::string details = std::string(kindText(_file.kind)) + "\n" +
                                    (_file.directory ? "–" : sizeInUnits(_file.size)) + "\n" +
                                    modifiedText(_file.modifiedUnixSec) + "\n" +
                                    (_file.hidden ? local("Yes", "Ano") : local("No", "Ne"));
        lv_label_set_text(values, details.c_str());
        lv_obj_set_style_text_line_space(fields, 17 - lv_font_get_line_height(uiFont12()), 0);
        lv_obj_set_style_text_line_space(values, 17 - lv_font_get_line_height(uiMonoFont12()), 0);
        auto* path = ui::label(_body, 116, y + 69, 191, uiMonoFont12(), ui::secondary);
        lv_label_set_text(path, _file.path.c_str());
        lv_obj_update_layout(_body);
        ui::caption(_root, 150, keyHints({lv_obj_get_scroll_bottom(_body) > 0 ? "F↑ X↓" : "",
                                         local("Esc Back", "Esc Zpět")}, {}));
    }

    void detach() override
    {
        if (_root) lv_obj_delete(_root);
        _root = _body = nullptr;
    }

    void onKey(uint32_t key, FilesRouter& router) override
    {
        switch (key) {
            case files_key::Left:
            case '\x1b': router.back(); break;
            case files_key::Up: lv_obj_scroll_by_bounded(_body, 0, 17, LV_ANIM_OFF); break;
            case files_key::Down: lv_obj_scroll_by_bounded(_body, 0, -17, LV_ANIM_OFF); break;
            case files_key::PageUp: lv_obj_scroll_by_bounded(_body, 0, 119, LV_ANIM_OFF); break;
            case files_key::PageDown: lv_obj_scroll_by_bounded(_body, 0, -119, LV_ANIM_OFF); break;
            default: break;
        }
    }

private:
    FileEntry _file;
    std::string _title;
    lv_obj_t* _root = nullptr;
    lv_obj_t* _body = nullptr;
};

class InfoPreviewSupport : public PreviewSupport {
public:
    const char* id() const override { return "info"; }
    bool supports(const FileEntry&) const override { return true; }
    std::unique_ptr<PreviewPage> open(const FileEntry& file) const override
    {
        return std::make_unique<InfoPreviewPage>(file);
    }
};

}  // namespace

std::unique_ptr<PreviewSupport> createInfoPreviewSupport()
{
    return std::make_unique<InfoPreviewSupport>();
}

}  // namespace files