#include "view_models/browser_view_model.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace files {
namespace {

constexpr uint32_t kHoldRepeatDelayMs = 320;
constexpr uint32_t kHoldRepeatMs      = 90;
// Leave one row of context when paging through the five-row browser window.
constexpr int kBrowserPageStep = 4;

}  // namespace

BrowserViewModel::BrowserViewModel(FilesRouter& router, FilesModel& model) : ViewModel(router), _model(model)
{
}

BrowserViewModel::~BrowserViewModel()
{
    if (_transfer_control) {
        _transfer_control->cancel();
    }
    internal::clearSecret(_package_password);
}

void BrowserViewModel::onEnter()
{
    clearHeldSelection();
    _magic_count = 0;
    spdlog::info("BrowserViewModel enter: {}", _model.browser().currentDirectory().get());
    _model.browser().refresh(true);
}

void BrowserViewModel::onKey(uint32_t key)
{
    if (packageDialogOpen()) {
        handlePackageKey(key);
        return;
    }
    if (_pending_rename.get().active) {
        switch (key) {
            case '\x1b':
            case files_key::Left:
                cancelRename();
                break;
            case '\r':
            case '\n':
                confirmRename();
                break;
            default:
                break;
        }
        return;
    }

    if (_pending_delete.get().active) {
        switch (key) {
            case '\x1b':
            case files_key::Left:
                cancelDelete();
                break;
            case '\r':
            case '\n':
                confirmDelete();
                break;
            default:
                break;
        }
        return;
    }

    if (_action_menu_open.get()) {
        switch (key) {
            case files_key::Up:
                selectPreviousAction();
                break;
            case files_key::Down:
                selectNextAction();
                break;
            case '\x1b':
            case files_key::Left:
            case '\t':
                closeActionMenu();
                break;
            case '\r':
            case '\n':
                activateSelectedAction();
                break;
            default:
                break;
        }
        return;
    }

    switch (key) {
        case files_key::Up:
            _model.browser().selectPrevious();
            break;
        case files_key::Down:
            _model.browser().selectNext();
            break;
        case files_key::PageUp:
            _model.browser().selectPageUp(kBrowserPageStep);
            break;
        case files_key::PageDown:
            _model.browser().selectPageDown(kBrowserPageStep);
            break;
        case '\x1b': {
            const auto result = _model.browser().goBack();
            if (!result) {
                status().set(result.message);
            }
            break;
        }
        case '\r':
        case '\n':
            openSelected();
            break;
        case '\t':
            openActionMenu();
            break;
        case ' ':
            if (canGenerateMagic()) {
                ++_magic_count;
                if (_magic_count >= 3) {
                    _magic_count = 0;
                    generateMagic();
                }
            } else {
                _magic_count = 0;
            }
            break;
        default:
            break;
    }
}

void BrowserViewModel::onKeyState(uint32_t key, bool pressed)
{
    if (packageDialogOpen()) {
        return;
    }
    if ((key == '\r' || key == '\n') && !_pending_delete.get().active && !_pending_rename.get().active) {
        _enter_pressed.set(pressed);
        return;
    }

    int direction = 0;
    switch (key) {
        case files_key::Up:
            direction = -1;
            break;
        case files_key::Down:
            direction = 1;
            break;
        default:
            return;
    }

    if (pressed) {
        if (_pending_delete.get().active || _pending_rename.get().active) {
            clearHeldSelection();
            return;
        }
        _held_selection_direction = direction;
        _next_selection_at_ms     = 0;
    } else if (_held_selection_direction == direction) {
        clearHeldSelection();
    }
}

