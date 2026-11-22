#include "models/command.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace files::internal {

void clearSecret(std::string& value)
{
    volatile char* bytes = value.empty() ? nullptr : &value[0];
    for (size_t i = 0; i < value.size(); ++i) {
        bytes[i] = 0;
    }
    value.clear();
}

namespace {

struct CommandFds {
    int output[2] = {-1, -1};
    int input[2] = {-1, -1};
    ~CommandFds()
    {
        for (int fd : {output[0], output[1], input[0], input[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }
};

}  // namespace

CommandResult runCommand(const std::vector<std::string>& arguments, const CommandOutput& onOutput,
                         const std::string& input, bool captureStdout)
{
    CommandFds fds;
    if (pipe(fds.output) < 0 || (!input.empty() && socketpair(AF_UNIX, SOCK_STREAM, 0, fds.input) < 0)) {
        return {-1, std::strerror(errno)};
    }
#if defined(__APPLE__)
    if (fds.input[0] >= 0) {
        const int enabled = 1;
        if (setsockopt(fds.input[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
            return {-1, std::strerror(errno)};
        }
    }
#endif

    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        return {-1, std::strerror(error)};
    }
    if (input.empty()) {
        error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    } else {
        error = posix_spawn_file_actions_adddup2(&actions, fds.input[1], STDIN_FILENO);
    }
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&actions, fds.output[1], STDERR_FILENO);
    }
    if (!error) {
        error = captureStdout ? posix_spawn_file_actions_adddup2(&actions, fds.output[1], STDOUT_FILENO)
                              : posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    for (int fd : {fds.output[0], fds.output[1], fds.input[0], fds.input[1]}) {
        if (fd >= 0 && !error) {
            error = posix_spawn_file_actions_addclose(&actions, fd);
        }
    }

    std::vector<char*> argv;
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    char locale[] = "LC_ALL=C";
    std::vector<char*> env{locale};
    for (char** entry = environ; *entry; ++entry) {
        if (std::strncmp(*entry, "LC_ALL=", 7) != 0) {
            env.push_back(*entry);
        }
    }
    env.push_back(nullptr);

    pid_t child = -1;
    if (!error) {
        error = posix_spawn(&child, argv.front(), &actions, nullptr, argv.data(), env.data());
    }
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        return {-1, std::strerror(error)};
    }

    close(fds.output[1]);
    fds.output[1] = -1;
    if (fds.input[0] >= 0) {
        close(fds.input[1]);
        fds.input[1] = -1;
        size_t sent = 0;
        while (sent < input.size()) {
#if defined(__APPLE__)
            constexpr int flags = 0;
#else
            constexpr int flags = MSG_NOSIGNAL;
#endif
            const ssize_t count = send(fds.input[0], input.data() + sent, input.size() - sent, flags);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            sent += static_cast<size_t>(count);
        }
        close(fds.input[0]);
        fds.input[0] = -1;
    }

    constexpr size_t outputLimit = 128 * 1024;
    CommandResult result;
    char buffer[4096];
    int readError = 0;
    for (;;) {
        const ssize_t count = read(fds.output[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            readError = count < 0 ? errno : 0;
            break;
        }
        std::string chunk(buffer, static_cast<size_t>(count));
        if (onOutput) {
            onOutput(chunk);
        }
        result.output += chunk;
        if (result.output.size() > outputLimit) {
            result.output.erase(0, result.output.size() - outputLimit);
        }
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (!readError && waited == child && WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    }
    return result;
}

}  // namespace files::internal
