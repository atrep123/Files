#include "view_models/browser_view_model.hpp"

#include "core/files_language.hpp"
#include "assets/font_assets.hpp"
#include "views/browser_text.hpp"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <chrono>

namespace files {
namespace {

constexpr uint32_t kHoldRepeatDelayMs = 320;
constexpr uint32_t kHoldRepeatMs      = 90;
constexpr uint32_t kVolumePollMs      = 750;
// Leave one row of context when paging through the five-row browser window.
constexpr int kBrowserPageStep = 4;

std::string volumeNoticeLine(std::string name, std::string_view prefix, std::string_view suffix)
{
    const std::string fixed = std::string(prefix) + std::string(suffix);
    const auto* font = uiFont12();
    // Reserve the symbol and the event itself before fitting the disk identity.
    name = fitText(std::move(name), 296 - textWidth("✓ " + fixed, font), font, 34 - textLength(fixed));
    return std::string(prefix) + name + std::string(suffix);
}

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
    _magic.set(0);
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

    if (_magic.get()) {
        if (key == '\x1b') _magic.set(0);
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
                if (result.message != "Already at Places") status().set(result.message);
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

void BrowserViewModel::showVolumeNotice(VolumeNotice notice)
{
    if (_volume_notice.get().text.empty()) {
        _notice_started_at = _last_tick;
        _volume_notice.set(std::move(notice));
    } else {
        _queued_notices.push_back(std::move(notice));
    }
}

void BrowserViewModel::advanceVolumeNotice()
{
    _notice_started_at = _last_tick;
    if (_queued_notices.empty()) {
        _volume_notice.set({});
    } else {
        _volume_notice.set(std::move(_queued_notices.front()));
        _queued_notices.pop_front();
    }
}

bool BrowserViewModel::canOpenUsb() const
{
    const auto mode = _package_dialog.get().mode;
    return hasRemovableVolumes() && !_pending_rename.get().active &&
           (mode == PackageDialogMode::Closed || mode == PackageDialogMode::Review ||
            mode == PackageDialogMode::Result || mode == PackageDialogMode::ConfirmEject);
}

bool BrowserViewModel::openUsb()
{
    if (!canOpenUsb()) {
        return false;
    }

    if (packageDialogOpen()) {
        handlePackageKey('\x1b');
    }
    if (_pending_delete.get().active) {
        cancelDelete();
    }
    closeActionMenu();
    clearHeldSelection();
    const auto result = _model.browser().openRemovableVolumes();
    _queued_notices.erase(std::remove_if(_queued_notices.begin(), _queued_notices.end(),
        [](const auto& notice) { return notice.connectedMountId != 0; }), _queued_notices.end());
    if (_volume_notice.get().connectedMountId) {
        advanceVolumeNotice();
    }
    if (!result) {
        showVolumeNotice({result.message});
    } else if (_router.page() == PageId::Preview) {
        _router.back();
    }
    return true;
}

bool BrowserViewModel::ejectUsb()
{
    if (!canEject() || packageDialogOpen() || _pending_delete.get().active || _pending_rename.get().active) {
        return false;
    }
    if (_router.page() == PageId::Preview) {
        _router.back();
    }
    closeActionMenu();
    requestEject();
    return true;
}

void BrowserViewModel::pollVolumes(uint32_t nowMs)
{
    _last_tick = nowMs;
    if (nowMs - _last_volume_check >= kVolumePollMs) {
        _last_volume_check = nowMs;
        const auto previousMountId = _model.browser().currentMountId();
        const auto changes = _model.browser().updateVolumes(internal::removableVolumes());
        if (changes.leftVolume || (_model.browser().inPlaces() &&
                                   (!changes.removed.empty() || !changes.arrived.empty()))) {
            clearHeldSelection();
            if (_action_menu_open.get()) {
                closeActionMenu();
            }
        }
        const bool closedPreview = changes.leftVolume && _router.page() == PageId::Preview;
        bool closedEdit = false;
        if (changes.leftVolume) {
            const bool deleting = _pending_delete.get().active;
            if (deleting || _pending_rename.get().active) {
                closedEdit = true;
                const auto* title = deleting ? local("Deletion cancelled", "Mazání zrušeno") :
                    _pending_rename.get().creating ? local("Folder creation interrupted", "Vytváření složky přerušeno") :
                                                     local("Rename interrupted", "Přejmenování přerušeno");
                cancelDelete();
                cancelRename();
                const auto source = std::find_if(changes.removed.begin(), changes.removed.end(),
                    [previousMountId](const auto& drive) { return drive.mount.id == previousMountId; });
                const auto text = source == changes.removed.end() ? local("Drive disconnected", "Disk odpojen") :
                    local("Drive ", "Disk ") + source->name + local(" disconnected.", " byl odpojen.");
                _package_dialog.set({PackageDialogMode::Result, title,
                    deleting ? text + local("\nDeletion did not start.", "\nMazání se nespustilo.") : text,
                    false, DialogTone::Error});
            }
            if (_router.page() == PageId::Preview) {
                _router.back();
            }
        }

        for (const auto& drive : changes.removed) {
            const auto id = drive.mount.id;
            _queued_notices.erase(std::remove_if(_queued_notices.begin(), _queued_notices.end(),
                [id](const auto& notice) { return notice.connectedMountId == id; }), _queued_notices.end());
        }
        for (const auto& drive : changes.removed) {
            const auto id = drive.mount.id;
            const auto mode = _package_dialog.get().mode;
            if (id == _package_source_mount_id &&
                (mode == PackageDialogMode::Review || mode == PackageDialogMode::Password)) {
                _prepared_deb.reset();
                internal::clearSecret(_package_password);
                _package_dialog.set({PackageDialogMode::Result, local("Debian package", "Balíček Debian"),
                                     local("Source drive disconnected: ", "Zdrojový disk odpojen: ") + drive.name, false,
                                     DialogTone::Error, _package_file_name});
            }
            if (_volume_notice.get().connectedMountId == id) {
                advanceVolumeNotice();
            }
            const bool transferDrive = mode == PackageDialogMode::Transferring &&
                (id == _pending_copy.file.mountId || id == _transfer_destination_mount_id);
            const bool packageDrive = id == _package_source_mount_id &&
                (mode == PackageDialogMode::Reading || mode == PackageDialogMode::Review ||
                 mode == PackageDialogMode::Password);
            const bool ejectDrive = id == _eject_mount_id &&
                (mode == PackageDialogMode::ConfirmEject || mode == PackageDialogMode::Ejecting);
            if (!transferDrive && !ejectDrive && !packageDrive && !(closedEdit && id == previousMountId)) {
                if (closedPreview && id == previousMountId) {
                    auto text = drive.name + local(" unplugged · preview closed", " odpojen · náhled zavřen");
                    if (hasRemovableVolumes() || textLength(text) > 34 || textWidth(text, uiFont12()) > 280) {
                        text = volumeNoticeLine(drive.name, "", local(" disconnected", " odpojen")) +
                               local("\nPreview closed", "\nNáhled zavřen");
                    }
                    _volume_notice.set({});
                    _notice_started_at = _last_tick;
                    _volume_notice.set({std::move(text), 0, DialogTone::Warning});
                } else {
                    showVolumeNotice({volumeNoticeLine(drive.name, local("Drive ", "Disk "),
                        local(" disconnected", " odpojen")), 0, DialogTone::Warning});
                }
            }
        }
        for (const auto& drive : changes.arrived) {
            showVolumeNotice({volumeNoticeLine(drive.name, local("Drive ", "Disk "), local(" connected", " připojen")) +
                              "\n", drive.mount.id, DialogTone::Success});
        }
        if (_package_dialog.get().mode == PackageDialogMode::ConfirmEject && !_model.browser().volume(_eject_mount_id)) {
            _eject_mount_id = 0;
            _package_dialog.set({PackageDialogMode::Result, local("Drive disconnected", "Disk byl odpojen"),
                                 _eject_name, false, DialogTone::Error});
        }
    }
    if (!_volume_notice.get().text.empty() && nowMs - _notice_started_at >= _volume_notice.get().duration()) {
        advanceVolumeNotice();
    }
}

std::string BrowserViewModel::directoryTitle()
{
    if (const auto* drive = _model.browser().volume(_model.browser().currentMountId())) return drive->name;
    const auto& path = currentDirectory().get();
    const auto* home = std::getenv("HOME");
    if (home && path == home) return local("Home", "Domů");
    return path == "/" ? local("File system", "Souborový systém") : local("Files", "Soubory");
}

std::string BrowserViewModel::directoryCaption()
{
    const auto& path = currentDirectory().get();
    if (const auto* drive = _model.browser().volume(_model.browser().currentMountId())) {
        const auto relative = std::filesystem::path(path).lexically_relative(drive->mount.target);
        return relative.empty() || relative == "." ? "" : "/ " + relative.string();
    }
    const auto* home = std::getenv("HOME");
    if (home && (path == home || path.rfind(std::string(home) + "/", 0) == 0)) {
        return path == home ? "" : "~" + path.substr(std::string(home).size());
    }
    return path == "/" ? "" : path;
}

std::string BrowserViewModel::menuContext() const
{
    if (_language_menu) return {};
    const auto* entry = selectedEntry();
    if (!entry) {
        if (const auto* drive = _model.browser().volume(_model.browser().currentMountId())) return drive->name;
    }
    if (!entry) return {};
    return inPlaces() && !entry->mountRoot ? localizedMessage(entry->name) : entry->name;
}

bool BrowserViewModel::canEject() const
{
    const auto* entry = selectedEntry();
    return _model.browser().currentMountId() || (entry && entry->mountRoot);
}

const FileEntry* BrowserViewModel::selectedEntry() const
{
    return _model.browser().selectedEntry();
}

std::vector<BrowserAction> BrowserViewModel::availableActions() const
{
    if (_language_menu) return {BrowserAction::SystemLanguage, BrowserAction::English, BrowserAction::Czech};
    const auto* entry = selectedEntry();
    if (_model.browser().inPlaces()) {
        if (entry && entry->mountRoot) {
            return {BrowserAction::Eject, BrowserAction::Open, BrowserAction::Language};
        }
        return {BrowserAction::Open, BrowserAction::Language};
    }
    if (entry && entry->mountRoot) {
        return {BrowserAction::Eject, BrowserAction::Open, BrowserAction::Places, BrowserAction::Language};
    }

    std::vector<BrowserAction> actions;
    if (_model.browser().currentMountId()) {
        actions.push_back(BrowserAction::Eject);
    }
    if (entry) {
        actions.insert(actions.end(), {BrowserAction::Open, BrowserAction::Info, BrowserAction::Copy, BrowserAction::Cut});
    }
    if (_pending_copy.active) {
        actions.push_back(BrowserAction::Paste);
    }
    if (entry) {
        actions.insert(actions.end(), {BrowserAction::Rename, BrowserAction::Delete});
    }
    actions.insert(actions.end(), {BrowserAction::Places, BrowserAction::NewFolder, BrowserAction::Language});
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
    clearStatus();
    FileEntry openedFile;
    const FileOperationResult result = _model.browser().openSelected(&openedFile);
    if (!result) {
        if (result.message == "Permission denied") {
            const auto* entry = selectedEntry();
            _package_dialog.set({PackageDialogMode::Result, local("Cannot open", "Nelze otevřít"),
                                 localizedMessage(result.message), false, DialogTone::Error, entry ? entry->name : ""});
        } else {
            status().set(result.message);
        }
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
    if (!selected || selected->mountRoot || _model.browser().inPlaces()) {
        return;
    }

    _pending_copy = PendingCopyFile{true, *selected};
    _model.browser().status().set("Clipboard: " + selected->name);
}

void BrowserViewModel::cutSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected || selected->mountRoot || _model.browser().inPlaces()) {
        return;
    }

    _pending_copy = PendingCopyFile{true, *selected, true};
    status().set(local("Move · At destination: Tab › Paste", "K přesunu · V cíli Tab › Vložit"));
}

void BrowserViewModel::pasteCopiedToCurrentDirectory()
{
    if (!_pending_copy.active) {
        _model.browser().status().set("Nothing to paste");
        return;
    }

    clearHeldSelection();
    const auto path = std::filesystem::path(currentDirectory().get());
    const auto* volume = _model.browser().volume(_model.browser().currentMountId());
    const auto folder = volume && path == volume->mount.target ? volume->name :
                        path.has_filename() ? path.filename().string() : path.string();
    _package_dialog.set({PackageDialogMode::ConfirmTransfer,
                         _pending_copy.cut ? local("Move here?", "Přesunout sem?") : local("Copy here?", "Zkopírovat sem?"),
                         std::string(local("To folder: ", "Do složky: ")) + folder, false, DialogTone::Normal,
                         _pending_copy.file.name, {}, false, _pending_copy.cut});
}

void BrowserViewModel::startTransfer()
{
    _transfer_control = std::make_shared<internal::TransferControl>();
    _transfer_destination_mount_id = _model.browser().currentMountId();
    const auto* source = _model.browser().volume(_pending_copy.file.mountId);
    const auto* destination = _model.browser().volume(_transfer_destination_mount_id);
    _transfer_source_name = source ? source->name : std::string{};
    _transfer_destination_name = destination ? destination->name : std::string{};
    _transfer_destination_caption = destination ? destination->name :
        std::filesystem::path(currentDirectory().get()).filename().string();
    PackageDialog dialog{PackageDialogMode::Transferring,
                         _pending_copy.cut ? local("Moving", "Přesouvám") : local("Copying", "Kopíruji"),
                         transferProgress(0), false, DialogTone::Normal, _pending_copy.file.name};
    if (!_pending_copy.file.directory && _pending_copy.file.size) dialog.progress = 0;
    _package_dialog.set(std::move(dialog));
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
        _package_dialog.set({PackageDialogMode::Result, local("Delete failed", "Mazání selhalo"),
                             localizedMessage(result.message), false, DialogTone::Error});
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
    _language_menu = false;
    _action_menu_open.set(false);
}

void BrowserViewModel::requestDeleteSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return;
    }