void BrowserViewModel::tick(uint32_t nowMs)
{
    if (_storage_operation.valid()) {
        tickStorage(nowMs);
        return;
    }
    if (packageDialogOpen()) {
        tickPackage(nowMs);
        return;
    }
    if (_model.browser().inLocations() && !_action_menu_open.get() && nowMs - _last_locations_check >= 1000) {
        _last_locations_check = nowMs;
        _model.browser().refreshLocations();
    }
    if (_pending_delete.get().active || _pending_rename.get().active) {
        clearHeldSelection();
        return;
    }

    if (_held_selection_direction != 0 && _next_selection_at_ms == 0) {
        _next_selection_at_ms = nowMs + kHoldRepeatDelayMs;
        return;
    }

    if (_held_selection_direction != 0 && nowMs >= _next_selection_at_ms) {
        selectByDirection(_held_selection_direction);
        _next_selection_at_ms = nowMs + kHoldRepeatMs;
    }
}

const FileEntry* BrowserViewModel::selectedEntry() const
{
    return _model.browser().selectedEntry();
}

std::vector<BrowserAction> BrowserViewModel::availableActions() const
{
    const auto* entry = selectedEntry();
    if (_model.browser().inLocations()) {
        if (!entry) {
            return {};
        }
        return entry->mountRoot ? std::vector<BrowserAction>{BrowserAction::Open, BrowserAction::Eject}
                                : std::vector<BrowserAction>{BrowserAction::Open};
    }
    if (entry && entry->mountRoot) {
        return {BrowserAction::Open, BrowserAction::Eject, BrowserAction::Locations};
    }

    std::vector<BrowserAction> actions;
    if (entry) {
        actions = {BrowserAction::Open, BrowserAction::Info, BrowserAction::Copy, BrowserAction::Cut};
    }
    if (_pending_copy.active) {
        actions.push_back(BrowserAction::Paste);
    }
    if (entry) {
        actions.insert(actions.end(), {BrowserAction::Rename, BrowserAction::Delete});
    }
    actions.push_back(BrowserAction::Locations);
    return actions;
}

int BrowserViewModel::actionCount() const
{
    return static_cast<int>(availableActions().size());
}

BrowserAction BrowserViewModel::actionAt(int index) const
{
    const auto actions = availableActions();
    return actions.empty() ? BrowserAction::Open
                           : actions[std::clamp(index, 0, static_cast<int>(actions.size()) - 1)];
}

void BrowserViewModel::openSelected()
{
    FileEntry openedFile;
    const FileOperationResult result = _model.browser().openSelected(&openedFile);
    if (!result) {
        _model.browser().status().set(result.message);
        return;
    }

    if (!openedFile.path.empty()) {
        if (openedFile.extension == ".deb") {
            startPackageRead(openedFile);
        } else if (_model.preview().open(openedFile)) {
            _router.push(PageId::Preview);
        } else {
            _model.browser().status().set("No preview available");
            spdlog::info("BrowserViewModel: no preview support for path='{}'", openedFile.path);
        }
    }
}

void BrowserViewModel::copySelectedToCurrentDirectory()
{
    const auto result = _model.browser().copySelectedTo(_model.browser().currentDirectory().get());
    if (!result) {
        _model.browser().status().set(result.message);
    }
}

void BrowserViewModel::copySelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected || selected->mountRoot || _model.browser().inLocations()) {
        return;
    }

    _pending_copy = PendingCopyFile{true, *selected};
    _model.browser().status().set("Copied " + selected->name);
}

void BrowserViewModel::cutSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected || selected->mountRoot || _model.browser().inLocations()) {
        return;
    }

    _pending_copy = PendingCopyFile{true, *selected, true};
    _model.browser().status().set("Cut " + selected->name);
}

void BrowserViewModel::pasteCopiedToCurrentDirectory()
{
    if (!_pending_copy.active) {
        _model.browser().status().set("Nothing to paste");
        return;
    }

    clearHeldSelection();
    _transfer_control = std::make_shared<internal::TransferControl>();
    _package_dialog.set({PackageDialogMode::Transferring, _pending_copy.cut ? "Move" : "Copy",
                         _pending_copy.file.name + "\n0 bytes copied"});
    _storage_operation = std::async(std::launch::async,
        [entry = _pending_copy.file, cut = _pending_copy.cut, directory = currentDirectory().get(),
         mountId = _model.browser().currentMountId(), control = _transfer_control] {
            return FileBrowserModel::performTransfer(entry, directory, mountId, cut, control.get());
        });
}

