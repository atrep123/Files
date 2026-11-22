#include "models/file_browser_model.hpp"

#include "core/files_config.hpp"
#include "models/filesystem_transfer.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <pwd.h>
#include <limits>
#include <system_error>
#include <utility>

namespace files {
namespace fs = std::filesystem;

namespace {

std::string pathString(const fs::path& path)
{
    return path.lexically_normal().string();
}

bool isPreferredEntry(const FileEntry& entry, const std::string& preferredPath)
{
    if (preferredPath.empty()) {
        return false;
    }

    const fs::path entryPath = fs::path(entry.path).lexically_normal();
    const fs::path preferred = fs::path(preferredPath).lexically_normal();
    if (entryPath == preferred) {
        return true;
    }

    // Directory entries can be symlinks.  The path used while descending may
    // be canonicalized through the link, while the entry itself keeps the
    // link's spelling.  Names are unique within one directory, so a basename
    // fallback is sufficient, but only when both paths belong to that same
    // parent.  This avoids selecting an unrelated entry with the same name.
    if (entryPath.parent_path() != preferred.parent_path()) {
        return false;
    }
    const std::string preferredName = preferred.filename().string();
    return !preferredName.empty() && entry.name == preferredName;
}

int64_t modifiedUnixSec(const fs::directory_entry& entry)
{
    std::error_code ec;
    const auto file_time = entry.last_write_time(ec);
    if (ec) {
        return 0;
    }

    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(system_time);
}

FileOperationResult errorResult(FileOperationStatus status, std::string message)
{
    return FileOperationResult{status, std::move(message)};
}

FileOperationResult disconnected()
{
    return errorResult(FileOperationStatus::NotFound, "Drive disconnected");
}

std::string freeSpace(const fs::path& path)
{
    std::error_code error;
    const auto space = fs::space(path, error);
    if (error) {
        return "Free space unavailable";
    }
    double available = static_cast<double>(space.available);
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t unit = 0;
    while (available >= 1024 && unit < 4) {
        available /= 1024;
        ++unit;
    }
    std::ostringstream label;
    label << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << available << ' ' << units[unit] << " free";
    return label.str();
}

bool isPermissionError(const std::error_code& error)
{
    return error == std::errc::permission_denied || error == std::errc::operation_not_permitted;
}

bool pathExistsNoFollow(const fs::path& path, bool& exists, std::error_code& error)
{
    error.clear();
    const fs::file_status status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        exists = false;
        return true;
    }
    if (error) {
        exists = false;
        return false;
    }

    exists = status.type() != fs::file_type::not_found;
    return true;
}

FileOperationResult filesystemError(const char* operation, const std::error_code& error)
{
    if (isPermissionError(error)) {
        return errorResult(FileOperationStatus::PermissionDenied, "Permission denied");
    }
    return errorResult(FileOperationStatus::Failed, std::string(operation) + " failed: " + error.message());
}

FileOperationResult transferError(const char* operation, const internal::FilesystemTransferResult& transfer)
{
    if (transfer.failure == internal::FilesystemTransferFailure::DestinationExists) {
        return errorResult(FileOperationStatus::Failed, "Name already exists");
    }
    if (isPermissionError(transfer.error)) {
        return errorResult(FileOperationStatus::PermissionDenied, "Permission denied");
    }
    return errorResult(FileOperationStatus::Failed, std::string(operation) + " failed: " + transfer.detail);
}

}  // namespace

FileBrowserModel::FileBrowserModel(std::string start_directory)
    : _current_directory(normalizeDirectoryPath(start_directory.empty() ? defaultStartDirectory() : start_directory))
{
}

const FileEntry* FileBrowserModel::selectedEntry() const
{
    const auto& list = _entries.get();
    const int index  = _selected_index.get();
    if (index < 0 || static_cast<size_t>(index) >= list.size()) {
        return nullptr;
    }
    return &list[static_cast<size_t>(index)];
}

FileEntry FileBrowserModel::entryWithMetadata(const FileEntry& entry) const
{
    std::error_code ec;
    const fs::directory_entry item(entry.path, ec);
    return ec ? entry : makeEntry(item, true);
}

bool FileBrowserModel::canGoBack() const
{
    if (inLocations() || !_history.empty()) {
        return true;
    }

    const fs::path current = fs::path(_current_directory.get());
    const fs::path parent  = current.parent_path();
    return !parent.empty() && parent != current;
}

