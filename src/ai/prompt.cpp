#include "ai/prompt.h"

#include <sstream>

namespace flowstate {

namespace {

void AppendCodeFence(std::ostringstream& output, const std::string& language, const std::string& text) {
    output << "```" << language << "\n" << text << "\n```\n";
}

}  // namespace

std::string BuildPrompt(const AiRequest& request) {
    std::ostringstream prompt;

    switch (request.kind) {
        case AiRequestKind::Explain:
            prompt << "You are helping explain code inside a terminal editor.\n\n";
            prompt << "File: " << request.file_path << "\n";
            prompt << "Language: " << request.language << "\n";
            prompt << "Cursor: line " << (request.cursor.row + 1) << ", column " << (request.cursor.col + 1)
                   << "\n\nSelected code:\n";
            AppendCodeFence(prompt, request.language, request.selected_text);
            prompt << "\nExplain what this code does, identify any likely issues, and explain the underlying "
                      "programming concepts clearly.\n";
            if (!request.user_instruction.empty()) {
                prompt << "\nUser request:\n" << request.user_instruction << "\n";
            }
            break;
        case AiRequestKind::Fix:
        case AiRequestKind::Refactor:
            prompt << "You are helping modify code inside a terminal editor.\n";
            prompt << "Return your proposed change as a unified diff only. Do not rewrite the entire file unless "
                      "necessary. Keep the patch minimal.\n\n";
            prompt << "File: " << request.file_path << "\n";
            prompt << "Language: " << request.language << "\n\n";
            prompt << "Context before selection:\n";
            AppendCodeFence(prompt, request.language, request.context_before);
            prompt << "\nSelected code:\n";
            AppendCodeFence(prompt, request.language, request.selected_text);
            prompt << "\nContext after selection:\n";
            AppendCodeFence(prompt, request.language, request.context_after);
            prompt << "\nUser request:\n" << request.user_instruction << "\n";
            break;
        case AiRequestKind::ErrorExplain:
            prompt << "You are helping debug a compiler/build error inside a terminal editor.\n\n";
            prompt << "File: " << request.file_path << "\n";
            prompt << "Language: " << request.language << "\n\n";
            prompt << "Build command:\n" << request.build_command << "\n\n";
            prompt << "Build output:\n```text\n" << request.build_output << "\n```\n\n";
            prompt << "Relevant code:\n";
            AppendCodeFence(prompt, request.language, request.selected_text);
            prompt << "\nExplain the error, identify the likely cause, and suggest a minimal fix. If a code change "
                      "is needed, provide it as a unified diff.\n";
            break;
    }

    return prompt.str();
}

}  // namespace flowstate
