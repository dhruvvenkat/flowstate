#pragma once

#include <string>
#include <string_view>

namespace flowstate {

enum class CommandType {
    Invalid,
    Open,
    Write,
    Quit,
    Build,
    Find,
    Goto,
    EnableAi,
    AiExplain,
    AiFix,
    AiRefactor,
    AiError,
    PatchAccept,
    PatchReject,
    PatchAcceptAll,
    PatchRejectAll,
};

struct Command {
    CommandType type = CommandType::Invalid;
    std::string argument;
    std::string error;
};

Command ParseCommand(std::string_view input);

}  // namespace flowstate
