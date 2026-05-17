#include "command.h"

#include <sstream>
#include <vector>

namespace flowstate {

namespace {

std::vector<std::string> Tokenize(std::string_view input) {
    std::istringstream stream{std::string(input)};
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string TrimColon(std::string_view input) {
    std::string trimmed(input);
    if (!trimmed.empty() && trimmed.front() == ':') {
        trimmed.erase(trimmed.begin());
    }
    return trimmed;
}

}  // namespace

Command ParseCommand(std::string_view input) {
    const std::string normalized = TrimColon(input);
    const std::vector<std::string> tokens = Tokenize(normalized);
    if (tokens.empty()) {
        return {.error = "Empty command."};
    }

    if (tokens[0] == "open") {
        if (tokens.size() < 2) {
            return {.error = "Usage: :open <file>"};
        }
        return {.type = CommandType::Open, .argument = normalized.substr(normalized.find(' ') + 1)};
    }
    if (tokens[0] == "write") {
        return {.type = CommandType::Write};
    }
    if (tokens[0] == "quit") {
        return {.type = CommandType::Quit};
    }
    if (tokens[0] == "build") {
        return {.type = CommandType::Build};
    }
    if (tokens[0] == "find") {
        if (tokens.size() < 2) {
            return {.error = "Usage: :find <text>"};
        }
        return {.type = CommandType::Find, .argument = normalized.substr(normalized.find(' ') + 1)};
    }
    if (tokens[0] == "goto") {
        if (tokens.size() < 2) {
            return {.error = "Usage: :goto <line>"};
        }
        return {.type = CommandType::Goto, .argument = tokens[1]};
    }
    if (tokens[0] == "enable-ai") {
        if (tokens.size() < 2 || (tokens[1] != "openai" && tokens[1] != "codex")) {
            return {.error = "Usage: :enable-ai <openai|codex>"};
        }
        return {.type = CommandType::EnableAi, .argument = tokens[1]};
    }
    if (tokens[0] == "ai" && tokens.size() >= 2) {
        if (tokens[1] == "explain") {
            return {.type = CommandType::AiExplain};
        }
        if (tokens[1] == "fix") {
            return {.type = CommandType::AiFix};
        }
        if (tokens[1] == "refactor") {
            return {.type = CommandType::AiRefactor};
        }
        if (tokens[1] == "error") {
            return {.type = CommandType::AiError};
        }
    }
    if (tokens[0] == "patch" && tokens.size() >= 2) {
        if (tokens[1] == "accept") {
            return {.type = CommandType::PatchAccept};
        }
        if (tokens[1] == "reject") {
            return {.type = CommandType::PatchReject};
        }
        if (tokens[1] == "accept-all") {
            return {.type = CommandType::PatchAcceptAll};
        }
        if (tokens[1] == "reject-all") {
            return {.type = CommandType::PatchRejectAll};
        }
    }

    return {.error = "Unknown command."};
}

}  // namespace flowstate
