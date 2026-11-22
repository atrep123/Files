#include "models/debian_package.hpp"

#include "models/removable_volumes.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

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

std::string stageFile(const fs::path& source, const fs::path& destination)
{
    const int input = open(source.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input < 0) {
        return std::strerror(errno);
    }
    struct stat before{};
    if (fstat(input, &before) != 0 || !S_ISREG(before.st_mode)) {
        close(input);
        return "Not a regular file";
    }
    const int output = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) {
        const int error = errno;
        close(input);
        return std::strerror(error);
    }

    std::string error;
    off_t copied = 0;
    char buffer[65536];
    while (copied < before.st_size && error.empty()) {
        const auto remaining = std::min<off_t>(sizeof(buffer), before.st_size - copied);
        const ssize_t count = read(input, buffer, static_cast<size_t>(remaining));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = count < 0 ? std::strerror(errno) : "Package changed while reading";
            break;
        }
        for (ssize_t written = 0; written < count;) {
            const ssize_t length = write(output, buffer + written, static_cast<size_t>(count - written));
            if (length < 0 && errno == EINTR) {
                continue;
            }
            if (length <= 0) {
                error = length < 0 ? std::strerror(errno) : "Could not write package";
                break;
            }
            written += length;
        }
        copied += count;
    }
    struct stat after{};
    if (error.empty() && (fstat(input, &after) != 0 || after.st_size != before.st_size ||
                          after.st_mtime != before.st_mtime || after.st_ctime != before.st_ctime)) {
        error = "Package changed while reading";
    }
    const int closed = close(output);
    if (closed != 0 && error.empty()) {
        error = std::strerror(errno);
    }
    close(input);
    return error;
}

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

DebInspection inspectDeb(const fs::path& source, uint64_t mountId)
{
#if defined(__linux__)
    std::string temporary = "/tmp/cardputer-files-deb-XXXXXX";
    if (!mkdtemp(temporary.data())) {
        return {nullptr, "Cannot prepare package: " + std::string(std::strerror(errno))};
    }
    auto package = std::make_unique<PreparedDeb>(temporary);
    {
        const auto parent = source.parent_path().empty() ? fs::path(".") : source.parent_path();
        const TransferDirectory directory(parent, mountId);
        if (!directory.connected()) {
            return {nullptr, "Drive disconnected"};
        }
        const auto error = stageFile(directory.path() / source.filename(), package->path);
        if (!directory.connected()) {
            return {nullptr, "Drive disconnected"};
        }
        if (!error.empty()) {
            return {nullptr, "Cannot read package: " + error};
        }
    }

    const auto info = runCommand({"/usr/bin/dpkg-deb", "-I", package->path.string()});
    if (!info) {
        return {nullptr, "Invalid or unreadable Debian package\n\n" + cleanCommandText(info.output)};
    }
    package->metadata = parseDebInfo(cleanCommandText(info.output));
    const auto& metadata = package->metadata;
    if (!packageNameValid(metadata.name) || metadata.version.empty() ||
        metadata.architecture.empty() || metadata.description.empty()) {
        return {nullptr, "Invalid Debian package metadata"};
    }
    const auto contents = runCommand({"/usr/bin/dpkg-deb", "--contents", package->path.string()}, {}, {}, false);
    if (!contents) {
        return {nullptr, "Damaged Debian package\n\n" + cleanCommandText(contents.output)};
    }
    std::string details = debDetails(metadata);
    const bool compatible = supportedArchitecture(metadata.architecture);
    if (!compatible) {
        details = "Unsupported architecture: " + metadata.architecture + "\n\n" + details;
    }
    return {std::move(package), std::move(details), compatible};
#else
    (void)source;
    (void)mountId;
    return {nullptr, "Debian packages require a Debian-compatible Linux system"};
#endif
}

DebInstallResult installDeb(const PreparedDeb& package, std::string& password, const CommandOutput& progress)
{
    struct ClearPassword {
        std::string& password;
        ~ClearPassword() { clearSecret(password); }
    } clear{password};

    const std::vector<std::string> apt{"/usr/bin/apt-get", "-y", "--no-remove", "--no-install-recommends",
                                       "-o", "APT::Status-Fd=1", "install", package.path.string()};
    const auto update = [&](const auto& message) {
        if (progress) {
            progress(message);
        }
    };
    update("Checking package dependencies...\n");
    auto simulation = apt;
    simulation.insert(simulation.begin() + 1, "--simulate");
    const auto plan = runCommand(simulation);
    if (!plan) {
        return commandFailure("Cannot install package. Check dependencies and package manager state.", plan);
    }
    const auto audit = runCommand({"/usr/bin/dpkg", "--audit"});
    if (!audit || !trim(audit.output).empty()) {
        return commandFailure("Package manager needs attention", audit);
    }

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

    // Store uses dpkg when dependencies are already met, and apt-get to resolve them otherwise.
    const auto& metadata = package.metadata;
    std::string dependencies = metadata.preDepends;
    if (!metadata.depends.empty()) {
        dependencies += (dependencies.empty() ? "" : ", ") + metadata.depends;
    }
    const bool installedDependencies = dependencies.empty() ||
        static_cast<bool>(runCommand({"/usr/bin/dpkg-checkbuilddeps", "-d", dependencies, "/dev/null"}));
    const std::vector<std::string> dpkg{"/usr/bin/dpkg", "--install", package.path.string()};
    const auto& command = installedDependencies ? dpkg : apt;
    // Forward only the package command. Its stdin never receives the password.
    clearSecret(password);
    std::vector<std::string> elevated{"/usr/bin/sudo", "-n", "--"};
    elevated.insert(elevated.end(), command.begin(), command.end());
    update("Installing " + metadata.name + "...\n");
    const auto result = runCommand(elevated, progress);
    if (!result) {
        // The password policy for sudo -v can differ from that for this command.
        const std::string output = trim(result.output);
        const std::string passwordRequired = "sudo: a password is required";
        if (!suppliedPassword && result.exitCode == 1 &&
            (output == passwordRequired || output.find("\n" + passwordRequired) != std::string::npos)) {
            return {DebInstallState::PasswordRequired, "System password required"};
        }
        return commandFailure("Package installation failed", result);
    }
    const std::string queryName = metadata.architecture == "all" ? metadata.name
                                                                : metadata.name + ":" + metadata.architecture;
    const auto status = runCommand({"/usr/bin/dpkg-query", "-W", "-f=${Status}\n${Version}", "--", queryName});
    if (!status || trim(status.output) != "install ok installed\n" + metadata.version) {
        return commandFailure("Could not verify the installed package", status);
    }
    return {DebInstallState::Installed, "Installed " + metadata.name + " " + metadata.version};
}

}  // namespace files::internal