void FileBrowserModel::refresh(bool preserveSelected)
{
    if (inLocations()) {
        refreshLocations();
        return;
    }
    const FileEntry* selected       = selectedEntry();
    const std::string preferredPath = preserveSelected && selected ? selected->path : "";
    (void)refreshSelecting(preferredPath);
}

void FileBrowserModel::showLocations(bool rememberDirectory, const std::string& preferredPath)
{
    if (!inLocations() && rememberDirectory) {
        const auto* selected = selectedEntry();
        _history.push_back({_current_directory.get(), selected ? selected->path : "", _current_mount_id});
    }
    _current_mount_id = 0;
    if (!inLocations()) {
        _current_directory.set("");
        _status.set("Ready");
        refreshLocations(preferredPath);
    }
}

void FileBrowserModel::refreshLocations()
{
    const auto* selected = selectedEntry();
    refreshLocations(selected ? selected->path : "");
}

void FileBrowserModel::refreshLocations(const std::string& preferredPath)
{
    std::vector<FileEntry> list;
    const auto addLocation = [&list](const fs::path& path, std::string name) -> FileEntry& {
        FileEntry entry;
        entry.path = path.string();
        entry.name = std::move(name);
        entry.kind = FileKind::Directory;
        entry.directory = true;
        entry.icon = "folder";
        list.push_back(std::move(entry));
        return list.back();
    };
    if (const auto* user = getpwuid(getuid()); user && user->pw_dir && std::string(user->pw_dir) != "/") {
        addLocation(user->pw_dir, "Home");
    }
    addLocation("/", "File system");
    for (const auto& volume : internal::removableVolumes()) {
        auto& entry = addLocation(volume.mount.target, volume.name);
        entry.mountId = volume.mount.id;
        entry.mountRoot = true;
        entry.freeSpace = freeSpace(volume.mount.target);
    }
    const auto& previous = _entries.get();
    if (previous.size() != list.size() ||
        !std::equal(list.begin(), list.end(), previous.begin(), [](const auto& a, const auto& b) {
            return a.path == b.path && a.name == b.name && a.mountId == b.mountId && a.freeSpace == b.freeSpace;
        })) {
        setEntries(std::move(list), preferredPath);
    }
}

FileOperationResult FileBrowserModel::ejectSelected()
{
    const auto* entry = selectedEntry();
    if (!entry || !entry->mountRoot) {
        return errorResult(FileOperationStatus::InvalidSelection, "Select a removable drive");
    }
    for (const auto& volume : internal::removableVolumes()) {
        if (volume.mount.id == entry->mountId) {
            auto result = internal::ejectVolume(volume);
            refresh(true);
            return result;
        }
    }
    refresh(true);
    return disconnected();
}

FileOperationResult FileBrowserModel::readDirectoryEntries(const std::string& directory,
                                                           std::vector<FileEntry>& entries) const
{
    entries.clear();
    std::error_code ec;

    const auto volumes = internal::removableVolumes();
    const fs::directory_iterator end;
    fs::directory_iterator item(directory, ec);
    while (!ec && item != end) {
        auto entry = makeEntry(*item, false);
        for (const auto& volume : volumes) {
            if (internal::volumeContains(volume, entry.path)) {
                entry.mountId = volume.mount.id;
                entry.mountRoot = entry.path == volume.mount.target;
            }
        }
        entries.push_back(std::move(entry));
        item.increment(ec);
    }
    if (ec) {
        spdlog::warn("FileBrowserModel: failed to read {}: {}", directory, ec.message());
        return filesystemError("Read", ec);
    }

    std::sort(entries.begin(), entries.end(), [](const FileEntry& lhs, const FileEntry& rhs) {
        if (lhs.directory != rhs.directory) {
            return lhs.directory && !rhs.directory;
        }
        return lhs.name < rhs.name;
    });
    return FileOperationResult{};
}