void BrowserViewModel::showInfoSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return;
    }

    const FileEntry detailed = _model.browser().entryWithMetadata(*selected);
    if (_model.preview().openInfo(detailed)) {
        _router.push(PageId::Preview);
    }
}

void BrowserViewModel::deleteSelected()
{
    const auto result = _model.browser().deleteSelected();
    if (!result) {
        _model.browser().status().set(result.message);
    }
}

void BrowserViewModel::openActionMenu()
{
    if (actionCount() <= 0) {
        return;
    }
    _action_menu_selected_index.set(0);
    _action_menu_open.set(true);
}

void BrowserViewModel::closeActionMenu()
{
    _action_menu_open.set(false);
}

void BrowserViewModel::requestDeleteSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return;
    }

    _pending_delete.set(PendingDeleteFile{true, *selected});
    closeActionMenu();
}

void BrowserViewModel::cancelDelete()
{
    _pending_delete.set(PendingDeleteFile{});
}

void BrowserViewModel::confirmDelete()
{
    if (!_pending_delete.get().active) {
        return;
    }

    _pending_delete.set(PendingDeleteFile{});
    deleteSelected();
}

void BrowserViewModel::requestRenameSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return;
    }

    _pending_rename.set(PendingRenameFile{true, *selected, selected->name});
    _pending_rename_name.set(selected->name);
    closeActionMenu();
}

void BrowserViewModel::setPendingRenameName(std::string name)
{
    if (!_pending_rename.get().active || _pending_rename_name.get() == name) {
        return;
    }
    _pending_rename_name.set(std::move(name));
}

void BrowserViewModel::cancelRename()
{
    _pending_rename.set(PendingRenameFile{});
    _pending_rename_name.set("");
}

void BrowserViewModel::confirmRename()
{
    PendingRenameFile pending = _pending_rename.get();
    if (!pending.active) {
        return;
    }

    const auto result = _model.browser().renameSelectedTo(_pending_rename_name.get());
    if (!result) {
        _model.browser().status().set(result.message);
        return;
    }
    _pending_rename.set(PendingRenameFile{});
    _pending_rename_name.set("");
}

void BrowserViewModel::selectPreviousAction()
{
    int next = _action_menu_selected_index.get() - 1;
    if (next < 0) {
        next = actionCount() - 1;
    }
    _action_menu_selected_index.set(next);
}

void BrowserViewModel::selectNextAction()
{
    int next = _action_menu_selected_index.get() + 1;
    if (next >= actionCount()) {
        next = 0;
    }
    _action_menu_selected_index.set(next);
}

void BrowserViewModel::activateSelectedAction()
{
    switch (actionAt(_action_menu_selected_index.get())) {
        case BrowserAction::Open:
            closeActionMenu();
            openSelected();
            break;
        case BrowserAction::Copy:
            closeActionMenu();
            copySelected();
            break;
        case BrowserAction::Cut:
            closeActionMenu();
            cutSelected();
            break;
        case BrowserAction::Paste:
            closeActionMenu();
            pasteCopiedToCurrentDirectory();
            break;
        case BrowserAction::Rename:
            requestRenameSelected();
            break;
        case BrowserAction::Info:
            closeActionMenu();
            showInfoSelected();
            break;
        case BrowserAction::Delete:
            requestDeleteSelected();
            break;
        case BrowserAction::Locations:
            closeActionMenu();
            _model.browser().showLocations();
            break;
        case BrowserAction::Eject:
            closeActionMenu();
            startEject();
            break;
    }
}

