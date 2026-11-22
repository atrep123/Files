#include "models/debian_package.hpp"
#include "models/debian_install.hpp"

#include "models/removable_volumes.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace files::internal {
namespace fs = std::filesystem;
namespace {

std::string trim(std::string text)
{
    const auto start = text.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? "" : text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

bool packageNameValid(const std::string& name)
{
    return name.size() >= 2 && name.front() != '-' && name.front() != '+' && name.front() != '.' &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789+.-") == std::string::npos;
}

bool supportedArchitecture(const std::string& architecture)
{
    if (architecture == "all") {
        return true;
    }
    const auto native = runCommand({"/usr/bin/dpkg", "--print-architecture"});
    if (native && trim(native.output) == architecture) {
        return true;
    }
    const auto foreign = runCommand({"/usr/bin/dpkg", "--print-foreign-architectures"});
    if (foreign) {
        std::istringstream names(foreign.output);
        for (std::string name; names >> name;) {
            if (name == architecture) {
                return true;
            }
        }
    }
    return false;
}

#if defined(__linux__)
struct Descriptor {
    int fd;
    ~Descriptor() { if (fd >= 0) close(fd); }
};

std::string writeBytes(int fd, const char* buffer, ssize_t count)
{
    for (ssize_t written = 0; written < count;) {
        const auto part = write(fd, buffer + written, static_cast<size_t>(count - written));
        if (part < 0 && errno == EINTR) {
            continue;
        }
        if (part <= 0) {
            return part < 0 ? std::strerror(errno) : "Could not write package";
        }
        written += part;
    }
    return {};
}

std::string stageFile(const fs::path& source, const fs::path& destination, int reviewed)
{
    const Descriptor input{open(source.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    if (input.fd < 0) {
        return errno == ELOOP ? "Symbolic links to packages are not supported" : std::strerror(errno);
    }
    struct stat before{};
    if (fstat(input.fd, &before) != 0 || !S_ISREG(before.st_mode)) {
        return "Not a regular file";
    }
    if (const auto error = debStagingError(destination.parent_path(), before.st_size); !error.empty()) {
        return error;
    }
    Descriptor output{open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (output.fd < 0) {
        return std::strerror(errno);
    }
    char buffer[65536];
    off_t remaining = before.st_size;
    while (remaining > 0) {
        const auto count = read(input.fd, buffer, static_cast<size_t>(std::min<off_t>(remaining, sizeof(buffer))));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return count < 0 ? std::strerror(errno) : "Package changed while reading";
        }
        auto error = writeBytes(reviewed, buffer, count);
        if (error.empty()) {
            error = writeBytes(output.fd, buffer, count);
        }
        if (!error.empty()) {
            return error;
        }
        remaining -= count;
    }
    struct stat after{};
    if (fstat(input.fd, &after) != 0 || after.st_size != before.st_size ||
        after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec != before.st_ctim.tv_sec || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec) {
        return "Package changed while reading";
    }
    const int closed = close(output.fd);
    output.fd = -1;
    if (closed != 0 || fcntl(reviewed, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        return std::strerror(errno);
    }
    return {};
}

CommandResult readSnapshot(int fd, const std::vector<std::string>& command, bool stdoutOutput = true)
{
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return {-1, std::strerror(errno)};
    }
    return runCommand(command, {}, {}, stdoutOutput, fd);
}
#endif

std::string debDetails(const DebMetadata& metadata)
{
    return "Package: " + metadata.name + "\nVersion: " + metadata.version +
           "   Arch: " + metadata.architecture +
           "\nDepends: " + (metadata.depends.empty() ? "None" : metadata.depends) +
           (metadata.preDepends.empty() ? "" : "\nPre-Depends: " + metadata.preDepends) +
           "\nDescription: " + metadata.description;
}

DebInstallResult commandFailure(const std::string& title, const CommandResult& result)
{
    return {DebInstallState::Failed, title + "\n\n" + trim(cleanCommandText(result.output))};
}

}  // namespace

PreparedDeb::PreparedDeb(fs::path directory) : path(std::move(directory) / "package.deb") {}

PreparedDeb::~PreparedDeb()
{
    std::error_code error;
    fs::remove(path, error);
    fs::remove(path.parent_path(), error);
}

std::string cleanCommandText(std::string text)
{
    for (char& c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == '\r') {
            c = '\n';
        } else if (byte < 0x20 && byte != '\n' && byte != '\t') {
            c = ' ';
        }
    }
    return text;
}

DebMetadata parseDebInfo(const std::string& output)
{
    DebMetadata metadata;
    std::istringstream lines(output);
    std::string* continued = nullptr;
    for (std::string line; std::getline(lines, line);) {
        if (line.empty() || line.front() != ' ') {
            continued = nullptr;
            continue;
        }
        line.erase(0, 1);  // dpkg-deb -I prefixes every control line with a space.
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t') && continued) {
            const auto value = trim(line);
            if (continued == &metadata.description) {
                *continued += value == "." ? "\n" : "\n" + value;
            } else {
                *continued += " " + value;
            }
            continue;
        }
        continued = nullptr;
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0, colon);
        const auto value = trim(line.substr(colon + 1));
        if (key == "Package") {
            metadata.name = value;
        } else if (key == "Version") {
            metadata.version = value;
        } else if (key == "Architecture") {
            metadata.architecture = value;
        } else if (key == "Description") {
            metadata.description = value;
            continued = &metadata.description;
        } else if (key == "Depends" || key == "Pre-Depends") {
            continued = key == "Depends" ? &metadata.depends : &metadata.preDepends;
            *continued = value;
        }
    }
    return metadata;
}

