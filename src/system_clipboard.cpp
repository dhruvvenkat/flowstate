#include "system_clipboard.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace flowstate {

namespace {

struct ClipboardCommand {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::string display_name;
};

bool IsExecutable(const std::filesystem::path& path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

std::optional<std::filesystem::path> FindExecutable(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return std::nullopt;
    }

    std::string_view paths(path_env);
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(':', start);
        const std::string_view entry =
            end == std::string_view::npos ? paths.substr(start) : paths.substr(start, end - start);
        const std::filesystem::path candidate =
            (entry.empty() ? std::filesystem::current_path() : std::filesystem::path(entry)) / std::string(name);
        if (IsExecutable(candidate)) {
            return candidate;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

bool HasEnv(std::string_view name) {
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    return value != nullptr && *value != '\0';
}

bool WriteAll(int fd, std::string_view text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::vector<char*> ArgvFor(const ClipboardCommand& command) {
    std::vector<char*> argv;
    argv.reserve(command.arguments.size() + 2);
    argv.push_back(const_cast<char*>(command.executable.c_str()));
    for (const std::string& argument : command.arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

bool RunCommandWithInput(const ClipboardCommand& command, std::string_view input, std::string* error) {
    int stdin_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) == -1) {
        if (error != nullptr) {
            *error = "Unable to create clipboard pipe.";
        }
        return false;
    }

    const pid_t child = ::fork();
    if (child == -1) {
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        if (error != nullptr) {
            *error = "Unable to start " + command.display_name + ": " + std::strerror(errno);
        }
        return false;
    }

    if (child == 0) {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        const int dev_null = ::open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDOUT_FILENO);
            ::dup2(dev_null, STDERR_FILENO);
        }
        std::vector<char*> argv = ArgvFor(command);
        ::execv(command.executable.c_str(), argv.data());
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    const bool wrote = WriteAll(stdin_pipe[1], input);
    ::close(stdin_pipe[1]);

    int status = 0;
    while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {
    }
    if (!wrote || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error != nullptr) {
            *error = command.display_name + " could not write the system clipboard.";
        }
        return false;
    }
    return true;
}

std::optional<std::string> RunCommandCaptureOutput(const ClipboardCommand& command, std::string* error) {
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdout_pipe) == -1) {
        if (error != nullptr) {
            *error = "Unable to create clipboard pipe.";
        }
        return std::nullopt;
    }

    const pid_t child = ::fork();
    if (child == -1) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        if (error != nullptr) {
            *error = "Unable to start " + command.display_name + ": " + std::strerror(errno);
        }
        return std::nullopt;
    }

    if (child == 0) {
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        const int dev_null = ::open("/dev/null", O_RDONLY);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDIN_FILENO);
        }
        const int err_null = ::open("/dev/null", O_WRONLY);
        if (err_null >= 0) {
            ::dup2(err_null, STDERR_FILENO);
        }
        std::vector<char*> argv = ArgvFor(command);
        ::execv(command.executable.c_str(), argv.data());
        _exit(127);
    }

    ::close(stdout_pipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes = ::read(stdout_pipe[0], buffer.data(), buffer.size());
        if (bytes > 0) {
            output.append(buffer.data(), static_cast<size_t>(bytes));
            continue;
        }
        if (bytes == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(stdout_pipe[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error != nullptr) {
            *error = command.display_name + " could not read the system clipboard.";
        }
        return std::nullopt;
    }
    return output;
}

std::optional<std::filesystem::path> ClipboardFileOverride() {
    const char* path = std::getenv("FLOWSTATE_CLIPBOARD_FILE");
    if (path == nullptr || *path == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(path);
}

bool WriteClipboardFile(const std::filesystem::path& path, std::string_view text, std::string* error) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output.good()) {
        if (error != nullptr) {
            *error = "Unable to write FLOWSTATE_CLIPBOARD_FILE.";
        }
        return false;
    }
    return true;
}

std::optional<std::string> ReadClipboardFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "Unable to read FLOWSTATE_CLIPBOARD_FILE.";
        }
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return contents;
}