    clearStatus();
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

    clearStatus();
    _pending_rename.set(PendingRenameFile{true, *selected, selected->name});
    _pending_rename_name.set(selected->name);
    closeActionMenu();
}

void BrowserViewModel::requestNewFolder()
{
    closeActionMenu();
    clearStatus();
    _pending_rename.set(PendingRenameFile{true, {}, {}, true});
    _pending_rename_name.set("");
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

    status().set("");
    const auto result = pending.creating ? _model.browser().createDirectory(_pending_rename_name.get()) :
                                          _model.browser().renameSelectedTo(_pending_rename_name.get());
    if (!result) {
        auto message = result.message;
        if (_pending_rename_name.get().empty()) message = pending.creating ? "Enter a folder name." : "Enter a file name.";
        else if (message == "Name already exists") {
            message = local("Name already exists. Enter another.", "Název už existuje. Zadejte jiný.");
        } else if (message == "Invalid name" && _pending_rename_name.get().find('/') != std::string::npos) {
            message = local("A name cannot contain /.", "Název nesmí obsahovat /.");
        }
        status().set(std::move(message));
        return;
    }
    _pending_rename.set(PendingRenameFile{});
    _pending_rename_name.set("");
    status().set(std::string(status().get()));
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
        case BrowserAction::Places:
            closeActionMenu();
            _model.browser().showPlaces();
            break;
        case BrowserAction::Eject:
            closeActionMenu();
            requestEject();
            break;
        case BrowserAction::NewFolder:
            requestNewFolder();
            break;
        case BrowserAction::Language:
            _language_menu = true;
            _action_menu_selected_index.set(static_cast<int>(languagePreference()));
            _action_menu_open.set(true);
            break;
        case BrowserAction::SystemLanguage:
        case BrowserAction::English:
        case BrowserAction::Czech: {
            const auto language = actionAt(_action_menu_selected_index.get()) == BrowserAction::English ? Language::English :
                                  actionAt(_action_menu_selected_index.get()) == BrowserAction::Czech ? Language::Czech :
                                                                                                    Language::System;
            const bool saved = setLanguagePreference(language);
            closeActionMenu();
            entries().set(std::vector<FileEntry>(entries().get()));
            if (!saved) status().set("Could not save language");
            break;
        }

    }
}