FileOperationResult FileBrowserModel::refreshSelecting(const std::string& preferredPath)
{
    if (_current_mount_id == 0) {
        for (const auto& volume : internal::removableVolumes()) {
            if (internal::volumeContains(volume, _current_directory.get())) {
                _current_mount_id = volume.mount.id;
                break;
            }
        }
    }
    if (!internal::volumePresent(_current_mount_id)) {
        showLocations(false);
        _status.set("Drive disconnected");
        return disconnected();
    }
    std::vector<FileEntry> list;
    const FileOperationResult result = readDirectoryEntries(_current_directory.get(), list);
    if (!result) {
        _status.set(result.message);
        // Keep the last known listing and selection available for retry. A
        // transient permission or filesystem error should not blank the UI.
        return result;
    }

    _status.set(list.empty() ? "Empty folder" : "Ready");
    setEntries(std::move(list), preferredPath);
    return result;
}

void FileBrowserModel::selectPrevious()
{
    const auto& list = _entries.get();
    if (list.empty()) {
        _selected_index.set(-1);
        return;
    }

    const size_t last    = std::min(list.size() - 1, static_cast<size_t>(std::numeric_limits<int>::max()));
    const size_t current = _selected_index.get() < 0 ? 0 : std::min(static_cast<size_t>(_selected_index.get()), last);
    const size_t next    = current == 0 ? last : current - 1;
    _selected_index.set(static_cast<int>(next));
}

void FileBrowserModel::selectNext()
{
    const auto& list = _entries.get();
    if (list.empty()) {
        _selected_index.set(-1);
        return;
    }

    const size_t last    = std::min(list.size() - 1, static_cast<size_t>(std::numeric_limits<int>::max()));
    const size_t current = _selected_index.get() < 0 ? 0 : std::min(static_cast<size_t>(_selected_index.get()), last);
    const size_t next    = current >= last ? 0 : current + 1;
    _selected_index.set(static_cast<int>(next));
}

void FileBrowserModel::selectPageUp(int pageSize)
{
    const auto& list = _entries.get();
    if (list.empty()) {
        _selected_index.set(-1);
        return;
    }

    const size_t last    = std::min(list.size() - 1, static_cast<size_t>(std::numeric_limits<int>::max()));
    const size_t current = _selected_index.get() < 0 ? 0 : std::min(static_cast<size_t>(_selected_index.get()), last);
    const size_t step    = pageSize > 0 ? static_cast<size_t>(pageSize) : 1U;
    _selected_index.set(static_cast<int>(step >= current ? 0 : current - step));
}

void FileBrowserModel::selectPageDown(int pageSize)
{
    const auto& list = _entries.get();
    if (list.empty()) {
        _selected_index.set(-1);
        return;
    }

    const size_t last      = std::min(list.size() - 1, static_cast<size_t>(std::numeric_limits<int>::max()));
    const size_t current   = _selected_index.get() < 0 ? 0 : std::min(static_cast<size_t>(_selected_index.get()), last);
    const size_t step      = pageSize > 0 ? static_cast<size_t>(pageSize) : 1U;
    const size_t remaining = last - current;
    _selected_index.set(static_cast<int>(step >= remaining ? last : current + step));
}

FileOperationResult FileBrowserModel::openSelected(FileEntry* openedFile)
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return errorResult(FileOperationStatus::InvalidSelection, "No file selected");
    }

    if (!internal::volumePresent(selected->mountId)) {
        refresh(true);
        return disconnected();
    }
    if (selected->directory) {
        // Keep the spelling used by the directory entry in history.  A
        // symlinked directory is canonicalized for browsing, but returning
        // should still highlight that exact entry in the parent directory.
        return goToDirectory(selected->path, true, selected->path);
    }

    if (openedFile) {
        *openedFile = entryWithMetadata(*selected);
        openedFile->mountId = selected->mountId;
    }
    return FileOperationResult{};
}

FileOperationResult FileBrowserModel::goBack()
{
    while (!_history.empty() && !internal::volumePresent(_history.back().mountId)) {
        _history.pop_back();
    }
    if (!_history.empty()) {
        const HistoryEntry previous = _history.back();
        if (previous.directory.empty()) {
            showLocations(false, previous.selectedPath);
            _history.pop_back();
            return {};
        }
        const FileOperationResult result = goToDirectory(previous.directory, false, previous.selectedPath);
        if (result) {
            _history.pop_back();
        }
        return result;
    }
    if (inLocations()) {
        return goToDirectory(defaultStartDirectory(), false);
    }

    fs::path current(_current_directory.get());
    const fs::path parent = current.parent_path();
    if (parent.empty() || parent == current) {
        return errorResult(FileOperationStatus::NotSupported, "Already at root");
    }
    return goToDirectory(pathString(parent), false, pathString(current));
}

