#pragma once

#include "models/command.hpp"
#include <cstdint>
#include <filesystem>

namespace files::internal {

inline constexpr char debHelperOption[] = "--files-install-reviewed-deb";
inline constexpr char debHelperExecutable[] = "/usr/share/APPLaunch/bin/M5CardputerZero-Files";

std::string debStagingError(const std::filesystem::path& directory, std::uintmax_t bytes);
std::vector<std::string> debAptCommand(const std::filesystem::path& path, bool simulate);
std::string debAptPlan(const std::string& output);
std::string sha256CommandOutput(const CommandResult& result);
std::string debAuditMessage();
bool trustedSystemExecutable(const std::filesystem::path& path);
int runDebInstallHelper(int argc, char** argv);

}  // namespace files::internal