void BrowserViewModel::requestEject()
{
    const auto* entry = selectedEntry();
    auto mountId = _model.browser().currentMountId();
    if (!mountId && entry && entry->mountRoot) {
        mountId = entry->mountId;
    }
    const auto* drive = _model.browser().volume(mountId);
    if (!drive) {
        status().set("Drive disconnected");
        return;
    }
    clearHeldSelection();
    _eject_mount_id = mountId;
    _eject_name = drive->name;
    _package_dialog.set({PackageDialogMode::ConfirmEject, local("Eject drive?", "Vysunout disk?"), _eject_name});
}

void BrowserViewModel::startEject()
{
    _eject_progress = std::string(local("Unmounting ", "Odpojuji ")) + _eject_name + "…";
    _package_dialog.set({PackageDialogMode::Ejecting, local("Ejecting drive", "Vysouvám disk"), _eject_progress});
    _storage_operation = std::async(std::launch::async, [this, mountId = _eject_mount_id] {
        return FileTransferResult{FileBrowserModel::ejectDrive(mountId, [this](const std::string& message) {
            std::lock_guard<std::mutex> lock(_eject_mutex);
            _eject_progress = message;
        })};
    });
}

void BrowserViewModel::tickStorage(uint32_t nowMs)
{
    auto dialog = _package_dialog.get();
    const bool transfer = dialog.mode == PackageDialogMode::Transferring;
    if (_storage_operation.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        FileTransferResult outcome;
        try {
            outcome = _storage_operation.get();
        } catch (const std::exception& error) {
            outcome.operation = {FileOperationStatus::Failed, error.what()};
        }
        const auto& result = outcome.operation;
        const bool cancelled = transfer && _transfer_control->cancelled();
        const auto ejectMountId = _eject_mount_id;
        _transfer_control.reset();
        if (transfer && (outcome.disconnectedSource || outcome.disconnectedDestination)) {
            // Reconcile while the transfer still owns the removal message.
            _last_volume_check = nowMs - kVolumePollMs;
            pollVolumes(nowMs);
            _model.browser().refresh(true);

            const bool sourceLost = outcome.disconnectedSource != 0;
            const bool targetLost = outcome.disconnectedDestination != 0;
            const auto& name = targetLost ? _transfer_destination_name : _transfer_source_name;
            std::string disconnect;
            if (sourceLost && targetLost && outcome.disconnectedSource != outcome.disconnectedDestination) {
                disconnect = local("Source and target drives disconnected.", "Zdrojový i cílový disk odpojen.");
            } else if (sourceLost && targetLost) {
                disconnect = name + local(" disconnected.", " odpojen.");
            } else {
                disconnect = std::string(targetLost ? local("Target drive ", "Cílový disk ") :
                                                      local("Source drive ", "Zdrojový disk ")) +
                             name + local(" disconnected.", " byl odpojen.");
            }
            const auto state = outcome.destinationState;
            using Destination = internal::TransferDestination;
            std::string text;
            if (state == Destination::Published) {
                text = local("Destination file was written.", "Cílový soubor byl zapsán.");
            } else if (targetLost || state == Destination::CleanupFailed) {
                text = local("A partial copy may remain.", "Na disku může být neúplná kopie.");
            } else if (state == Destination::PartialRemoved) {
                text = local("Partial copy removed from destination.", "Neúplná kopie v cíli smazána.");
            }
            if (targetLost && !text.empty()) {
                const auto destinationName = std::filesystem::path(outcome.destinationPath).filename().string();
                text += local("\nAfter reconnecting, check the file\n", "\nPo připojení zkontrolujte soubor\n") +
                        (destinationName.empty() ? _pending_copy.file.name : destinationName);
            }
            const bool move = _pending_copy.cut;
            _package_dialog.set({PackageDialogMode::Result,
                                 move ? local("Move interrupted", "Přesun přerušen") :
                                        local("Copy interrupted", "Kopírování přerušeno"),
                                 disconnect + (text.empty() ? "" : "\n" + text), false, DialogTone::Error});
            return;
        }
        if (transfer) {
            if (result) _model.browser().refreshSelecting(outcome.destinationPath);
            else _model.browser().refresh(true);
            if (result || cancelled) {
                _package_dialog.set({});
                std::error_code source_status_error;
                const bool sourceKept = cancelled && _pending_copy.cut && std::filesystem::exists(
                    std::filesystem::symlink_status(_pending_copy.file.path, source_status_error));
                status().set(cancelled ? (_pending_copy.cut ?
                    (sourceKept ? local("Move cancelled · source kept", "Přesun zrušen · zdroj zachován") : "Move cancelled") :
                    "Copy cancelled") : result.message);
            }
        } else {
            // Reconcile the drive before unlocking input, suppressing a duplicate removal notice.
            _last_volume_check = nowMs - kVolumePollMs;
            pollVolumes(nowMs);
            _model.browser().refresh();
            _eject_mount_id = 0;
            if (result) {
                if (!_model.browser().inPlaces()) _model.browser().showPlaces();
                showVolumeNotice({volumeNoticeLine(_eject_name, local("", "Disk "), local(" ejected", " vysunut")) +
                                  local("\nSafe to unplug.", "\nDisk lze bezpečně odpojit."), 0, DialogTone::Success});
            }
        }
        if (result && transfer && _pending_copy.cut) {
            _pending_copy = {};
        }
        if (result || cancelled) {
            _package_dialog.set({});
        } else {
            const bool busy = !transfer && result.message.find(" is in use.") != std::string::npos;
            const bool conflict = result.message == "Name already exists";
            const bool sameFolder = transfer && result.message == "Already here";
            const auto* title = busy ? local("Drive is busy", "Disk je zaneprázdněn") :
                                conflict ? local("Name conflict", "Konflikt názvu") :
                                sameFolder ? local("No move needed", "Přesun nebyl potřeba") :
                                transfer ? (_pending_copy.cut ? local("Move failed", "Přesun selhal") :
                                                                local("Copy failed", "Kopírování selhalo")) :
                                           local("Eject failed", "Vysunutí selhalo");
            auto message = localizedMessage(result.message);
            std::string identity;
            if (busy) {
                identity = _eject_name;
                message = local("Close files on the drive.\nPress Esc, then E to retry.",
                                "Zavřete soubory na disku.\nStiskněte Esc a potom E.");
            } else if (!transfer) {
                identity = _eject_name;
                const bool lost = result.message == "Drive disconnected" ||
                    (result.message.rfind("Could not unmount ", 0) == 0 && !_model.browser().volume(ejectMountId));
                if (lost) {
                    message = local("The drive was disconnected.", "Disk byl odpojen.");
                } else {
                    auto reason = message;
                    if (const auto split = reason.find('\n'); split != std::string::npos) reason.erase(0, split + 1);
                    message = local("Keep the drive plugged in.\nPress Esc, then E to try again.",
                                    "Disk ponechte připojený.\nStiskněte Esc a potom znovu E.");
                    if (!reason.empty()) message += local("\nDetails\n", "\nPodrobnosti\n") + reason;
                }
            } else if (conflict) {
                message = _pending_copy.file.name + local(" already exists here.\nChoose another folder.",
                                                           " už v cíli existuje.\nVyberte jinou složku.");
            } else if (sameFolder) {
                identity = _pending_copy.file.name;
                message = local("The item is already in this folder.", "Položka už je v této složce.");
            } else if (result.message == "Disk is full") {
                message = local("Not enough space in “", "Nedostatek místa v „") + _transfer_destination_caption +
                    local("”.\nFree up space or choose another folder.", "“.\nUvolněte místo nebo zvolte jinou složku.");
            } else if (result.message == "Disk is read-only") {
                identity = message;
                message = local("Destination: ", "Cíl: ") + _transfer_destination_caption + "\n" +
                    local("Choose a writable destination.", "Zvolte cíl s právem zápisu.");
            }
            _package_dialog.set({PackageDialogMode::Result, title, message, false,
                                 busy || conflict || sameFolder ? DialogTone::Warning : DialogTone::Error, identity});
        }
        return;
    }
    if (nowMs - _last_package_update < 125) return;
    _last_package_update = nowMs;
    if (!transfer) {
        std::lock_guard<std::mutex> lock(_eject_mutex);
        dialog.text = localizedMessage(_eject_progress);
        if (dialog.text != _package_dialog.get().text) _package_dialog.set(std::move(dialog));
    } else if (!_transfer_control->cancelled()) {
        const auto bytes = _transfer_control->bytes();
        const auto total = _pending_copy.file.size;
        dialog.text = transferProgress(bytes);
        if (dialog.progress && total) dialog.progress = static_cast<unsigned>(std::min(100.0L, 100.0L * bytes / total));
        if (dialog.text != _package_dialog.get().text || dialog.progress != _package_dialog.get().progress) {
            _package_dialog.set(std::move(dialog));
        }
    }
}

