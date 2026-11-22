#include "models/removable_volumes.hpp"

#include "models/command.hpp"
#include <algorithm>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <unistd.h>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace files::internal {
namespace fs = std::filesystem;
namespace {

std::string unescapeMountField(const std::string& text)
{
    std::string value;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() &&
            text.find_first_not_of("01234567", i + 1) >= i + 4) {
            value += static_cast<char>((text[i+1] - '0') * 64 + (text[i+2] - '0') * 8 + text[i+3] - '0');
            i += 3;
        } else {
            value += text[i];
        }
    }
    return value;
}

std::vector<MountRecord> currentMounts()
{
    std::ifstream stream("/proc/self/mountinfo");
    return readMountInfo(stream);
}

fs::path blockPath(const fs::path& sysfs, const MountRecord& mount)
{
    if (mount.source.string().rfind("/dev/", 0) != 0) {
        return {};
    }
    std::error_code ec;
    auto path = fs::canonical(sysfs / "dev/block" / mount.deviceNumber, ec);
#if defined(__linux__)
    // FUSE filesystems have an anonymous mount device number.
    if (ec && mount.deviceNumber.rfind("0:", 0) == 0) {
        struct stat status{};
        if (stat(mount.source.c_str(), &status) == 0 && S_ISBLK(status.st_mode)) {
            const auto number = std::to_string(major(status.st_rdev)) + ":" + std::to_string(minor(status.st_rdev));
            path = fs::canonical(sysfs / "dev/block" / number, ec);
        }
    }
#endif
    return ec ? fs::path{} : path;
}

fs::path diskPath(const fs::path& block)
{
    std::error_code ec;
    return fs::exists(block / "partition", ec) ? block.parent_path() : block;
}

bool usbDisk(fs::path path)
{
    for (; path.has_parent_path() && path != path.parent_path(); path = path.parent_path()) {
        std::error_code ec;
        if (fs::read_symlink(path / "subsystem", ec).filename() == "usb") {
            return true;
        }
    }
    return false;
}

bool removableDisk(const fs::path& path)
{
    std::ifstream stream(path / "removable");
    char flag = '0';
    stream >> flag;
    return flag == '1';
}

CommandResult runUdisks(const char* action, const fs::path& device)
{
    std::vector<std::string> arguments{"/usr/bin/udisksctl", action, "--no-user-interaction", "-b", device.string()};
    auto result = runCommand(arguments);
    if (result || (result.output.find("NotAuthorized") == std::string::npos &&
                   result.output.find("MountedByOtherUser") == std::string::npos)) {
        return result;
    }
    // APPLaunch may run outside an active polkit session. Respect an existing sudo policy.
    arguments.insert(arguments.begin(), {"/usr/bin/sudo", "-n", "--"});
    auto elevated = runCommand(arguments);
    if (!elevated) {
        elevated.output = result.output + elevated.output;
    }
    return elevated;
}

FileOperationResult ejectError(std::string message, const CommandResult& result)
{
    if (!result.output.empty()) {
        message += "\n" + result.output;
    }
    return {FileOperationStatus::Failed, std::move(message)};
}

}  // namespace

std::vector<MountRecord> readMountInfo(std::istream& input)
{
    std::vector<MountRecord> mounts;
    for (std::string line; std::getline(input, line);) {
        std::istringstream fields(line);
        uint64_t parent;
        MountRecord mount{};
        std::string root, target, token, source;
        if (!(fields >> mount.id >> parent >> mount.deviceNumber >> root >> target)) {
            continue;
        }
        if (mount.deviceNumber.find(':') == std::string::npos ||
            mount.deviceNumber.find_first_not_of("0123456789:") != std::string::npos) {
            continue;
        }
        while (fields >> token && token != "-") {}
        if (token != "-" || !(fields >> token >> source)) {
            continue;
        }
        mount.root = unescapeMountField(root);
        mount.target = unescapeMountField(target);
        mount.source = unescapeMountField(source);
        mounts.push_back(std::move(mount));
    }
    return mounts;
}