DebInspection inspectDeb(const fs::path& source, uint64_t mountId, const CommandOutput& metadataReady)
{
#if defined(__linux__)
    std::string temporary = "/var/tmp/cardputer-files-deb-XXXXXX";
    if (!mkdtemp(temporary.data())) {
        return {nullptr, "Cannot prepare package: " + std::string(std::strerror(errno))};
    }
    auto package = std::make_unique<PreparedDeb>(temporary);
    const Descriptor reviewed{memfd_create("files-reviewed-package", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (reviewed.fd < 0) {
        return {nullptr, "Cannot seal the package for review: " + std::string(std::strerror(errno))};
    }
    {
        const auto parent = source.parent_path().empty() ? fs::path(".") : source.parent_path();
        const TransferDirectory directory(parent, mountId);
        if (!directory.connected()) {
            return {nullptr, "Drive disconnected"};
        }
        const auto error = stageFile(directory.path() / source.filename(), package->path, reviewed.fd);
        if (!directory.connected()) {
            return {nullptr, "Drive disconnected"};
        }
        if (!error.empty()) {
            return {nullptr, "Cannot read package: " + error};
        }
    }

    const auto info = readSnapshot(reviewed.fd, {"/usr/bin/dpkg-deb", "-I", "-"});
    if (!info || info.outputTruncated) {
        return {nullptr, "Invalid or unreadable Debian package\n\n" + cleanCommandText(info.output)};
    }
    package->metadata = parseDebInfo(cleanCommandText(info.output));
    const auto& metadata = package->metadata;
    if (!packageNameValid(metadata.name) || metadata.version.empty() ||
        metadata.architecture.empty() || metadata.description.empty()) {
        return {nullptr, "Invalid Debian package metadata"};
    }
    std::string details = debDetails(metadata);
    if (metadataReady) {
        metadataReady(details);
    }
    const auto contents = readSnapshot(reviewed.fd, {"/usr/bin/dpkg-deb", "--contents", "-"}, false);
    if (!contents) {
        return {nullptr, "Damaged Debian package\n\n" + cleanCommandText(contents.output)};
    }
    if (!supportedArchitecture(metadata.architecture)) {
        return {std::move(package), "Unsupported architecture: " + metadata.architecture + "\n\n" + details, false};
    }
    package->sha256 = sha256CommandOutput(readSnapshot(reviewed.fd, {"/usr/bin/sha256sum"}));
    if (package->sha256.empty()) {
        return {nullptr, "Cannot calculate the SHA-256 of the reviewed package"};
    }

    const auto simulated = runCommand(debAptCommand(package->path, true));
    const auto plan = debAptPlan(simulated.output);
    if (simulated && !simulated.outputTruncated && !plan.empty()) {
        package->aptPlanSha256 = sha256CommandOutput(runCommand({"/usr/bin/sha256sum"}, {}, plan));
    }
    if (package->aptPlanSha256.empty()) {
        package->planError = "Cannot verify the APT installation plan. Check dependencies and package manager state.\n\n" +
                             cleanCommandText(simulated.output);
        const auto error = package->planError;
        return {std::move(package), details + "\n\n" + error, false};
    }
    details = "Installation can run package scripts as root.\n\n" + details +
              "\n\nKeep local configuration (dpkg --force-confdef --force-confold)."
              "\n\nAPT plan (simulation)\n" + cleanCommandText(plan);
    return {std::move(package), std::move(details), true};
#else
    (void)source;
    (void)mountId;
    (void)metadataReady;
    return {nullptr, "Debian packages require a Debian-compatible Linux system"};
#endif
}

DebInstallResult installDeb(const PreparedDeb& package, std::string& password, const CommandOutput& progress)
{
    struct ClearPassword {
        std::string& password;
        ~ClearPassword() { clearSecret(password); }
    } clear{password};

#if defined(__linux__)
    if (package.sha256.empty() || package.aptPlanSha256.empty()) {
        return {DebInstallState::Failed, package.planError.empty() ? "Open the Debian package for review first."
                                                                  : package.planError};
    }
    if (!trustedSystemExecutable(debHelperExecutable)) {
        return {DebInstallState::Failed, "The installed Files package helper is missing or its path is writable."};
    }
    const auto update = [&](const auto& message) {
        if (progress) {
            progress(message);
        }
    };
    const bool suppliedPassword = !password.empty();
    if (suppliedPassword) {
        password += '\n';
        update("Checking sudo authorization...\n");
        const auto auth = runCommand({"/usr/bin/sudo", "-S", "-p", "", "-v"}, {}, password);
        if (!auth) {
            const bool retry = auth.exitCode == 1 && (auth.output.find("password") != std::string::npos ||
                                                      auth.output.find("Sorry") != std::string::npos);
            if (retry) {
                return {DebInstallState::AuthenticationFailed, "System password required"};
            }
            return commandFailure("Sudo authorization failed", auth);
        }
    }

    clearSecret(password);
    const Descriptor input{open(package.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    struct stat info{};
    if (input.fd < 0 || fstat(input.fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        return {DebInstallState::Failed, "The prepared package changed after review or cannot be opened. Open it again."};
    }
    std::string storeState;
    if (const char* configured = std::getenv("M5APPSTORE_STATE_DIR"); configured && *configured) {
        if (std::string(configured).rfind("~/", 0) == 0 && std::getenv("HOME")) {
            storeState = (fs::path(std::getenv("HOME")) / (configured + 2)).string();
        } else {
            storeState = fs::absolute(configured).string();
        }
    }
    const auto& metadata = package.metadata;
    const std::vector<std::string> elevated{"/usr/bin/sudo", "-n", "--", debHelperExecutable, debHelperOption,
                                            package.sha256, package.aptPlanSha256, metadata.name, metadata.version,
                                            storeState};
    const auto result = runCommand(elevated, progress, {}, true, input.fd);
    if (!result) {
        const std::string output = trim(result.output);
        const std::string passwordRequired = "sudo: a password is required";
        if (!suppliedPassword && result.exitCode == 1 &&
            (output == passwordRequired || output.find("\n" + passwordRequired) != std::string::npos)) {
            return {DebInstallState::PasswordRequired, "System password required"};
        }
        auto failure = commandFailure("Package installation failed", result);
        if (result.output.find("dpkg --audit") == std::string::npos) {
            failure.message += "\n\n" + debAuditMessage();
        }
        return failure;
    }
    return {DebInstallState::Installed, "Installed " + metadata.name + " " + metadata.version};
#else
    (void)package;
    (void)progress;
    return {DebInstallState::Failed, "Debian packages require a Debian-compatible Linux system"};
#endif
}

}  // namespace files::internal