std::optional<ClipboardCommand> WriteCommand() {
    if (HasEnv("WAYLAND_DISPLAY")) {
        if (const std::optional<std::filesystem::path> executable = FindExecutable("wl-copy");
            executable.has_value()) {
            return ClipboardCommand{
                .executable = *executable,
                .arguments = {"--type", "text/plain"},
                .display_name = "wl-copy",
            };
        }
    }
    if (HasEnv("DISPLAY")) {
        if (const std::optional<std::filesystem::path> executable = FindExecutable("xclip");
            executable.has_value()) {
            return ClipboardCommand{
                .executable = *executable,
                .arguments = {"-selection", "clipboard"},
                .display_name = "xclip",
            };
        }
        if (const std::optional<std::filesystem::path> executable = FindExecutable("xsel");
            executable.has_value()) {
            return ClipboardCommand{
                .executable = *executable,
                .arguments = {"--clipboard", "--input"},
                .display_name = "xsel",
            };
        }
    }
    if (const std::optional<std::filesystem::path> executable = FindExecutable("pbcopy");
        executable.has_value()) {
        return ClipboardCommand{
            .executable = *executable,
            .display_name = "pbcopy",
        };
    }
    return std::nullopt;
}

std::optional<ClipboardCommand> ReadCommand() {
    if (HasEnv("WAYLAND_DISPLAY")) {
        if (const std::optional<std::filesystem::path> executable = FindExecutable("wl-paste");
            executable.has_value()) {
            return ClipboardCommand{
                .executable = *executable,
                .arguments = {"--type", "text/plain"},
                .display_name = "wl-paste",
            };
        }
    }
    if (HasEnv("DISPLAY")) {
        if (const std::optional<std::filesystem::path> executable = FindExecutable("xclip");
            executable.has_value()) {
            return ClipboardCommand{
                .executable = *executable,
                .arguments = {"-selection", "clipboard", "-out"},
                .display_name = "xclip",
            };
        }
        if (const std::optional<std::filesystem::path> executable = FindExecutable("xsel");
            executable.has_value()) {
            return ClipboardCommand{
                .executable = *executable,
                .arguments = {"--clipboard", "--output"},
                .display_name = "xsel",
            };
        }
    }
    if (const std::optional<std::filesystem::path> executable = FindExecutable("pbpaste");
        executable.has_value()) {
        return ClipboardCommand{
            .executable = *executable,
            .display_name = "pbpaste",
        };
    }
    return std::nullopt;
}

std::string Base64Encode(std::string_view text) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((text.size() + 2) / 3) * 4);
    for (size_t index = 0; index < text.size(); index += 3) {
        const unsigned int first = static_cast<unsigned char>(text[index]);
        const unsigned int second = index + 1 < text.size() ? static_cast<unsigned char>(text[index + 1]) : 0;
        const unsigned int third = index + 2 < text.size() ? static_cast<unsigned char>(text[index + 2]) : 0;
        const unsigned int combined = (first << 16) | (second << 8) | third;
        encoded.push_back(kAlphabet[(combined >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(combined >> 12) & 0x3F]);
        encoded.push_back(index + 1 < text.size() ? kAlphabet[(combined >> 6) & 0x3F] : '=');
        encoded.push_back(index + 2 < text.size() ? kAlphabet[combined & 0x3F] : '=');
    }
    return encoded;
}

}  // namespace

bool WriteSystemClipboard(std::string_view text, std::string* error) {
    if (const std::optional<std::filesystem::path> path = ClipboardFileOverride(); path.has_value()) {
        return WriteClipboardFile(*path, text, error);
    }

    const std::optional<ClipboardCommand> command = WriteCommand();
    if (!command.has_value()) {
        if (error != nullptr) {
            *error = "No system clipboard writer found.";
        }
        return false;
    }
    return RunCommandWithInput(*command, text, error);
}

std::optional<std::string> ReadSystemClipboard(std::string* error) {
    if (const std::optional<std::filesystem::path> path = ClipboardFileOverride(); path.has_value()) {
        return ReadClipboardFile(*path, error);
    }

    const std::optional<ClipboardCommand> command = ReadCommand();
    if (!command.has_value()) {
        if (error != nullptr) {
            *error = "No system clipboard reader found.";
        }
        return std::nullopt;
    }
    return RunCommandCaptureOutput(*command, error);
}

std::string Osc52ClipboardSequence(std::string_view text) {
    return "\x1b]52;c;" + Base64Encode(text) + "\x07";
}

}  // namespace flowstate