std::string BrowserViewModel::transferProgress(uint64_t bytes) const
{
    const auto& file = _pending_copy.file;
    if (!file.directory) return sizeInUnits(bytes) + local(" of ", " z ") + sizeInUnits(file.size);
    return local("Transferred ", "Přeneseno ") + sizeInUnits(bytes);
}

void BrowserViewModel::markCancelling(PackageDialog& dialog) const
{
    dialog.title = _pending_copy.cut ? local("Cancelling move…", "Ruším přesun…") :
                                     local("Cancelling copy…", "Ruším kopírování…");
    dialog.cancelling = true;
}

void BrowserViewModel::startPackageRead(const FileEntry& entry)
{
    clearHeldSelection();
    _password_attempts = 0;
    _package_source_mount_id = entry.mountId;
    _package_file_name = entry.name;
    const auto* drive = _model.browser().volume(entry.mountId);
    _package_source_name = drive ? drive->name : std::string{};
    _package_dialog.set({PackageDialogMode::Reading, local("Debian package", "Balíček Debian"),
                         local("Checking package…", "Ověřuji balíček…"), false, DialogTone::Normal, entry.name});
    _package_read = std::async(std::launch::async, [path = entry.path, mountId = entry.mountId] {
        return internal::inspectDeb(path, mountId);
    });
}

