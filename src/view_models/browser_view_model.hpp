#pragma once

#include "models/files_model.hpp"
#include "models/debian_package.hpp"
#include "view_models/view_model.hpp"
#include <tools/observable/single_observable.hpp>
#include <future>
#include <mutex>

namespace files {

enum class BrowserAction {
    Open,
    Copy,
    Cut,
    Paste,
    Rename,
    Info,
    Delete,
    Locations,
    Eject,
};

enum class PackageDialogMode { Closed, Reading, Review, Password, Installing, Result };

struct PackageDialog {
    PackageDialogMode mode = PackageDialogMode::Closed;
    std::string title;
    std::string text;
    bool allowInstall = false;
};

class BrowserViewModel : public ViewModel {
public:
    BrowserViewModel(FilesRouter& router, FilesModel& model);
    ~BrowserViewModel() override;

    PageId pageId() const override
    {
        return PageId::Browser;
    }

    void onEnter() override;
    void onKey(uint32_t key) override;
    void onKeyState(uint32_t key, bool pressed) override;
    void tick(uint32_t nowMs) override;

    smooth_ui_toolkit::SingleObservable<std::string>& currentDirectory()
    {
        return _model.browser().currentDirectory();
    }

    smooth_ui_toolkit::SingleObservable<std::vector<FileEntry>>& entries()
    {
        return _model.browser().entries();
    }

    smooth_ui_toolkit::SingleObservable<int>& selectedIndex()
    {
        return _model.browser().selectedIndex();
    }

    smooth_ui_toolkit::SingleObservable<std::string>& status()
    {
        return _model.browser().status();
    }

    smooth_ui_toolkit::SingleObservable<bool>& enterPressed()
    {
        return _enter_pressed;
    }

    smooth_ui_toolkit::SingleObservable<bool>& actionMenuOpen()
    {
        return _action_menu_open;
    }

    smooth_ui_toolkit::SingleObservable<int>& actionMenuSelectedIndex()
    {
        return _action_menu_selected_index;
    }

    smooth_ui_toolkit::SingleObservable<PendingDeleteFile>& pendingDelete()
    {
        return _pending_delete;
    }

    smooth_ui_toolkit::SingleObservable<PendingRenameFile>& pendingRename()
    {
        return _pending_rename;
    }

    smooth_ui_toolkit::SingleObservable<std::string>& pendingRenameName()
    {
        return _pending_rename_name;
    }

    smooth_ui_toolkit::SingleObservable<uint32_t>& magic()
    {
        return _magic;
    }

    auto& packageDialog() { return _package_dialog; }
    bool packageDialogOpen() const { return _package_dialog.get().mode != PackageDialogMode::Closed; }
    bool packagePasswordActive() const { return _package_dialog.get().mode == PackageDialogMode::Password; }
    auto& packageScroll() { return _package_scroll; }
    void editPackagePassword(uint32_t key, const char* utf8);
    const FileEntry* selectedEntry() const;
    bool canGoBack() const
    {
        return _model.browser().canGoBack();
    }
    int actionCount() const;
    BrowserAction actionAt(int index) const;
    void openSelected();
    void copySelectedToCurrentDirectory();
    void copySelected();
    void cutSelected();
    void pasteCopiedToCurrentDirectory();
    void showInfoSelected();
    void deleteSelected();
    void openActionMenu();
    void closeActionMenu();
    void requestDeleteSelected();
    void cancelDelete();
    void confirmDelete();
    void requestRenameSelected();
    void setPendingRenameName(std::string name);
    void cancelRename();
    void confirmRename();

private:
    FilesModel& _model;
    smooth_ui_toolkit::SingleObservable<bool> _enter_pressed{false};
    smooth_ui_toolkit::SingleObservable<bool> _action_menu_open{false};
    smooth_ui_toolkit::SingleObservable<int> _action_menu_selected_index{0};
    smooth_ui_toolkit::SingleObservable<PendingDeleteFile> _pending_delete{PendingDeleteFile{}};
    smooth_ui_toolkit::SingleObservable<PendingRenameFile> _pending_rename{PendingRenameFile{}};
    smooth_ui_toolkit::SingleObservable<std::string> _pending_rename_name{""};
    PendingCopyFile _pending_copy;
    smooth_ui_toolkit::SingleObservable<PackageDialog> _package_dialog{PackageDialog{}};
    smooth_ui_toolkit::SingleObservable<int> _package_scroll{0};
    std::unique_ptr<internal::PreparedDeb> _prepared_deb;
    std::string _package_password;
    int _password_attempts = 0;
    std::mutex _package_log_mutex;
    std::string _package_log;
    // The workers finish before the state they use is destroyed.
    std::future<internal::DebInspection> _package_read;
    std::future<internal::DebInstallResult> _package_install;
    uint32_t _last_locations_check = 0;
    uint32_t _last_package_update = 0;
    smooth_ui_toolkit::SingleObservable<uint32_t> _magic{0};
    int _held_selection_direction  = 0;
    uint32_t _next_selection_at_ms = 0;
    uint32_t _magic_count          = 0;

    std::vector<BrowserAction> availableActions() const;
    void handlePackageKey(uint32_t key);
    void startPackageRead(const FileEntry& entry);
    void startPackageInstall();
    void tickPackage(uint32_t nowMs);
    void selectPreviousAction();
    void selectNextAction();
    void activateSelectedAction();
    void selectByDirection(int direction);
    void clearHeldSelection();
    bool canGenerateMagic() const;
    void generateMagic();
};

}  // namespace files