std::vector<RemovableVolume> removableVolumes(const std::vector<MountRecord>& mounts,
                                             const fs::path& mediaDirectory, const fs::path& sysfs)
{
    std::vector<RemovableVolume> volumes;
    for (const auto& mount : mounts) {
        if (mount.target.parent_path() != mediaDirectory && mount.target.parent_path() != "/media" &&
            mount.target.parent_path() != fs::path("/run") / mediaDirectory.relative_path()) {
            continue;
        }
        auto block = blockPath(sysfs, mount);
        if (block.empty()) {
            continue;
        }
        const auto disk = diskPath(block);
        const bool usb = usbDisk(disk);
        const bool loop = disk.filename().string().rfind("loop", 0) == 0;
        if (!usb && !loop && !removableDisk(disk)) {
            continue;
        }
        auto device = mount;
        device.source = fs::path("/dev") / block.filename();
        volumes.push_back({std::move(device), fs::path("/dev") / disk.filename(),
                           mount.target.filename().string(), usb});
    }
    std::sort(volumes.begin(), volumes.end(), [](const auto& a, const auto& b) {
        return a.name == b.name ? a.mount.target < b.mount.target : a.name < b.name;
    });
    return volumes;
}

std::vector<RemovableVolume> removableVolumes()
{
#if defined(__linux__)
    if (const auto* user = getpwuid(getuid())) {
        return removableVolumes(currentMounts(), fs::path("/media") / user->pw_name, "/sys");
    }
#endif
    return {};
}

bool volumeContains(const RemovableVolume& volume, const fs::path& path)
{
    const fs::path relative = path.lexically_relative(volume.mount.target);
    return !relative.empty() && *relative.begin() != "..";
}

bool volumePresent(uint64_t id)
{
    if (!id) {
        return true;
    }
    const auto mounts = currentMounts();
    return std::any_of(mounts.begin(), mounts.end(), [id](const auto& mount) {
        return mount.id == id && !blockPath("/sys", mount).empty();
    });
}

TransferDirectory::TransferDirectory(const fs::path& directory, uint64_t mountId)
    : _mountId(mountId), _path(directory)
{
#if defined(__linux__)
    if (_mountId) {
        const int fd = open(directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        struct statx status{};
        if (fd >= 0 && statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &status) == 0 &&
            (status.stx_mask & STATX_MNT_ID) && status.stx_mnt_id == _mountId) {
            _fd = fd;
            _path = fs::path("/proc/self/fd") / std::to_string(fd) / ".";
        } else if (fd >= 0) {
            close(fd);
        }
    }
#endif
}

TransferDirectory::~TransferDirectory()
{
    if (_fd >= 0) {
        close(_fd);
    }
}

bool TransferDirectory::connected() const
{
    return !_mountId || (_fd >= 0 && volumePresent(_mountId));
}

FileOperationResult ejectVolume(const RemovableVolume& volume)
{
    if (!volumePresent(volume.mount.id)) {
        return {FileOperationStatus::NotFound, "Drive disconnected"};
    }
    const auto mounts = currentMounts();
    // udisksctl selects by device. Avoid unmounting a different alias of the same device.
    if (std::count_if(mounts.begin(), mounts.end(), [&volume](const auto& mount) {
            return mount.deviceNumber == volume.mount.deviceNumber;
        }) != 1) {
        return {FileOperationStatus::Failed, "Drive has multiple mount points"};
    }
    const auto unmount = runUdisks("unmount", volume.mount.source);
    if (!unmount || volumePresent(volume.mount.id)) {
        return ejectError("Could not unmount " + volume.name, unmount);
    }
    if (!volume.powerOff) {
        return {FileOperationStatus::Ok, "Unmounted " + volume.name};
    }

    for (const auto& mount : currentMounts()) {
        const auto block = blockPath("/sys", mount);
        if (!block.empty() && (fs::path("/dev") / diskPath(block).filename()) == volume.disk) {
            return {FileOperationStatus::Ok, "Unmounted. Other volumes are still mounted."};
        }
    }
    const auto power = runUdisks("power-off", volume.disk);
    if (!power) {
        return ejectError("Unmounted. Could not power off " + volume.name, power);
    }
    return {FileOperationStatus::Ok, "Safe to remove " + volume.name};
}

}  // namespace files::internal
