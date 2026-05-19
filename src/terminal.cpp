#include "terminal.h"

#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <string>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace flowstate {

namespace {

::termios* g_original_mode = nullptr;
bool g_raw_mode_enabled = false;

KeyPress CharacterKey(char ch, bool alt = false) {
    return {.type = KeyType::Character, .ch = ch, .alt = alt};
}

KeyPress ArrowKey(char final_byte, bool shift = false) {
    switch (final_byte) {
        case 'A':
            return {.type = KeyType::ArrowUp, .shift = shift};
        case 'B':
            return {.type = KeyType::ArrowDown, .shift = shift};
        case 'C':
            return {.type = KeyType::ArrowRight, .shift = shift};
        case 'D':
            return {.type = KeyType::ArrowLeft, .shift = shift};
    }
    return {.type = KeyType::Escape};
}

KeyPress HomeEndKey(char final_byte, bool shift = false) {
    switch (final_byte) {
        case 'H':
            return {.type = KeyType::Home, .shift = shift};
        case 'F':
            return {.type = KeyType::End, .shift = shift};
    }
    return {.type = KeyType::Escape};
}

bool ModifierHasShift(std::string_view modifier) {
    int value = 0;
    for (char ch : modifier) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    return value > 1 && ((value - 1) & 1) != 0;
}

bool ConsumeUnsigned(std::string_view text, size_t* index, int* value) {
    if (index == nullptr || value == nullptr || *index >= text.size() || text[*index] < '0' ||
        text[*index] > '9') {
        return false;
    }

    int parsed = 0;
    while (*index < text.size() && text[*index] >= '0' && text[*index] <= '9') {
        parsed = parsed * 10 + (text[*index] - '0');
        ++(*index);
    }
    *value = parsed;
    return true;
}

bool LooksLikeModifiedNavigationKey(char final_byte) {
    return final_byte == 'A' || final_byte == 'B' || final_byte == 'C' || final_byte == 'D' ||
           final_byte == 'H' || final_byte == 'F';
}

KeyPress MouseControlSequenceKey(std::string_view sequence) {
    if (sequence.size() < 2 || sequence.front() != '<') {
        return {.type = KeyType::Unknown};
    }

    const char final_byte = sequence.back();
    if (final_byte != 'M' && final_byte != 'm') {
        return {.type = KeyType::Unknown};
    }

    size_t index = 1;
    int button = 0;
    int col = 0;
    int row = 0;
    if (!ConsumeUnsigned(sequence, &index, &button) || index >= sequence.size() || sequence[index++] != ';' ||
        !ConsumeUnsigned(sequence, &index, &col) || index >= sequence.size() || sequence[index++] != ';' ||
        !ConsumeUnsigned(sequence, &index, &row) || index + 1 != sequence.size()) {
        return {.type = KeyType::Unknown};
    }

    if (final_byte == 'm') {
        return {.type = KeyType::Unknown};
    }

    switch (button) {
        case 64:
            return {.type = KeyType::MouseWheelUp};
        case 65:
            return {.type = KeyType::MouseWheelDown};
        default:
            return {.type = KeyType::Unknown};
    }
}

KeyPress ControlSequenceKey(std::string_view sequence) {
    if (sequence.empty()) {
        return {.type = KeyType::Escape};
    }

    const char final_byte = sequence.back();
    if (sequence.front() == '<') {
        return MouseControlSequenceKey(sequence);
    }

    if (sequence.size() == 1) {
        switch (final_byte) {
            case 'A':
            case 'B':
            case 'C':
            case 'D':
                return ArrowKey(final_byte);
            case 'Z':
                return {.type = KeyType::Tab, .shift = true};
            case 'H':
                return HomeEndKey(final_byte);
            case 'F':
                return HomeEndKey(final_byte);
        }
    }

    if (sequence.size() == 2 && LooksLikeModifiedNavigationKey(final_byte) &&
        ModifierHasShift(sequence.substr(0, 1))) {
        if (final_byte == 'A' || final_byte == 'B' || final_byte == 'C' || final_byte == 'D') {
            return ArrowKey(final_byte, true);
        }
        return HomeEndKey(final_byte, true);
    }

    if (final_byte == '~') {
        switch (sequence.front()) {
            case '1':
            case '7':
                return {.type = KeyType::Home};
            case '3':
                return {.type = KeyType::DeleteKey};
            case '4':
            case '8':
                return {.type = KeyType::End};
            case '5':
                return {.type = KeyType::PageUp};
            case '6':
                return {.type = KeyType::PageDown};
        }
    }

    const size_t semicolon = sequence.rfind(';');
    if (semicolon != std::string_view::npos && semicolon + 1 < sequence.size() - 1) {
        const bool shift = ModifierHasShift(sequence.substr(semicolon + 1, sequence.size() - semicolon - 2));
        if (final_byte == 'A' || final_byte == 'B' || final_byte == 'C' || final_byte == 'D') {
            return ArrowKey(final_byte, shift);
        }
        if (final_byte == 'H' || final_byte == 'F') {
            return HomeEndKey(final_byte, shift);
        }
    }

    return {.type = KeyType::Escape};
}

void RestoreTerminalMode() {
    if (g_raw_mode_enabled && g_original_mode != nullptr) {
        ::write(STDOUT_FILENO, "\x1b[?1006l\x1b[?1000l", 16);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, g_original_mode);
        g_raw_mode_enabled = false;
    }
}

