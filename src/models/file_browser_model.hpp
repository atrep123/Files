#pragma once

#include "core/files_types.hpp"
#include "models/file_type_registry.hpp"
#include "models/filesystem_transfer.hpp"
#include "models/removable_volumes.hpp"
#include <filesystem>
#include <tools/observable/single_observable.hpp>
#include <string>
#include <vector>

namespace files {

class FileBrowserModel {
public:
    explicit FileBrowserModel(std::string start_directory = {});

    smooth_ui_toolkit::SingleObservable<std::string>& currentDirectory()
    {
        return _current_directory;
    }

    smooth_ui_toolkit::SingleObservable<std::vector<FileEntry>>& entries()
    {
        return _entries;
    }

    smooth_ui_toolkit::SingleObservable<int>& selectedIndex()
    {
        return _selected_index;
    }

    smooth_ui_toolkit::SingleObservable<std::string>& status()
    {
        return _status;
    }

    const FileEntry* selectedEntry() const;
    FileEntry entryWithMetadata(const FileEntry& entry) const;
    bool canGoBack() const;
    bool inLocations() const { return _current_directory.get().empty(); }
    void showLocations(bool rememberDirectory = true, const std::string& preferredPath = {});
    void refreshLocations();
    FileOperationResult ejectSelected();
    static FileOperationResult ejectDrive(uint64_t mountId, const internal::CommandOutput& progress = {});
    uint64_t currentMountId() const { return _current_mount_id; }
    static FileOperationResult performTransfer(const FileEntry& entry, const std::string& destinationDirectory,
                                               uint64_t destinationMountId, bool move,
                                               internal::TransferControl* control = nullptr);
    void refresh(bool preserveSelected = true);
    void selectPrevious();
    void selectNext();
    void selectPageUp(int pageSize);
    void selectPageDown(int pageSize);
    FileOperationResult openSelected(FileEntry* openedFile = nullptr);
    FileOperationResult goBack();
    FileOperationResult goToDirectory(const std::string& path, bool pushHistory = true);
    FileOperationResult copyEntryTo(const FileEntry& entry, const std::string& destinationDirectory);
    FileOperationResult copySelectedTo(const std::string& destinationDirectory);
    FileOperationResult moveEntryTo(const FileEntry& entry, const std::string& destinationDirectory);
    FileOperationResult renameSelectedTo(const std::string& name);
    FileOperationResult deleteSelected();

private:
    struct HistoryEntry {
        std::string directory;
        std::string selectedPath;
        uint64_t mountId;
    };

    FileTypeRegistry _file_types;
    smooth_ui_toolkit::SingleObservable<std::string> _current_directory;
    smooth_ui_toolkit::SingleObservable<std::vector<FileEntry>> _entries{std::vector<FileEntry>{}};
    smooth_ui_toolkit::SingleObservable<int> _selected_index{-1};
    smooth_ui_toolkit::SingleObservable<std::string> _status{""};
    std::vector<HistoryEntry> _history;
    uint64_t _current_mount_id = 0;

    FileEntry makeEntry(const std::filesystem::directory_entry& item, bool includeMetadata) const;
    FileOperationResult readDirectoryEntries(const std::string& directory, std::vector<FileEntry>& entries) const;
    void setEntries(std::vector<FileEntry> entries, const std::string& preferredPath);
    void refreshLocations(const std::string& preferredPath);
    FileOperationResult transferEntry(const FileEntry& entry, const std::string& destinationDirectory, bool move);
    FileOperationResult refreshSelecting(const std::string& preferredPath);
    FileOperationResult goToDirectory(const std::string& path, bool pushHistory, const std::string& preferredPath);
};

}  // namespace files