void BrowserViewModel::startEject()
{
    const auto* entry = selectedEntry();
    if (!entry || !entry->mountRoot) {
        return;
    }
    clearHeldSelection();
    _eject_progress = "Checking " + entry->name + "...";
    _package_dialog.set({PackageDialogMode::Ejecting, "Eject drive", _eject_progress});
    _storage_operation = std::async(std::launch::async, [this, mountId = entry->mountId] {
        return FileBrowserModel::ejectDrive(mountId, [this](const std::string& message) {
            std::lock_guard<std::mutex> lock(_eject_mutex);
            _eject_progress = message;
        });
    });
}

void BrowserViewModel::tickStorage(uint32_t nowMs)
{
    auto dialog = _package_dialog.get();
    const bool transfer = dialog.mode == PackageDialogMode::Transferring;
    if (_storage_operation.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        FileOperationResult result;
        try {
            result = _storage_operation.get();
        } catch (const std::exception& error) {
            result = {FileOperationStatus::Failed, error.what()};
        }
        const bool cancelled = transfer && _transfer_control->cancelled();
        _transfer_control.reset();
        _model.browser().refresh(!transfer);
        status().set(result.message);
        if (result && transfer && _pending_copy.cut) {
            _pending_copy = {};
        }
        if (result || cancelled) {
            _package_dialog.set({});
        } else {
            _package_dialog.set({PackageDialogMode::Result, dialog.title, result.message});
        }
        return;
    }
    if (nowMs - _last_package_update < 125) {
        return;
    }
    _last_package_update = nowMs;
    if (transfer) {
        if (_transfer_control->cancelled()) {
            dialog.text = "Cancelling transfer...";
        } else {
            dialog.text = _pending_copy.file.name + "\n" + std::to_string(_transfer_control->bytes()) + " bytes copied";
        }
    } else {
        std::lock_guard<std::mutex> lock(_eject_mutex);
        dialog.text = _eject_progress;
    }
    if (dialog.text != _package_dialog.get().text) {
        _package_dialog.set(std::move(dialog));
    }
}

void BrowserViewModel::startPackageRead(const FileEntry& entry)
{
    clearHeldSelection();
    _password_attempts = 0;
    _package_dialog.set({PackageDialogMode::Reading, "Debian package", "Reading package..."});
    _package_read = std::async(std::launch::async, [path = entry.path, mountId = entry.mountId] {
        return internal::inspectDeb(path, mountId);
    });
}

void BrowserViewModel::startPackageInstall()
{
    if (!_prepared_deb) {
        return;
    }
    _package_log = "Preparing installation...\n";
    std::string password;
    password.swap(_package_password);
    _package_dialog.set({PackageDialogMode::Installing, "Installing package", _package_log});
    _package_install = std::async(std::launch::async, [this, password = std::move(password)]() mutable {
        return internal::installDeb(*_prepared_deb, password, [this](const std::string& chunk) {
            std::lock_guard<std::mutex> lock(_package_log_mutex);
            _package_log += internal::cleanCommandText(chunk);
            constexpr size_t maxLog = 8192;
            if (_package_log.size() > maxLog) {
                const auto start = _package_log.find('\n', _package_log.size() - maxLog);
                _package_log.erase(0, start == std::string::npos ? _package_log.size() - maxLog : start + 1);
            }
        });
    });
}