void HandleFatalSignal(int signal_number) {
    RestoreTerminalMode();
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

void InstallSignalHandlers() {
    static bool installed = false;
    if (installed) {
        return;
    }

    installed = true;
    std::atexit(RestoreTerminalMode);
    std::signal(SIGABRT, HandleFatalSignal);
    std::signal(SIGBUS, HandleFatalSignal);
    std::signal(SIGFPE, HandleFatalSignal);
    std::signal(SIGHUP, HandleFatalSignal);
    std::signal(SIGILL, HandleFatalSignal);
    std::signal(SIGINT, HandleFatalSignal);
    std::signal(SIGQUIT, HandleFatalSignal);
    std::signal(SIGSEGV, HandleFatalSignal);
    std::signal(SIGTERM, HandleFatalSignal);
}

}  // namespace

Terminal::Terminal() : original_mode_(new termios{}) {}

Terminal::~Terminal() {
    if (g_original_mode == original_mode_) {
        RestoreTerminalMode();
        g_original_mode = nullptr;
    }
    delete original_mode_;
}

bool Terminal::EnableRawMode(std::string* error) {
    if (tcgetattr(STDIN_FILENO, original_mode_) == -1) {
        if (error != nullptr) {
            *error = std::strerror(errno);
        }
        return false;
    }

    termios raw = *original_mode_;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        if (error != nullptr) {
            *error = std::strerror(errno);
        }
        return false;
    }

    raw_mode_enabled_ = true;
    g_original_mode = original_mode_;
    g_raw_mode_enabled = true;
    ::write(STDOUT_FILENO, "\x1b[?1000h\x1b[?1006h", 16);
    InstallSignalHandlers();
    return true;
}

KeyPress Terminal::ReadKey() const {
    auto read_optional_byte = [](char* output, int timeout_ms) -> bool {
        if (output == nullptr) {
            return false;
        }

        pollfd descriptor{
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0,
        };
        while (true) {
            const int ready = poll(&descriptor, 1, timeout_ms);
            if (ready > 0) {
                break;
            }
            if (ready == 0) {
                return false;
            }
            if (errno != EINTR) {
                return false;
            }
        }

        while (true) {
            const ssize_t bytes_read = read(STDIN_FILENO, output, 1);
            if (bytes_read == 1) {
                return true;
            }
            if (bytes_read == 0) {
                return false;
            }
            if (errno != EINTR) {
                return false;
            }
        }
    };

    char ch = '\0';
    while (true) {
        const ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read == 1) {
            break;
        }
        if (bytes_read == 0) {
            return {.type = KeyType::Unknown};
        }
        if (bytes_read == -1 && errno != EAGAIN) {
            return {};
        }
    }

    if (ch == '\x1b') {
        char first = '\0';
        if (!read_optional_byte(&first, 25)) {
            return {.type = KeyType::Escape};
        }
        if (first == '[') {
            std::string sequence;
            for (size_t index = 0; index < 32; ++index) {
                char next = '\0';
                if (!read_optional_byte(&next, 50)) {
                    return {.type = KeyType::Escape};
                }
                sequence.push_back(next);
                if (next >= '@' && next <= '~') {
                    break;
                }
            }
            return ControlSequenceKey(sequence);
        } else if (first == 'O') {
            char second = '\0';
            if (!read_optional_byte(&second, 50)) {
                return {.type = KeyType::Escape};
            }
            switch (second) {
                case 'H':
                    return {.type = KeyType::Home};
                case 'F':
                    return {.type = KeyType::End};
            }
        } else {
            return CharacterKey(first, true);
        }
        return {.type = KeyType::Escape};
    }

    if (ch == '\r') {
        return {.type = KeyType::Enter};
    }
    if (ch == '\t') {
        return {.type = KeyType::Tab};
    }
    if (ch == '\b' || ch == 127) {
        return {.type = KeyType::Backspace};
    }
    if (ch >= 1 && ch <= 26) {
        return {.type = KeyType::Character, .ch = static_cast<char>('a' + ch - 1), .ctrl = true};
    }
    if (ch == CtrlKey('_')) {
        return {.type = KeyType::Character, .ch = '/', .ctrl = true};
    }
    return CharacterKey(ch);
}

std::pair<int, int> Terminal::WindowSize() const {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1 || size.ws_col == 0) {
        return {24, 80};
    }
    return {size.ws_row, size.ws_col};
}

void Terminal::Write(const std::string& text) const {
    ::write(STDOUT_FILENO, text.c_str(), text.size());
}

}  // namespace flowstate
