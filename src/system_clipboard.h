#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace flowstate {

bool WriteSystemClipboard(std::string_view text, std::string* error = nullptr);
std::optional<std::string> ReadSystemClipboard(std::string* error = nullptr);
std::string Osc52ClipboardSequence(std::string_view text);

}  // namespace flowstate