void BrowserViewModel::tickPackage(uint32_t nowMs)
{
    constexpr auto noWait = std::chrono::milliseconds(0);
    try {
        if (_package_read.valid() && _package_read.wait_for(noWait) == std::future_status::ready) {
            auto inspection = _package_read.get();
            _prepared_deb = std::move(inspection.package);
            _package_dialog.set({_prepared_deb ? PackageDialogMode::Review : PackageDialogMode::Result, "Debian package",
                                 std::move(inspection.text), inspection.installable});
        }
        if (_package_install.valid() && _package_install.wait_for(noWait) == std::future_status::ready) {
            const auto result = _package_install.get();
            using State = internal::DebInstallState;
            if (result.state == State::PasswordRequired ||
                (result.state == State::AuthenticationFailed && ++_password_attempts < 3)) {
                _package_password.reserve(129);
                const bool retry = result.state == State::AuthenticationFailed;
                _package_dialog.set({PackageDialogMode::Password, "System password",
                                     retry ? "Incorrect password. Try again.\n\n" : "Enter your sudo password.\n\n"});
            } else {
                _prepared_deb.reset();
                _package_dialog.set({PackageDialogMode::Result,
                                     result.state == State::Installed ? "Installation complete" : "Installation failed",
                                     result.state == State::AuthenticationFailed ? "Sudo authentication failed"
                                                                                : result.message});
            }
        }
    } catch (const std::exception& error) {
        _prepared_deb.reset();
        _package_dialog.set({PackageDialogMode::Result, "Package operation failed", error.what()});
    }

    if (_package_dialog.get().mode == PackageDialogMode::Installing && nowMs - _last_package_update >= 125) {
        _last_package_update = nowMs;
        auto dialog = _package_dialog.get();
        {
            std::lock_guard<std::mutex> lock(_package_log_mutex);
            if (dialog.text == _package_log) {
                return;
            }
            dialog.text = _package_log;
        }
        _package_dialog.set(std::move(dialog));
    }
}

void BrowserViewModel::editPackagePassword(uint32_t key, const char* utf8)
{
    if (key == 8 && !_package_password.empty()) {
        _package_password.back() = '\0';
        _package_password.pop_back();
    } else if (utf8 && utf8[0] >= ' ' && utf8[0] < 0x7f && utf8[1] == '\0' && _package_password.size() < 128) {
        _package_password.push_back(utf8[0]);
    } else {
        return;
    }
    auto dialog = _package_dialog.get();
    dialog.text = dialog.text.substr(0, dialog.text.find("\n\n") + 2) + std::string(_package_password.size(), '*');
    _package_dialog.set(std::move(dialog));
}

void BrowserViewModel::handlePackageKey(uint32_t key)
{
    const auto mode = _package_dialog.get().mode;
    if (mode == PackageDialogMode::Transferring) {
        if (key == '\x1b' && _transfer_control->cancel()) {
            auto dialog = _package_dialog.get();
            dialog.text = "Cancelling transfer...";
            _package_dialog.set(std::move(dialog));
        }
        return;
    }
    if (mode == PackageDialogMode::Reading || mode == PackageDialogMode::Installing ||
        mode == PackageDialogMode::Ejecting) {
        return;
    }
    if (key == '\x1b' || (key == files_key::Left && mode != PackageDialogMode::Password)) {
        internal::clearSecret(_package_password);
        _prepared_deb.reset();
        _package_dialog.set({});
        return;
    }
    if (key == '\r' || key == '\n') {
        if ((mode == PackageDialogMode::Review && _package_dialog.get().allowInstall) ||
            (mode == PackageDialogMode::Password && !_package_password.empty())) {
            startPackageInstall();
        }
        return;
    }
    if (mode != PackageDialogMode::Password) {
        if (key == files_key::Up || key == files_key::Down) {
            _package_scroll.set(key == files_key::Up ? -18 : 18);
        } else if (key == files_key::PageUp || key == files_key::PageDown) {
            _package_scroll.set(key == files_key::PageUp ? -96 : 96);
        }
    }
}

void BrowserViewModel::selectByDirection(int direction)
{
    if (direction < 0) {
        if (_action_menu_open.get()) {
            selectPreviousAction();
        } else {
            _model.browser().selectPrevious();
        }
    } else if (direction > 0) {
        if (_action_menu_open.get()) {
            selectNextAction();
        } else {
            _model.browser().selectNext();
        }
    }
}

void BrowserViewModel::clearHeldSelection()
{
    _held_selection_direction = 0;
    _next_selection_at_ms     = 0;
}

bool BrowserViewModel::canGenerateMagic() const
{
    return !_pending_delete.get().active && !_pending_rename.get().active && !_action_menu_open.get();
}

void BrowserViewModel::generateMagic()
{
    _magic.set(_magic.get() + 1);
    spdlog::info("BrowserViewModel: magic trigger serial={}", _magic.get());
}

}  // namespace files
