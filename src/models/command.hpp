#pragma once

#include <functional>
#include <string>
#include <vector>

namespace files::internal {

struct CommandResult {
    int exitCode = -1;
    std::string output;

    explicit operator bool() const { return exitCode == 0; }
};

using CommandOutput = std::function<void(const std::string&)>;
CommandResult runCommand(const std::vector<std::string>& arguments, const CommandOutput& onOutput = {},
                         const std::string& input = {}, bool captureStdout = true);
void clearSecret(std::string& value);

}  // namespace files::internal
