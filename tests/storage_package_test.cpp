#include "models/debian_package.hpp"
#include "models/removable_volumes.hpp"
#include "models/file_browser_model.hpp"
#include "models/filesystem_transfer.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace files;
using namespace files::internal;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TempDirectory {
    fs::path path;

    TempDirectory()
    {
        std::string directory = "/tmp/files-storage-test-XXXXXX";
        check(mkdtemp(directory.data()) != nullptr, "cannot create fixture directory");
        path = directory;
    }

    ~TempDirectory()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void testMountList(const fs::path& temp)
{
    std::istringstream raw(R"(garbage
61 40 8:1 / /media/pi/Winter\040USB rw,nosuid shared:22 - vfat /dev/sda1 rw
62 40 8:16 / /media/pi/Fixed rw - ext4 /dev/sdb rw
63 40 8:32 / /mnt/LaptopSSD rw - ext4 /dev/sdc rw
64 40 7:0 / /media/pi/Test rw - ext4 /dev/loop0 rw
65 40 179:9 / /run/media/pi/SD\134Cards rw - ext4 /dev/mmcblk0p1 rw
66 40 0:40 / /media/pi/Remote rw - nfs server:/work rw
67 40 8:32 / /media/other/Private rw - ext4 /dev/sdc rw
68 40 8:1 / /media/pi/Incomplete
69 40 8:32 / /media/Elsewhere rw - ext4 /dev/sdc rw
)");
    const auto mounts = readMountInfo(raw);
    check(mounts.size() == 8, "malformed mount rows must be ignored");
    check(mounts.front().target == "/media/pi/Winter USB", "mount path with spaces was not decoded");
    check(mounts[4].target == "/run/media/pi/SD\\Cards", "literal backslash was not decoded");

    const auto root = temp / "sys";
    fs::create_directories(root / "dev/block");
    const auto block = [&](const char* number, const char* device) {
        const auto path = root / device;
        fs::create_directories(path);
        fs::create_symlink(path, root / "dev/block" / number);
        return path;
    };
    const auto usbPartition = block("8:1", "devices/usb1/1-1/host0/block/sda/sda1");
    std::ofstream(usbPartition / "partition") << "1\n";
    std::ofstream(usbPartition.parent_path() / "removable") << "0\n";
    fs::create_symlink("../../bus/usb", root / "devices/usb1/subsystem");
    std::ofstream(block("8:16", "devices/block/sdb") / "removable") << "0\n";
    std::ofstream(block("8:32", "devices/usb1/1-2/host1/block/sdc") / "removable") << "0\n";
    block("7:0", "devices/virtual/block/loop0");
    const auto sdPartition = block("179:9", "devices/block/mmcblk0/mmcblk0p1");
    std::ofstream(sdPartition / "partition") << "1\n";
    std::ofstream(sdPartition.parent_path() / "removable") << "1\n";

    const auto volumes = removableVolumes(mounts, "/media/pi", root);
    check(volumes.size() == 4, "USB, SD and loop mounts should appear, but not fixed or unrelated mounts");
    const auto& usb = volumes.back();
    check(usb.mount.id == 61 && usb.mount.source == "/dev/sda1" && usb.disk == "/dev/sda" && usb.powerOff,
          "USB partition must map to the correct parent, including disks with RM=0");
    check(volumeContains(usb, "/media/pi/Winter USB/folder/item"), "child was not recognized");
    check(!volumeContains(usb, "/media/pi/Winter USB2/file"), "similar prefix matched another drive");
    check(!volumes[1].powerOff && !volumes[2].powerOff, "SD and loop devices should only be unmounted");
}

fs::path buildDeb(const fs::path& base, const std::string& arch, const std::string& depends = {})
{
    fs::create_directories(base / "DEBIAN");
    const auto docs = base / "usr/share/doc/cz-zkouska-files";
    fs::create_directories(docs);
    std::ofstream(docs / "zkouska.txt") << "Files test\n";
    std::ofstream control(base / "DEBIAN/control");
    control << "Package: cz-zkouska-files\nVersion: 1.0.0\nArchitecture: " << arch
            << "\nMaintainer: Files Test <files-test@example.invalid>\n";
    if (!depends.empty()) {
        control << "Depends: " << depends << '\n';
    }
    control << "Description: Files test package\n extra text\n .\n after a blank line\n";
    control.close();
    const auto deb = base.string() + " ; test.deb";
    const auto build = runCommand({"/usr/bin/dpkg-deb", "--build", "--root-owner-group", base.string(), deb});
    check(static_cast<bool>(build), build.output.c_str());
    return deb;
}

void testDeb(const fs::path& temp)
{
    const auto parsed = parseDebInfo(" new Debian package, version 2.0.\n"
                                     " Package: cz-zkouska-files\n Version: 1:0~1\n Architecture: all\n"
                                     " Depends: foo (>= 1),\n  bar | baz\n Pre-Depends: quux\n"
                                     " Description: Summary: with colon\n  details\n  .\n  last");
    check(parsed.version == "1:0~1" && parsed.name == "cz-zkouska-files" && parsed.architecture == "all",
          "package identity fields must be parsed");
    check(parsed.depends == "foo (>= 1), bar | baz" && parsed.preDepends == "quux",
          "dependency fields and continuation lines must be parsed");
    check(parsed.description == "Summary: with colon\ndetails\n\nlast", "description lost formatting");

    std::error_code error;
    if (!fs::exists("/usr/bin/dpkg-deb", error) || !fs::exists("/usr/bin/apt-get", error)) {
        std::cout << "dpkg-deb or apt-get is unavailable; only the metadata parser was exercised\n";
        return;
    }
    const auto path = buildDeb(temp / "valid", "all");
    fs::path staged;
    {
        auto review = inspectDeb(path);
        check(review.installable && review.package != nullptr, review.text.c_str());
        check(review.text.find("Depends: None") != std::string::npos &&
                  review.text.find("extra text\n\nafter a blank line") != std::string::npos,
              "package review is missing metadata");
        staged = review.package->path;
        check(fs::is_regular_file(staged) && fs::file_size(path) == fs::file_size(staged), "staging failed");
        std::ofstream(path, std::ios::trunc) << "replaced after review";
        check(fs::file_size(staged) > fs::file_size(path), "review did not pin the staged bytes");
    }
    check(!fs::exists(staged.parent_path()), "cancelling review must remove the staged package");

    const auto invalid = inspectDeb(path);
    check(!invalid.installable && invalid.text.find("Invalid or unreadable") != std::string::npos,
          "corrupt package was not rejected");

    const auto wrong = buildDeb(temp / "wrong", "files-no-such-architecture");
    const auto unsupported = inspectDeb(wrong);
    check(!unsupported.installable && unsupported.package &&
              unsupported.text.find("Unsupported architecture") != std::string::npos,
          "wrong architecture should remain visible with install disabled");

    const auto valid = buildDeb(temp / "truncated", "all");
    fs::resize_file(valid, fs::file_size(valid) - 64);
    const auto damaged = inspectDeb(valid);
    check(!damaged.installable && damaged.text.find("Damaged Debian package") != std::string::npos,
          "valid control archive with truncated data should be rejected");

    const auto unsatisfied = buildDeb(temp / "dependencies", "all", "cz-files-nonexistent-dependency (= 99999999)");
    const auto missing = inspectDeb(unsatisfied);
    check(missing.installable && missing.package, missing.text.c_str());
    std::string emptyPassword;
    bool attemptedAuth = false;
    const auto rejected = installDeb(*missing.package, emptyPassword, [&](const auto& message) {
        attemptedAuth |= message.find("sudo authorization") != std::string::npos;
    });
    check(rejected.state == DebInstallState::Failed && !attemptedAuth &&
              rejected.message.find("cz-files-nonexistent-dependency") != std::string::npos,
          rejected.message.c_str());
}

void selectByName(FileBrowserModel& browser, const std::string& name)
{
    for (size_t i = 0; i < browser.entries().get().size(); ++i) {
        const auto* entry = browser.selectedEntry();
        if (entry && entry->name == name) {
            return;
        }
        browser.selectNext();
    }
    throw std::runtime_error("Missing browser entry: " + name);
}

void testDisconnectDuringCopy(const fs::path& mount, const fs::path& temp)
{
    const auto volumes = removableVolumes();
    const auto volume = std::find_if(volumes.begin(), volumes.end(), [&](const auto& v) {
        return v.mount.target == mount;
    });
    check(volume != volumes.end(), "temporary mount not discovered");
    const TransferDirectory destination(mount, volume->mount.id);
    check(destination.connected(), "failed to pin the test mount");
    const auto source = temp / "keep-source.txt";
    std::ofstream(source) << "detect loss before publishing\n";
    bool copiedIntoStage = false;
    const auto copied = copyPathToDirectory(source, destination.path(), [&] {
        for (const auto& item : fs::directory_iterator(mount)) {
            if (item.path().filename().string().rfind(".files-transfer-", 0) == 0) {
                copiedIntoStage = fs::is_regular_file(item.path() / "payload");
                break;
            }
        }
        const auto detached = runCommand({"/usr/bin/umount", "--lazy", mount.string()});
        return detached && destination.connected();
    });
    check(copiedIntoStage && copied.failure == FilesystemTransferFailure::DriveDisconnected &&
              fs::is_regular_file(source) && !fs::exists(mount / source.filename()) &&
              !fs::exists(destination.path() / source.filename()), "copy was published after disconnection");
    for (const auto& item : fs::directory_iterator(destination.path())) {
        check(item.path().filename().string().rfind(".files-transfer-", 0) != 0, "staged copy was left behind");
    }
    std::cout << "Real mount detached between staging and publication; source kept and no destination residue\n";
}

void testLoop(const fs::path& mount, const fs::path& temp)
{
    FileBrowserModel browser(temp.string());
    std::ofstream(temp / "to-drive.txt") << "written from local storage\n";
    std::ofstream(mount / "from-drive.txt") << "written from mounted image\n";
    browser.refresh();
    selectByName(browser, "to-drive.txt");
    const FileEntry local = *browser.selectedEntry();
    browser.showLocations();
    selectByName(browser, mount.filename().string());
    const FileEntry root = *browser.selectedEntry();
    check(root.mountRoot && root.mountId && !root.freeSpace.empty(), "drive should show a name and free space");
    check(!browser.deleteSelected() && !browser.renameSelectedTo("renamed"), "mounted root can be modified");
    check(static_cast<bool>(browser.openSelected()), "could not open mounted volume");
    auto copied = browser.copyEntryTo(local, mount.string());
    check(static_cast<bool>(copied) && fs::exists(mount / "to-drive.txt"), copied.message.c_str());
    selectByName(browser, "from-drive.txt");
    const FileEntry remote = *browser.selectedEntry();
    check(remote.mountId == root.mountId && !remote.mountRoot, "file should retain removable mount identity");
    check(static_cast<bool>(browser.goBack()) && browser.inLocations(), "drive Escape must return to locations");
    check(static_cast<bool>(browser.goBack()) && browser.currentDirectory().get() == temp.string(),
          "locations Escape must return to the folder");
    copied = browser.copyEntryTo(remote, temp.string());
    check(static_cast<bool>(copied) && fs::exists(temp / "from-drive.txt"), copied.message.c_str());

    // The caller owns the temporary loop mount, allowing an actual dismount after Files releases its descriptors.
    const auto detached = runCommand({"/usr/bin/umount", mount.string()});
    check(static_cast<bool>(detached), detached.output.c_str());
    std::ofstream(mount / "from-drive.txt") << "underlying path; must not be copied\n";
    const auto lost = browser.copyEntryTo(remote, temp.string());
    check(!lost && lost.message == "Drive disconnected" && !fs::exists(temp / "from-drive copy.txt"),
          "a disconnected mount must never fall through to the underlying filesystem");
    std::cout << "Loop image visible, copy in both directions, unmount and stale clipboard verified\n";
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        TempDirectory temp;
        if (argc == 3) {
            testDisconnectDuringCopy(argv[1], temp.path);
        } else if (argc == 2) {
            testLoop(argv[1], temp.path);
        } else {
            testMountList(temp.path);
            testDeb(temp.path);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