void BrowserViewModel::startPackageInstall()
{
    if (!_prepared_deb) {
        return;
    }
    _package_log = local("Preparing installation…\n", "Připravuji instalaci…\n");
    std::string password;
    password.swap(_package_password);
    _package_dialog.set({PackageDialogMode::Installing, local("Installing package", "Instaluji balíček"),
                         _package_log, false, DialogTone::Normal, _prepared_deb->metadata.name});
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
            if (!internal::volumePresent(_package_source_mount_id)) {
                inspection = {nullptr, "Drive disconnected"};
            }
            _prepared_deb = std::move(inspection.package);
            PackageDialog review{_prepared_deb ? PackageDialogMode::Review : PackageDialogMode::Result,
                                 local("Debian package", "Balíček Debian"), localizedMessage(inspection.text),
                                 inspection.installable, inspection.installable ? DialogTone::Normal : DialogTone::Error};
            if (_prepared_deb) {
                review.text.clear();
                const auto& metadata = _prepared_deb->metadata;
                review.identity = metadata.name;
                review.metadata = metadata.version + " · " + metadata.architecture;
                review.rootScripts = metadata.hasInstallScripts;
                if (inspection.installable) {
                    review.text = inspection.storeHelperPresent ?
                        local("APT plan verified · Store available", "Plán APT ověřen · Store dostupný") :
                        local("APT plan verified · Store unavailable", "Plán APT ověřen · bez Store");
                } else if (inspection.problem.rfind("Unsupported architecture:", 0) == 0) {
                    review.fixedStatus = localizedMessage(inspection.problem);
                } else {
                    review.fixedStatus = local("Installation blocked.", "Instalace zablokována.");
                    review.text = local("APT could not verify the plan.\nCheck APT and dependencies.",
                                        "Nelze ověřit plán APT.\nZkontrolujte APT a závislosti.");
                    const auto details = inspection.problem.find("\n\n");
                    if (details != std::string::npos) review.text += local("\nDetails\n", "\nPodrobnosti\n") +
                                                                      inspection.problem.substr(details + 2) + "\n";
                }
                if (!review.text.empty()) review.text += "\n";
                review.text += local("Dependencies: ", "Závislosti: ");
                if (!metadata.preDepends.empty()) {
                    review.text += metadata.preDepends;
                    if (!metadata.depends.empty()) review.text += ", ";
                }
                review.text += metadata.depends.empty() && metadata.preDepends.empty() ?
                    local("None", "Žádné") : metadata.depends;
                if (!metadata.description.empty()) review.text += "\n" + metadata.description;
                if (inspection.installable) {
                    review.text += local("\n\nKeep local configuration.", "\n\nMístní konfigurace zůstane zachována.");
                    review.text += std::string(local("\n\nAPT plan (simulation)\n", "\n\nPlán APT (simulace)\n")) + inspection.plan;
                }
            } else {
                review.identity = _package_file_name;
                const auto details = inspection.text.find("\n\n");
                review.fixedStatus = localizedMessage(inspection.text.substr(0, details));
                if (inspection.text == "Drive disconnected" && !_package_source_name.empty()) {
                    review.fixedStatus = local("Source drive disconnected: ", "Zdrojový disk odpojen: ") + _package_source_name;
                    review.text.clear();
                } else {
                    review.text = local("Get a fresh copy of the package.", "Získejte novou kopii balíčku.");
                    if (details != std::string::npos) review.text += local("\nDetails\n", "\nPodrobnosti\n") +
                                                                    inspection.text.substr(details + 2);
                }
            }
            _package_dialog.set(std::move(review));
        }
        if (_package_install.valid() && _package_install.wait_for(noWait) == std::future_status::ready) {
            const auto result = _package_install.get();
            using State = internal::DebInstallState;
            if (result.state == State::PasswordRequired ||
                (result.state == State::AuthenticationFailed && ++_password_attempts < 3)) {
                _package_password.reserve(129);
                const bool retry = result.state == State::AuthenticationFailed;
                _package_dialog.set({PackageDialogMode::Password, local("Password to install", "Heslo pro instalaci"),
                                     retry ? local("Incorrect password. ", "Nesprávné heslo. ") + passwordAttempt() + "." :
                                             local("Enter your account password.", "Zadejte heslo svého účtu."),
                                     false, retry ? DialogTone::Warning : DialogTone::Normal,
                                     _prepared_deb->metadata.name});
            } else {
                const auto identity = _prepared_deb->metadata.name + " · " + _prepared_deb->metadata.version;
                _prepared_deb.reset();
                const bool installed = result.state == State::Installed;
                auto message = installed ? std::string{} :
                    (result.state == State::AuthenticationFailed ? local("Couldn’t verify your account.", "Účet se nepodařilo ověřit.") :
                                                                  localizedMessage(result.message));
                if (result.message == "The Files installer helper is missing.") {
                    message = local("The Files installer helper is missing.\nAsk an administrator to fix this.",
                                    "Chybí instalační nástroj Files.\nPožádejte správce o opravu.");
                } else if (result.message == "The installer helper path has unsafe permissions.") {
                    message = local("The installer helper path has unsafe permissions.\nAsk an administrator to fix this.",
                                    "Cesta k instalačnímu nástroji má nezabezpečená oprávnění.\nPožádejte správce o opravu.");
                } else if (!installed && result.state != State::AuthenticationFailed) {
                    const auto details = message.find('\n');
                    if (result.message.rfind("Package installation failed", 0) == 0) {
                        const auto reason = details == std::string::npos ? std::string::npos :
                            message.find_first_not_of("\r\n ", details);
                        message = reason == std::string::npos ? "" : message.substr(reason);
                        if (message.empty() || message.rfind("dpkg --audit", 0) == 0) {
                            message.insert(0, local("The package manager gave no reason.\n",
                                                    "Správce balíčků neuvedl důvod.\n"));
                        }
                    }
                    const auto audit = message.find("dpkg --audit reported no unfinished packages. Check the error above before retrying.");
                    if (audit != std::string::npos) {
                        message.replace(audit, std::string_view("dpkg --audit reported no unfinished packages. Check the error above before retrying.").size(),
                            local("dpkg: No unfinished installations.\nPress F to return to the error.",
                                  "Kontrola dpkg: žádné nedokončené instalace.\nKlávesou F se vraťte k chybě."));
                    }
                    if (message.rfind("No space left on device.", 0) == 0) {
                        message.replace(0, std::string_view("No space left on device.").size(),
                                        local("There isn’t enough disk space.", "Na disku není dost místa."));
                    }
                }
                _package_dialog.set({PackageDialogMode::Result,
                                     installed ? local("Installation complete", "Instalace dokončena") :
                                                 local("Installation failed", "Instalace selhala"),
                                     message, false, installed ? DialogTone::Success : DialogTone::Error, identity});
            }
        }
    } catch (const std::exception& error) {
        _prepared_deb.reset();
        _package_dialog.set({PackageDialogMode::Result, local("Package operation failed", "Operace s balíčkem selhala"),
                             error.what(), false, DialogTone::Error});
    }

    const auto mode = _package_dialog.get().mode;
    if (mode == PackageDialogMode::Installing &&
        nowMs - _last_package_update >= 125) {
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

std::string BrowserViewModel::passwordAttempt() const
{
    return local("Attempt ", "Pokus ") + std::to_string(_password_attempts + 1) + local(" of 3", " ze 3");
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
    dialog.maskedPassword.assign(_package_password.size(), '*');
    if (_password_attempts) {
        dialog.text = passwordAttempt();
        dialog.tone = DialogTone::Normal;
    }
    _package_dialog.set(std::move(dialog));
}

void BrowserViewModel::handlePackageKey(uint32_t key)
{
    const auto mode = _package_dialog.get().mode;
    if (mode == PackageDialogMode::Transferring) {
        if (key == '\x1b' && _transfer_control->cancel()) {
            auto dialog = _package_dialog.get();
            markCancelling(dialog);
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
        _eject_mount_id = 0;
        _package_dialog.set({});
        return;
    }
    if (key == '\r' || key == '\n') {
        if (mode == PackageDialogMode::ConfirmEject) {
            startEject();
        } else if (mode == PackageDialogMode::ConfirmTransfer) {
            startTransfer();
        } else if ((mode == PackageDialogMode::Review && _package_dialog.get().allowInstall) ||
            (mode == PackageDialogMode::Password && !_package_password.empty())) {
            startPackageInstall();
        }
        return;
    }
    if (mode != PackageDialogMode::Password) {
        if (key == files_key::Up || key == files_key::Down) {
            _package_scroll.set(key == files_key::Up ? -19 : 19);
        } else if (key == files_key::PageUp || key == files_key::PageDown) {
            _package_scroll.set(key == files_key::PageUp ? -76 : 76);
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
    clearHeldSelection();
    _magic.set(++_magic_serial);
    spdlog::info("BrowserViewModel: magic trigger serial={}", _magic.get());
}

}  // namespace files