FileOperationResult FileBrowserModel::goToDirectory(const std::string& path, bool pushHistory)
{
    return goToDirectory(path, pushHistory, "");
}

FileOperationResult FileBrowserModel::goToDirectory(const std::string& path, bool pushHistory,
                                                    const std::string& preferredPath)
{
    fs::path requestedPath(path);
    if (requestedPath.is_relative()) {
        requestedPath = fs::path(_current_directory.get()) / requestedPath;
    }

    std::error_code ec;
    const fs::path target        = fs::weakly_canonical(requestedPath, ec);
    const std::string targetPath = pathString(ec ? requestedPath : target);
    ec.clear();
    if (!fs::is_directory(targetPath, ec)) {
        if (ec) {
            return filesystemError("Open", ec);
        }
        return errorResult(FileOperationStatus::NotFound, "Folder not found");
    }

    std::vector<FileEntry> targetEntries;
    const FileOperationResult readResult = readDirectoryEntries(targetPath, targetEntries);
    if (!readResult) {
        _status.set(readResult.message);
        return readResult;
    }

    const std::string previous = _current_directory.get();
    if (pushHistory && previous != targetPath) {
        _history.push_back({previous, pathString(requestedPath), _current_mount_id});
    }
    _current_mount_id = 0;
    for (const auto& volume : internal::removableVolumes()) {
        if (internal::volumeContains(volume, targetPath)) {
            _current_mount_id = volume.mount.id;
            break;
        }
    }

    _current_directory.set(targetPath);
    _status.set(targetEntries.empty() ? "Empty folder" : "Ready");
    setEntries(std::move(targetEntries), preferredPath);
    return FileOperationResult{};
}

FileOperationResult FileBrowserModel::copyEntryTo(const FileEntry& entry, const std::string& destinationDirectory)
{
    return transferEntry(entry, destinationDirectory, false);
}

FileOperationResult FileBrowserModel::copySelectedTo(const std::string& destinationDirectory)
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return errorResult(FileOperationStatus::InvalidSelection, "No file selected");
    }

    return copyEntryTo(*selected, destinationDirectory);
}

FileOperationResult FileBrowserModel::moveEntryTo(const FileEntry& entry, const std::string& destinationDirectory)
{
    return transferEntry(entry, destinationDirectory, true);
}

FileOperationResult FileBrowserModel::transferEntry(const FileEntry& entry, const std::string& destinationDirectory,
                                                    bool move)
{
    if (entry.mountRoot || destinationDirectory.empty()) {
        return errorResult(FileOperationStatus::InvalidSelection, "Open a folder first");
    }
    const fs::path source(entry.path);
    const std::string sourceName = entry.name;
    uint64_t destinationMountId = destinationDirectory == _current_directory.get() ? _current_mount_id : 0;
    if (!destinationMountId) {
        for (const auto& volume : internal::removableVolumes()) {
            if (internal::volumeContains(volume, destinationDirectory)) {
                destinationMountId = volume.mount.id;
                break;
            }
        }
    }
    const auto sourceParent = source.parent_path().empty() ? fs::path(".") : source.parent_path();
    const internal::TransferDirectory sourceDirectory(sourceParent, entry.mountId);
    const internal::TransferDirectory destination(destinationDirectory, destinationMountId);
    const auto connected = [&] { return sourceDirectory.connected() && destination.connected(); };
    if (!connected()) {
        return disconnected();
    }
    const auto pinnedSource = sourceDirectory.path() / source.filename();
    const auto transfer = move ? internal::movePathToDirectory(pinnedSource, destination.path(), connected)
                               : internal::copyPathToDirectory(pinnedSource, destination.path(), connected);
    if (!connected() || transfer.failure == internal::FilesystemTransferFailure::DriveDisconnected) {
        return disconnected();
    }
    if (transfer.failure == internal::FilesystemTransferFailure::SamePath) {
        return errorResult(FileOperationStatus::InvalidSelection, "Already here");
    }
    if (!transfer) {
        return transferError(move ? "Cut" : "Copy", transfer);
    }
    refresh(false);
    _status.set(std::string(move ? "Moved " : "Copied ") + sourceName);
    return {};
}

