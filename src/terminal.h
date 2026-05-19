#pragma once

#include <termios.h>

#include <string>
#include <utility>

namespace flowstate {

enum class KeyType {
    Unknown,
    Character,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    DeleteKey,
    Home,
    End,
    PageUp,
    PageDown,
    Tab,
    Enter,
    Escape,
    Backspace,
    Paste,
    MouseWheelUp,
    MouseWheelDown,
};

struct KeyPress {
    KeyType type = KeyType::Unknown;
    char ch = '\0';
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    std::string text;
};

class Terminal {
  public:
    Terminal();
    ~Terminal();

    bool EnableRawMode(std::string* error = nullptr);
    KeyPress ReadKey() const;
    std::pair<int, int> WindowSize() const;
    void Write(const std::string& text) const;

  private:
    bool raw_mode_enabled_ = false;
    ::termios* original_mode_;
};

constexpr char CtrlKey(char value) { return static_cast<char>(value & 0x1f); }

}  // namespace flowstate