FileOperationResult FileBrowserModel::renameSelectedTo(const std::string& name)
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return errorResult(FileOperationStatus::InvalidSelection, "No file selected");
    }
    if (inLocations() || selected->mountRoot) {
        return errorResult(FileOperationStatus::NotSupported, "Cannot rename a location");
    }
    if (!internal::volumePresent(selected->mountId)) {
        return disconnected();
    }
    const std::string oldName = selected->name;
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) {
        return errorResult(FileOperationStatus::InvalidSelection, "Invalid name");
    }

    std::error_code ec;
    const fs::path source(selected->path);
    const fs::path destination = source.parent_path() / name;
    if (source == destination) {
        return FileOperationResult{};
    }
    bool destinationExists = false;
    if (!pathExistsNoFollow(destination, destinationExists, ec)) {
        return filesystemError("Rename", ec);
    }
    if (destinationExists) {
        return errorResult(FileOperationStatus::Failed, "Name already exists");
    }

    const internal::TransferDirectory directory(source.parent_path(), selected->mountId);
    if (!directory.connected()) {
        return disconnected();
    }
    fs::rename(directory.path() / source.filename(), directory.path() / destination.filename(), ec);
    if (ec) {
        return filesystemError("Rename", ec);
    }

    refreshSelecting(pathString(destination));
    _status.set("Renamed " + oldName);
    return FileOperationResult{};
}

FileOperationResult FileBrowserModel::deleteSelected()
{
    const FileEntry* selected = selectedEntry();
    if (!selected) {
        return errorResult(FileOperationStatus::InvalidSelection, "No file selected");
    }

    if (inLocations() || selected->mountRoot) {
        return errorResult(FileOperationStatus::NotSupported, "Cannot delete a location");
    }
    if (!internal::volumePresent(selected->mountId)) {
        return disconnected();
    }
    const fs::path deletedPath = selected->path;
    const std::string deletedName = selected->name;
    if (selected->directory) {
        for (const auto& volume : internal::removableVolumes()) {
            const auto relative = volume.mount.target.lexically_relative(deletedPath);
            if (!relative.empty() && *relative.begin() != "..") {
                return errorResult(FileOperationStatus::NotSupported, "Folder contains a mounted drive");
            }
        }
    }
    const internal::TransferDirectory directory(deletedPath.parent_path(), selected->mountId);
    if (!directory.connected()) {
        return disconnected();
    }
    std::error_code ec;
    fs::remove_all(directory.path() / deletedPath.filename(), ec);
    if (ec) {
        return filesystemError("Delete", ec);
    }

    refresh(false);
    _status.set("Deleted " + deletedName);
    return FileOperationResult{};
}

FileEntry FileBrowserModel::makeEntry(const fs::directory_entry& item, bool includeMetadata) const
{
    std::error_code ec;
    const fs::path fsPath       = item.path();
    const bool directory        = item.is_directory(ec);
    const std::string extension = directory ? "" : normalizedExtension(fsPath.extension().string());

    uint64_t size = 0;
    if (includeMetadata && !directory) {
        ec.clear();
        size = static_cast<uint64_t>(item.file_size(ec));
        if (ec) {
            size = 0;
        }
    }

    FileEntry entry;
    entry.path            = pathString(fsPath);
    entry.name            = fsPath.filename().string();
    entry.extension       = extension;
    entry.directory       = directory;
    entry.kind            = directory ? FileKind::Directory : _file_types.kindForExtension(extension);
    entry.icon            = _file_types.iconFor(entry);
    entry.size            = size;
    entry.modifiedUnixSec = includeMetadata ? modifiedUnixSec(item) : 0;
    entry.hidden          = !entry.name.empty() && entry.name.front() == '.';
    entry.readable        = true;
    entry.writable        = true;
    return entry;
}

void FileBrowserModel::setEntries(std::vector<FileEntry> entries, const std::string& preferredPath)
{
    int selected = entries.empty() ? -1 : 0;
    if (!preferredPath.empty()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (isPreferredEntry(entries[i], preferredPath)) {
                selected = static_cast<int>(i);
                break;
            }
        }
    }

    _selected_index.set(selected);
    _entries.set(std::move(entries));
}

}  // namespace files
