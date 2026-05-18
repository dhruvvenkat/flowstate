#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ai/codex_client.h"
#include "ai/no_ai_client.h"
#include "app.h"
#include "build.h"
#include "buffer.h"
#include "command.h"
#include "diff.h"
#include "editor_state.h"
#include "git_status.h"
#include "intellisense/clangd_client.h"
#include "intellisense/completion.h"
#include "json.h"
#include "patch.h"
#include "selection.h"
#include "screen.h"
#include "syntax/cpp_highlighter.h"
#include "syntax/go_highlighter.h"
#include "syntax/javascript_highlighter.h"
#include "syntax/java_highlighter.h"
#include "syntax/markdown_highlighter.h"
#include "syntax/python_highlighter.h"
#include "syntax/rust_highlighter.h"
#include "syntax/theme.h"

namespace flowstate {

class EditorAppTestPeer {
  public:
    static EditorState& State(EditorApp& app) { return app.state_; }

    static bool HandleInlineAiKey(EditorApp& app, const KeyPress& key) {
        return app.HandleInlineAiKey(key);
    }
};

}  // namespace flowstate

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestBufferEditing() {
    flowstate::Buffer buffer;
    flowstate::Cursor cursor;
    buffer.insertChar(cursor, 'a');
    cursor.col = 1;
    buffer.insertChar(cursor, 'b');
    cursor.col = 2;
    buffer.insertNewline(cursor);
    buffer.insertChar(cursor, 'c');

    Expect(buffer.lineCount() == 2, "buffer should have two lines");
    Expect(buffer.line(0) == "ab", "first line should be preserved");
    Expect(buffer.line(1) == "c", "second line should contain inserted text");

    buffer.deleteCharBefore(cursor);
    Expect(buffer.line(1).empty(), "backspace should remove the character");
}

void WriteFixtureFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Expect(output.good(), "fixture file should be written");
}

void TestLoadFileRejectsUnreadableContent() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path text_path = directory / "flowstate_readable_fixture.txt";
    const std::filesystem::path binary_path = directory / "flowstate_binary_fixture.bin";
    const std::filesystem::path encoded_path = directory / "flowstate_encoded_fixture.txt";

    std::string readable_text = "hello\ncaf";
    readable_text.push_back(static_cast<char>(0xc3));
    readable_text.push_back(static_cast<char>(0xa9));
    readable_text.push_back('\n');
    WriteFixtureFile(text_path, readable_text);

    std::string error;
    flowstate::Buffer readable = flowstate::LoadFileBuffer(text_path, &error);
    Expect(error.empty(), "valid UTF-8 text files should load");
    Expect(readable.text() == readable_text,
           "loaded text should preserve human-readable content");

    std::string binary_bytes;
    binary_bytes.push_back(0x7f);
    binary_bytes += "ELF";
    binary_bytes.push_back('\0');
    binary_bytes += "binary";
    WriteFixtureFile(binary_path, binary_bytes);

    error.clear();
    flowstate::LoadFileBuffer(binary_path, &error);
    Expect(error.find("non-text") != std::string::npos, "binary files should be rejected");

    std::string invalid_utf8 = "hello ";
    invalid_utf8.push_back(static_cast<char>(0xc3));
    invalid_utf8.push_back('(');
    WriteFixtureFile(encoded_path, invalid_utf8);

    error.clear();
    flowstate::LoadFileBuffer(encoded_path, &error);
    Expect(error.find("encoded") != std::string::npos, "invalid UTF-8 files should be rejected");

    error.clear();
    flowstate::LoadFileBuffer(directory, &error);
    Expect(error.find("non-file") != std::string::npos, "directories should not open as editor buffers");

    std::filesystem::remove(text_path);
    std::filesystem::remove(binary_path);
    std::filesystem::remove(encoded_path);
}

void TestSelectionExtraction() {
    flowstate::Buffer buffer;
    buffer.setText("alpha\nbeta\ngamma", false);
    flowstate::Selection selection{
        .active = true,
        .anchor = {0, 2},
        .head = {2, 2},
    };

    const std::string extracted = flowstate::ExtractSelection(buffer, selection);
    Expect(extracted == "pha\nbeta\nga", "selection extraction should preserve multi-line ranges");
}

void TestSelectionRangeHelpers() {
    flowstate::Buffer buffer;
    buffer.setText("alpha\nbeta\ngamma", false);

    const flowstate::SelectionRange middle_line = flowstate::CurrentLineRange(buffer, {.row = 1, .col = 2});
    Expect(flowstate::ExtractRange(buffer, middle_line) == "beta\n",
           "current line ranges should include the trailing newline for non-final lines");

    const flowstate::SelectionRange final_line = flowstate::CurrentLineRange(buffer, {.row = 2, .col = 1});
    Expect(flowstate::ExtractRange(buffer, final_line) == "gamma",
           "current line ranges should not invent a trailing newline for the final line");
}

void TestBufferRangeEditing() {
    flowstate::Buffer insert_buffer;
    insert_buffer.setText("abcd", false);
    flowstate::Cursor insert_cursor{0, 2};
    insert_buffer.insertText(insert_cursor, "X\nY");
    Expect(insert_buffer.lineCount() == 2, "multi-line paste should split the current line");
    Expect(insert_buffer.line(0) == "abX", "paste should keep the line prefix before the cursor");
    Expect(insert_buffer.line(1) == "Ycd", "paste should keep the line suffix after the cursor");
    Expect(insert_cursor.row == 1 && insert_cursor.col == 1,
           "paste cursor should land at the end of the inserted text");

    flowstate::Buffer same_line_delete_buffer;
    same_line_delete_buffer.setText("abcd", false);
    flowstate::Cursor same_line_cursor{0, 0};
    same_line_delete_buffer.deleteRange(same_line_cursor, {.row = 0, .col = 1}, {.row = 0, .col = 3});
    Expect(same_line_delete_buffer.text() == "ad", "single-line range deletion should remove the selected text");
    Expect(same_line_cursor.row == 0 && same_line_cursor.col == 1,
           "single-line range deletion should move the cursor to the range start");

    flowstate::Buffer multi_line_delete_buffer;
    multi_line_delete_buffer.setText("alpha\nbeta\ngamma", false);
    flowstate::Cursor multi_line_cursor{0, 0};
    multi_line_delete_buffer.deleteRange(multi_line_cursor, {.row = 0, .col = 2}, {.row = 2, .col = 2});
    Expect(multi_line_delete_buffer.text() == "almma",
           "multi-line range deletion should join the prefix and suffix across lines");
    Expect(multi_line_cursor.row == 0 && multi_line_cursor.col == 2,
           "multi-line range deletion should move the cursor to the range start");

    flowstate::Buffer replace_buffer;
    replace_buffer.setText("abcde", false);
    flowstate::Cursor replace_cursor{0, 0};
    replace_buffer.replaceRange(replace_cursor, {.row = 0, .col = 1}, {.row = 0, .col = 4}, "X\nY");
    Expect(replace_buffer.lineCount() == 2, "replacing with multi-line text should split the buffer");
    Expect(replace_buffer.line(0) == "aX", "replace should preserve the prefix before the replaced range");
    Expect(replace_buffer.line(1) == "Ye", "replace should preserve the suffix after the replaced range");
    Expect(replace_cursor.row == 1 && replace_cursor.col == 1,
           "replace should leave the cursor at the end of the inserted text");
}

void TestDeleteRangePlacesCursorAtSelectionStart() {
    flowstate::Buffer same_line_buffer;
    same_line_buffer.setText("abcdef", false);
    flowstate::Cursor same_line_cursor{0, 5};
    same_line_buffer.deleteRange(same_line_cursor, {.row = 0, .col = 2}, {.row = 0, .col = 5});
    Expect(same_line_buffer.text() == "abf", "selected text should be deleted");
    Expect(same_line_cursor.row == 0 && same_line_cursor.col == 2,
           "deleting a same-line selection should place the cursor at the first selected character");

    flowstate::Buffer multi_line_buffer;
    multi_line_buffer.setText("alpha\nbeta\ngamma", false);
    flowstate::Cursor multi_line_cursor{2, 3};
    multi_line_buffer.deleteRange(multi_line_cursor, {.row = 0, .col = 2}, {.row = 2, .col = 2});
    Expect(multi_line_buffer.text() == "almma", "multi-line selected text should be deleted");
    Expect(multi_line_cursor.row == 0 && multi_line_cursor.col == 2,
           "deleting a multi-line selection should place the cursor at the first selected character");
}

void TestIndentedNewlineAndBackspace() {
    flowstate::Buffer buffer;
    buffer.setText("    if (ready) {", false);
    flowstate::Cursor cursor{0, buffer.line(0).size()};

    Expect(flowstate::kIndentWidth == 4, "editor indentation should be four spaces");

    buffer.insertNewline(cursor);
    Expect(buffer.lineCount() == 2, "enter on an indented line should split the buffer");
    Expect(buffer.line(1) == "    ", "new lines should inherit the previous line indentation");
    Expect(cursor.row == 1 && cursor.col == 4, "cursor should land after the copied indentation");

    buffer.deleteCharBefore(cursor);
    Expect(buffer.line(1).empty(), "backspace inside indentation should clear that indentation");
    Expect(cursor.row == 1 && cursor.col == 0, "cursor should move to the start of the line after clearing indentation");

    buffer.setText("\t\treturn value;", false);
    cursor = {0, 2};
    buffer.deleteCharBefore(cursor);
    Expect(buffer.line(0) == "return value;", "tab indentation should also clear as one indentation block");
    Expect(cursor.row == 0 && cursor.col == 0, "clearing tab indentation should place the cursor at line start");

    buffer.setText("        value", false);
    cursor = {0, 8};
    buffer.deleteCharBefore(cursor);
    Expect(buffer.line(0) == "    value", "backspace at an indentation boundary should remove one indent step");
    Expect(cursor.row == 0 && cursor.col == 4, "cursor should move back by one indent step");

    cursor = {0, 2};
    buffer.deleteCharBefore(cursor);
    Expect(buffer.line(0) == "  value", "backspace inside indentation should move to the previous indent stop");
    Expect(cursor.row == 0 && cursor.col == 0, "cursor should land on the previous indent stop");
}

void TestInsertIndentUsesTabStops() {
    flowstate::Buffer buffer;
    flowstate::Cursor cursor{0, 0};

    buffer.insertIndent(cursor);
    Expect(buffer.line(0) == "    ", "tab at column zero should insert one indentation width");
    Expect(cursor.row == 0 && cursor.col == 4, "tab should advance to the next indentation stop");

    buffer.setText("ab", false);
    cursor = {0, 2};
    buffer.insertIndent(cursor);
    Expect(buffer.line(0) == "ab  ", "tab should insert only enough spaces to reach the next stop");
    Expect(cursor.row == 0 && cursor.col == 4, "tab from column two should land on column four");
}

void TestLineCommentToggle() {
    flowstate::Buffer cpp_buffer;
    cpp_buffer.setText("int main() {\n    return 0;\n\n}", false);

    flowstate::LineCommentToggleResult result = cpp_buffer.toggleLineComments(0, 3, "//");
    Expect(result == flowstate::LineCommentToggleResult::Commented, "uncommented lines should be commented");
    Expect(cpp_buffer.text() == "// int main() {\n    // return 0;\n\n// }",
           "line commenting should preserve indentation and skip blank lines");

    result = cpp_buffer.toggleLineComments(0, 3, "//");
    Expect(result == flowstate::LineCommentToggleResult::Uncommented,
           "fully commented lines should be uncommented");
    Expect(cpp_buffer.text() == "int main() {\n    return 0;\n\n}",
           "line uncommenting should restore the original text");

    flowstate::Buffer python_buffer;
    python_buffer.setText("def run():\n    return 1", false);
    result = python_buffer.toggleLineComments(0, 1, "#");
    Expect(result == flowstate::LineCommentToggleResult::Commented, "python lines should use hash comments");
    Expect(python_buffer.text() == "# def run():\n    # return 1",
           "hash comments should be inserted after indentation");
}

void TestPairedCharacterInsertion() {
    flowstate::Buffer buffer;
    buffer.setText("call", false);
    flowstate::Cursor cursor{0, 4};

    buffer.insertPairedChars(cursor, '(', ')');
    Expect(buffer.text() == "call()", "paired insertion should insert both bracket characters");
    Expect(cursor.row == 0 && cursor.col == 5, "paired insertion should leave the cursor between the pair");

    buffer.insertChar(cursor, 'x');
    ++cursor.col;
    Expect(buffer.text() == "call(x)", "typing after paired insertion should happen inside the pair");
}

void TestEditorStateUndoRedo() {
    flowstate::Buffer buffer;
    buffer.setText("alpha", false);

    flowstate::EditorState state(std::move(buffer));
    state.fileCursor() = {0, 2};
    state.selection() = {.active = true, .extend_on_cursor_move = true, .anchor = {0, 1}, .head = {0, 3}};

    state.BeginFileEdit();
    state.fileBuffer().insertText(state.fileCursor(), "X");
    state.clearSelection();
    Expect(state.CommitFileEdit(), "committing a real edit should create an undo entry");
    Expect(state.fileBuffer().text() == "alXpha", "tracked edits should modify the file buffer");
    Expect(state.fileCursor().row == 0 && state.fileCursor().col == 3,
           "tracked edits should preserve the post-edit cursor");
    Expect(state.fileBuffer().dirty(), "tracked edits should mark the buffer dirty");

    Expect(state.UndoFileEdit(), "undo should restore the previous file state");
    Expect(state.fileBuffer().text() == "alpha", "undo should restore the original text");
    Expect(state.fileCursor().row == 0 && state.fileCursor().col == 2,
           "undo should restore the original cursor");
    Expect(state.selection().active && state.selection().extend_on_cursor_move &&
               state.selection().anchor.row == 0 && state.selection().anchor.col == 1 &&
               state.selection().head.row == 0 && state.selection().head.col == 3,
           "undo should restore the original selection");
    Expect(!state.fileBuffer().dirty(), "undo should restore the original dirty state");

    Expect(state.RedoFileEdit(), "redo should restore the edited file state");
    Expect(state.fileBuffer().text() == "alXpha", "redo should restore the edited text");
    Expect(state.fileCursor().row == 0 && state.fileCursor().col == 3,
           "redo should restore the edited cursor");
    Expect(!state.selection().active, "redo should restore the edited selection state");
    Expect(state.fileBuffer().dirty(), "redo should restore the edited dirty state");

    state.fileCursor() = {0, 0};
    state.BeginFileEdit();
    state.fileBuffer().deleteCharBefore(state.fileCursor());
    Expect(!state.CommitFileEdit(), "no-op edits should not create history entries");
}

void TestGitChangePeekExpansionState() {
    flowstate::Buffer buffer;
    buffer.setText("alpha\nbeta", false);

    flowstate::EditorState state(std::move(buffer));
    Expect(!state.hasGitChangePeekExpansions(), "git change peeks should start collapsed");
    state.toggleGitChangePeekExpansion(1);
    Expect(state.hasGitChangePeekExpansions(), "toggling a git change row should expand it");
    Expect(state.isGitChangePeekExpanded(1), "expanded git change rows should be queryable");
    state.toggleGitChangePeekExpansion(1);
    Expect(!state.isGitChangePeekExpanded(1), "toggling the same git change row should collapse it");

    state.toggleGitChangePeekExpansion(0);
    state.BeginFileEdit();
    state.fileBuffer().insertChar(state.fileCursor(), 'x');
    Expect(state.CommitFileEdit(), "editing should still commit after a git change peek was open");
    Expect(!state.hasGitChangePeekExpansions(), "file edits should clear stale git change peeks");
}

void TestCommandParsing() {
    const flowstate::Command open = flowstate::ParseCommand(":open src/main.cpp");
    const flowstate::Command accept_all = flowstate::ParseCommand(":patch accept-all");
    const flowstate::Command find = flowstate::ParseCommand(":find totalBefore");
    const flowstate::Command goto_line = flowstate::ParseCommand(":goto 42");
    const flowstate::Command enable_ai = flowstate::ParseCommand(":enable-ai codex");
    const flowstate::Command missing_ai_provider = flowstate::ParseCommand(":enable-ai");
    const flowstate::Command invalid_ai_provider = flowstate::ParseCommand(":enable-ai nope");
    const flowstate::Command invalid = flowstate::ParseCommand(":patch nope");

    Expect(open.type == flowstate::CommandType::Open, "open command should parse");
    Expect(open.argument == "src/main.cpp", "open command should preserve the path");
    Expect(accept_all.type == flowstate::CommandType::PatchAcceptAll, "accept-all should parse");
    Expect(find.type == flowstate::CommandType::Find, "find command should parse");
    Expect(find.argument == "totalBefore", "find command should preserve the query");
    Expect(goto_line.type == flowstate::CommandType::Goto, "goto command should parse");
    Expect(goto_line.argument == "42", "goto command should preserve the target line");
    Expect(enable_ai.type == flowstate::CommandType::EnableAi, "enable-ai command should parse");
    Expect(enable_ai.argument == "codex", "enable-ai should preserve the requested provider");
    Expect(missing_ai_provider.type == flowstate::CommandType::Invalid &&
               missing_ai_provider.error == "Usage: :enable-ai <openai|codex>",
           "enable-ai should require a provider");
    Expect(invalid_ai_provider.type == flowstate::CommandType::Invalid &&
               invalid_ai_provider.error == "Usage: :enable-ai <openai|codex>",
           "enable-ai should reject unsupported providers");
    Expect(invalid.type == flowstate::CommandType::Invalid, "invalid command should be rejected");
}

void TestDiffParsingAndPatchApply() {
    flowstate::Buffer buffer = flowstate::LoadFileBuffer("sample.cpp");
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    int totalBefore;\n    return totalBefore;\n}", false);

    const std::string diff_text =
        "--- a/sample.cpp\n"
        "+++ b/sample.cpp\n"
        "@@ -1,4 +1,4 @@\n"
        " int main() {\n"
        "-    int totalBefore;\n"
        "+    int totalBefore = 0;\n"
        "     return totalBefore;\n"
        " }\n";

    const flowstate::PatchSet patch = flowstate::ParseUnifiedDiff(diff_text);
    Expect(patch.valid(), "valid diff should parse");
    Expect(patch.hunks.size() == 1, "single hunk expected");
    Expect(flowstate::PatchTargetsBuffer(patch, buffer), "patch should target the current buffer");

    flowstate::PatchSession session = flowstate::CreatePatchSession(patch, buffer);
    const flowstate::PatchApplyResult result = flowstate::AcceptCurrentHunk(buffer, session);
    Expect(result.success, "current hunk should apply");
    Expect(buffer.text().find("int totalBefore = 0;") != std::string::npos,
           "accepted hunk should update the file buffer");
}

void TestDiffExtractionWithProse() {
    const std::string raw =
        "Here is the minimal fix.\n\n"
        "--- a/sample.cpp\n"
        "+++ b/sample.cpp\n"
        "@@ -1,2 +1,2 @@\n"
        "-int totalBefore;\n"
        "+int totalBefore = 0;\n"
        "\n"
        "This initializes the variable.\n";

    const std::string extracted = flowstate::ExtractDiffText(raw);
    Expect(extracted.find("This initializes the variable.") == std::string::npos,
           "diff extraction should stop before trailing prose");
    const flowstate::PatchSet patch = flowstate::ParseUnifiedDiff(extracted);
    Expect(patch.valid(), "extracted diff should remain parseable");
}

void TestGitDiffMarkerParsing() {
    const flowstate::GitLineStatus added =
        flowstate::ParseGitDiffMarkers("@@ -1,0 +2,2 @@\n"
                                       "+alpha\n"
                                       "+beta\n",
                                       4);
    Expect(added.available, "parsed git status should be available");
    Expect(added.lines[1].marker == flowstate::GitLineMarker::Added,
           "pure added lines should receive green added markers");
    Expect(added.lines[2].marker == flowstate::GitLineMarker::Added,
           "multi-line additions should mark each added line");

    const flowstate::GitLineStatus modified =
        flowstate::ParseGitDiffMarkers("@@ -2 +2 @@\n"
                                       "-old_value\n"
                                       "+new_value\n",
                                       4);
    Expect(modified.lines[1].marker == flowstate::GitLineMarker::Modified,
           "replacement lines should receive blue modified markers");
    Expect(modified.lines[1].previous_lines.size() == 1 && modified.lines[1].previous_lines[0] == "old_value",
           "modified markers should retain the old text for peek rendering");

    const flowstate::GitLineStatus deleted =
        flowstate::ParseGitDiffMarkers("@@ -2 +1,0 @@\n"
                                       "-removed\n",
                                       3);
    Expect(deleted.lines[0].marker == flowstate::GitLineMarker::Deleted,
           "deleted lines should place a red marker at the deletion anchor");
    Expect(deleted.lines[0].previous_lines.size() == 1 && deleted.lines[0].previous_lines[0] == "removed",
           "deleted markers should retain the removed text for peek rendering");

    const flowstate::GitLineStatus mixed =
        flowstate::ParseGitDiffMarkers("@@ -1,2 +1,3 @@\n"
                                       "-old_one\n"
                                       "-old_two\n"
                                       "+new_one\n"
                                       "+new_two\n"
                                       "+extra\n",
                                       3);
    Expect(mixed.lines[0].marker == flowstate::GitLineMarker::Modified,
           "the first replacement line in a run should be modified");
    Expect(mixed.lines[0].previous_lines.size() == 1 && mixed.lines[0].previous_lines[0] == "old_one",
           "the first modified line should retain its previous text");
    Expect(mixed.lines[1].marker == flowstate::GitLineMarker::Modified,
           "the second replacement line in a run should be modified");
    Expect(mixed.lines[1].previous_lines.size() == 1 && mixed.lines[1].previous_lines[0] == "old_two",
           "the second modified line should retain its previous text");
    Expect(mixed.lines[2].marker == flowstate::GitLineMarker::Added,
           "extra new lines in a replacement run should remain added");
    Expect(mixed.lines[2].previous_lines.empty(),
           "pure added lines should not expose previous text");
}

void TestBuildRunner() {
    const flowstate::BuildResult result = flowstate::RunBuildCommand("sh -c 'echo boom; exit 7'");
    Expect(result.ran, "build command should run");
    Expect(result.exit_code == 7, "build command should preserve exit code");
    Expect(result.output.find("boom") != std::string::npos, "build output should be captured");
}

bool HasSpan(const std::vector<flowstate::SyntaxSpan>& spans,
             size_t start,
             size_t end,
             flowstate::SyntaxTokenKind kind) {
    for (const flowstate::SyntaxSpan& span : spans) {
        if (span.start == start && span.end == end && span.kind == kind) {
            return true;
        }
    }
    return false;
}

bool ContainsColoredText(std::string_view rendered, std::string_view color_code, std::string_view text) {
    return rendered.find(std::string(color_code) + std::string(text)) != std::string_view::npos;
}

size_t CountOccurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }

    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void TestLanguageDetection() {
    flowstate::Buffer cpp_buffer;
    cpp_buffer.setPath("sample.cpp");
    Expect(cpp_buffer.languageId() == flowstate::LanguageId::Cpp, "cpp files should detect as C++");
    Expect(cpp_buffer.guessLanguage() == "C++", "cpp files should show the C++ label");

    flowstate::Buffer rust_buffer;
    rust_buffer.setPath("sample.rs");
    Expect(rust_buffer.languageId() == flowstate::LanguageId::Rust, "rust files should detect as Rust");
    Expect(rust_buffer.guessLanguage() == "Rust", "rust files should show the Rust label");

    flowstate::Buffer python_buffer;
    python_buffer.setPath("sample.py");
    Expect(python_buffer.languageId() == flowstate::LanguageId::Python, "python files should detect as Python");
    Expect(python_buffer.guessLanguage() == "Python", "python files should show the Python label");

    flowstate::Buffer javascript_buffer;
    javascript_buffer.setPath("sample.js");
    Expect(javascript_buffer.languageId() == flowstate::LanguageId::JavaScript,
           "javascript files should detect as JavaScript");
    Expect(javascript_buffer.guessLanguage() == "JavaScript",
           "javascript files should show the JavaScript label");

    flowstate::Buffer typescript_buffer;
    typescript_buffer.setPath("sample.ts");
    Expect(typescript_buffer.languageId() == flowstate::LanguageId::TypeScript,
           "typescript files should detect as TypeScript");
    Expect(typescript_buffer.guessLanguage() == "TypeScript",
           "typescript files should show the TypeScript label");

    flowstate::Buffer java_buffer;
    java_buffer.setPath("sample.java");
    Expect(java_buffer.languageId() == flowstate::LanguageId::Java, "java files should detect as Java");
    Expect(java_buffer.guessLanguage() == "Java", "java files should show the Java label");

    flowstate::Buffer go_buffer;
    go_buffer.setPath("sample.go");
    Expect(go_buffer.languageId() == flowstate::LanguageId::Go, "go files should detect as Go");
    Expect(go_buffer.guessLanguage() == "Go", "go files should show the Go label");

    flowstate::Buffer markdown_buffer;
    markdown_buffer.setPath("notes.md");
    Expect(markdown_buffer.languageId() == flowstate::LanguageId::Markdown,
           "markdown files should detect as Markdown");
    Expect(markdown_buffer.guessLanguage() == "Markdown",
           "markdown files should show the Markdown label");

    flowstate::Buffer header_buffer;
    header_buffer.setPath("sample.h");
    Expect(header_buffer.languageId() == flowstate::LanguageId::CHeader,
           "header files should keep the shared C/C++ language id");
    Expect(header_buffer.guessLanguage() == "C/C++", "header files should keep the C/C++ status label");

    flowstate::Buffer text_buffer;
    text_buffer.setPath("notes.custom");
    Expect(text_buffer.languageId() == flowstate::LanguageId::PlainText,
           "unknown extensions should fall back to plain text");

    const std::optional<std::string_view> cpp_prefix =
        flowstate::LineCommentPrefix(flowstate::LanguageId::Cpp);
    Expect(cpp_prefix.has_value() && *cpp_prefix == "//", "cpp files should use slash line comments");

    const std::optional<std::string_view> python_prefix =
        flowstate::LineCommentPrefix(flowstate::LanguageId::Python);
    Expect(python_prefix.has_value() && *python_prefix == "#", "python files should use hash line comments");

    Expect(!flowstate::LineCommentPrefix(flowstate::LanguageId::PlainText).has_value(),
           "plain text files should not expose a line comment prefix");
}

void TestCppHighlighterSpans() {
    flowstate::CppHighlighter highlighter;
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state = highlighter.HighlightLine("#include <iostream> // stream support", {}, &spans);
    Expect(state.value == 0, "single-line comments should not carry state");
    Expect(HasSpan(spans, 0, 8, flowstate::SyntaxTokenKind::Preprocessor),
           "include directive should be tokenized as preprocessor");
    Expect(HasSpan(spans, 9, 19, flowstate::SyntaxTokenKind::IncludePath),
           "include target should be tokenized separately");
    Expect(HasSpan(spans, 20, 37, flowstate::SyntaxTokenKind::Comment),
           "trailing line comment should be tokenized as comment");

    spans.clear();
    state = highlighter.HighlightLine("/* block comment", {}, &spans);
    Expect(state.value != 0, "unterminated block comments should carry state to the next line");
    Expect(HasSpan(spans, 0, 16, flowstate::SyntaxTokenKind::Comment),
           "block comment start should be tokenized as comment");

    spans.clear();
    state = highlighter.HighlightLine("continues here */", state, &spans);
    Expect(state.value == 0, "closed block comments should clear the carried state");
    Expect(HasSpan(spans, 0, 17, flowstate::SyntaxTokenKind::Comment),
           "continued block comments should stay tokenized as comment");

    spans.clear();
    state = highlighter.HighlightLine("#define MAX_COUNT 0x2A", {}, &spans);
    Expect(HasSpan(spans, 0, 7, flowstate::SyntaxTokenKind::Preprocessor),
           "preprocessor directives should stay tokenized as preprocessor");
    Expect(HasSpan(spans, 8, 17, flowstate::SyntaxTokenKind::Macro),
           "macro names should be tokenized separately");
    Expect(HasSpan(spans, 18, 22, flowstate::SyntaxTokenKind::Number),
           "numeric literals should be tokenized");

    spans.clear();
    state = highlighter.HighlightLine("constexpr auto value = ComputeValue(0x2A, \"hi\", 'x', true);", {}, &spans);
    Expect(HasSpan(spans, 0, 9, flowstate::SyntaxTokenKind::Keyword),
           "constexpr should be tokenized as a keyword");
    Expect(HasSpan(spans, 10, 14, flowstate::SyntaxTokenKind::Type),
           "auto should be tokenized as a type keyword");
    Expect(HasSpan(spans, 23, 35, flowstate::SyntaxTokenKind::Function),
           "function identifiers should be tokenized when followed by a call");
    Expect(HasSpan(spans, 36, 40, flowstate::SyntaxTokenKind::Number),
           "hex numeric literals should be tokenized");
    Expect(HasSpan(spans, 42, 46, flowstate::SyntaxTokenKind::String),
           "string literals should be tokenized");
    Expect(HasSpan(spans, 48, 51, flowstate::SyntaxTokenKind::String),
           "character literals should be tokenized");
    Expect(HasSpan(spans, 53, 57, flowstate::SyntaxTokenKind::Keyword),
           "boolean literals should be tokenized as keywords");

    spans.clear();
    state = highlighter.HighlightLine("class Widget final : public Base {", {}, &spans);
    Expect(HasSpan(spans, 0, 5, flowstate::SyntaxTokenKind::Keyword),
           "class should be tokenized as a keyword");
    Expect(HasSpan(spans, 6, 12, flowstate::SyntaxTokenKind::Type),
           "declared type names should be tokenized as types");
    Expect(HasSpan(spans, 13, 18, flowstate::SyntaxTokenKind::Keyword),
           "final should be tokenized as a keyword");
    Expect(HasSpan(spans, 21, 27, flowstate::SyntaxTokenKind::Keyword),
           "access specifiers should be tokenized as keywords");
}

void TestRustHighlighterSpans() {
    flowstate::RustHighlighter highlighter;
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state =
        highlighter.HighlightLine("pub fn render_value(input: i32) -> String { println!(\"{}\", 0x2A); } // comment",
                                  {},
                                  &spans);
    Expect(state.value == 0, "single-line rust constructs should not carry state");
    Expect(HasSpan(spans, 0, 3, flowstate::SyntaxTokenKind::Keyword),
           "pub should be tokenized as a keyword");
    Expect(HasSpan(spans, 4, 6, flowstate::SyntaxTokenKind::Keyword),
           "fn should be tokenized as a keyword");
    Expect(HasSpan(spans, 7, 19, flowstate::SyntaxTokenKind::Function),
           "declared rust function names should be tokenized as functions");
    Expect(HasSpan(spans, 27, 30, flowstate::SyntaxTokenKind::Type),
           "primitive rust types should be tokenized as types");
    Expect(HasSpan(spans, 35, 41, flowstate::SyntaxTokenKind::Type),
           "Rust type names should be tokenized as types");
    Expect(HasSpan(spans, 44, 51, flowstate::SyntaxTokenKind::Macro),
           "macro invocations should be tokenized as macros");
    Expect(HasSpan(spans, 53, 57, flowstate::SyntaxTokenKind::String),
           "string literals should be tokenized");
    Expect(HasSpan(spans, 59, 63, flowstate::SyntaxTokenKind::Number),
           "numeric literals should be tokenized");
    Expect(HasSpan(spans, 68, 78, flowstate::SyntaxTokenKind::Comment),
           "line comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("/* outer /* inner */ still", {}, &spans);
    Expect(state.value != 0, "nested rust block comments should carry state to the next line");
    Expect(HasSpan(spans, 0, 26, flowstate::SyntaxTokenKind::Comment),
           "block comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("comment */ let value = r#\"hi", state, &spans);
    Expect(state.value != 0, "unterminated raw rust strings should carry state");
    Expect(HasSpan(spans, 0, 10, flowstate::SyntaxTokenKind::Comment),
           "continued nested block comments should stay comments");
    Expect(HasSpan(spans, 23, 28, flowstate::SyntaxTokenKind::String),
           "raw rust strings should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("there\"#; let answer = 42usize;", state, &spans);
    Expect(state.value == 0, "closed raw rust strings should clear carried state");
    Expect(HasSpan(spans, 0, 7, flowstate::SyntaxTokenKind::String),
           "continued raw rust strings should stay tokenized as strings");
    Expect(HasSpan(spans, 22, 29, flowstate::SyntaxTokenKind::Number),
           "rust numeric suffixes should remain part of the number token");
}

void TestPythonHighlighterSpans() {
    flowstate::PythonHighlighter highlighter;
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state =
        highlighter.HighlightLine("@decorator\n"
                                  "async def render_value(value: int) -> str:\n",
                                  {},
                                  nullptr);
    Expect(state.value == 0, "python lexer should not carry state across complete lines");

    spans.clear();
    state = highlighter.HighlightLine("@decorator", {}, &spans);
    Expect(HasSpan(spans, 0, 10, flowstate::SyntaxTokenKind::Macro),
           "python decorators should be tokenized as macros");

    spans.clear();
    state = highlighter.HighlightLine(
        "async def render_value(value: int) -> str: return format_value(0x2A, \"hi\") # note", {}, &spans);
    Expect(HasSpan(spans, 0, 5, flowstate::SyntaxTokenKind::Keyword),
           "async should be tokenized as a keyword");
    Expect(HasSpan(spans, 6, 9, flowstate::SyntaxTokenKind::Keyword),
           "def should be tokenized as a keyword");
    Expect(HasSpan(spans, 10, 22, flowstate::SyntaxTokenKind::Function),
           "declared python function names should be tokenized as functions");
    Expect(HasSpan(spans, 30, 33, flowstate::SyntaxTokenKind::Type),
           "python builtin parameter types should be tokenized as types");
    Expect(HasSpan(spans, 38, 41, flowstate::SyntaxTokenKind::Type),
           "python return types should be tokenized as types");
    Expect(HasSpan(spans, 43, 49, flowstate::SyntaxTokenKind::Keyword),
           "return should be tokenized as a keyword");
    Expect(HasSpan(spans, 50, 62, flowstate::SyntaxTokenKind::Function),
           "python function calls should be tokenized as functions");
    Expect(HasSpan(spans, 63, 67, flowstate::SyntaxTokenKind::Number),
           "python numeric literals should be tokenized");
    Expect(HasSpan(spans, 69, 73, flowstate::SyntaxTokenKind::String),
           "python string literals should be tokenized");
    Expect(HasSpan(spans, 75, 81, flowstate::SyntaxTokenKind::Comment),
           "python comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("text = \"\"\"hello", {}, &spans);
    Expect(state.value != 0, "unterminated triple-quoted python strings should carry state");
    Expect(HasSpan(spans, 7, 15, flowstate::SyntaxTokenKind::String),
           "python triple-quoted string starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world\"\"\"", state, &spans);
    Expect(state.value == 0, "closed triple-quoted python strings should clear carried state");
    Expect(HasSpan(spans, 0, 8, flowstate::SyntaxTokenKind::String),
           "continued triple-quoted python strings should remain tokenized as strings");
}

void TestJavaScriptHighlighterSpans() {
    flowstate::JavaScriptHighlighter highlighter(flowstate::LanguageId::JavaScript);
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state = highlighter.HighlightLine(
        "export async function renderValue(input) { return formatValue(0x2A, \"hi\"); } // note", {}, &spans);
    Expect(state.value == 0, "single-line javascript constructs should not carry state");
    Expect(HasSpan(spans, 0, 6, flowstate::SyntaxTokenKind::Keyword),
           "export should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 7, 12, flowstate::SyntaxTokenKind::Keyword),
           "async should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 13, 21, flowstate::SyntaxTokenKind::Keyword),
           "function should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 22, 33, flowstate::SyntaxTokenKind::Function),
           "declared javascript function names should be tokenized as functions");
    Expect(HasSpan(spans, 43, 49, flowstate::SyntaxTokenKind::Keyword),
           "return should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 50, 61, flowstate::SyntaxTokenKind::Function),
           "javascript function calls should be tokenized as functions");
    Expect(HasSpan(spans, 62, 66, flowstate::SyntaxTokenKind::Number),
           "javascript numeric literals should be tokenized");
    Expect(HasSpan(spans, 68, 72, flowstate::SyntaxTokenKind::String),
           "javascript string literals should be tokenized");
    Expect(HasSpan(spans, 77, 84, flowstate::SyntaxTokenKind::Comment),
           "javascript comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("const message = `hello", {}, &spans);
    Expect(state.value != 0, "unterminated javascript template strings should carry state");
    Expect(HasSpan(spans, 16, 22, flowstate::SyntaxTokenKind::String),
           "javascript template string starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world`;", state, &spans);
    Expect(state.value == 0, "closed javascript template strings should clear carried state");
    Expect(HasSpan(spans, 0, 6, flowstate::SyntaxTokenKind::String),
           "continued javascript template strings should remain tokenized as strings");
}

void TestTypeScriptHighlighterSpans() {
    flowstate::JavaScriptHighlighter highlighter(flowstate::LanguageId::TypeScript);
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state = highlighter.HighlightLine("interface WidgetProps { title: string }", {}, &spans);
    Expect(state.value == 0, "single-line typescript constructs should not carry state");
    Expect(HasSpan(spans, 0, 9, flowstate::SyntaxTokenKind::Keyword),
           "interface should be tokenized as a typescript keyword");
    Expect(HasSpan(spans, 10, 21, flowstate::SyntaxTokenKind::Type),
           "declared typescript interface names should be tokenized as types");
    Expect(HasSpan(spans, 31, 37, flowstate::SyntaxTokenKind::Type),
           "typescript annotation types should be tokenized as types");

    spans.clear();
    state = highlighter.HighlightLine(
        "function renderValue(value: number): Promise<string> { return formatValue(value as number); }",
        {},
        &spans);
    Expect(HasSpan(spans, 0, 8, flowstate::SyntaxTokenKind::Keyword),
           "function should stay tokenized as a typescript keyword");
    Expect(HasSpan(spans, 9, 20, flowstate::SyntaxTokenKind::Function),
           "declared typescript function names should be tokenized as functions");
    Expect(HasSpan(spans, 28, 34, flowstate::SyntaxTokenKind::Type),
           "typescript parameter annotation types should be tokenized");
    Expect(HasSpan(spans, 37, 44, flowstate::SyntaxTokenKind::Type),
           "typescript generic container types should be tokenized");
    Expect(HasSpan(spans, 45, 51, flowstate::SyntaxTokenKind::Type),
           "typescript generic parameter types should be tokenized");
    Expect(HasSpan(spans, 55, 61, flowstate::SyntaxTokenKind::Keyword),
           "return should be tokenized as a typescript keyword");
    Expect(HasSpan(spans, 62, 73, flowstate::SyntaxTokenKind::Function),
           "typescript function calls should be tokenized as functions");
    Expect(HasSpan(spans, 80, 82, flowstate::SyntaxTokenKind::Keyword),
           "typescript as-casts should keep the as keyword highlighted");
    Expect(HasSpan(spans, 83, 89, flowstate::SyntaxTokenKind::Type),
           "typescript cast target types should be tokenized");
}

void TestJavaHighlighterSpans() {
    flowstate::JavaHighlighter highlighter;
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state = highlighter.HighlightLine("@Override", {}, &spans);
    Expect(state.value == 0, "single-line java annotations should not carry state");
    Expect(HasSpan(spans, 0, 9, flowstate::SyntaxTokenKind::Macro),
           "java annotations should be tokenized as macros");

    spans.clear();
    state = highlighter.HighlightLine(
        "public String renderValue(int count) { return formatValue(0x2A, \"hi\"); } // note", {}, &spans);
    Expect(HasSpan(spans, 0, 6, flowstate::SyntaxTokenKind::Keyword),
           "public should be tokenized as a java keyword");
    Expect(HasSpan(spans, 7, 13, flowstate::SyntaxTokenKind::Type),
           "java return types should be tokenized as types");
    Expect(HasSpan(spans, 14, 25, flowstate::SyntaxTokenKind::Function),
           "declared java method names should be tokenized as functions");
    Expect(HasSpan(spans, 26, 29, flowstate::SyntaxTokenKind::Type),
           "java primitive parameter types should be tokenized");
    Expect(HasSpan(spans, 39, 45, flowstate::SyntaxTokenKind::Keyword),
           "return should be tokenized as a java keyword");
    Expect(HasSpan(spans, 46, 57, flowstate::SyntaxTokenKind::Function),
           "java method calls should be tokenized as functions");
    Expect(HasSpan(spans, 58, 62, flowstate::SyntaxTokenKind::Number),
           "java numeric literals should be tokenized");
    Expect(HasSpan(spans, 64, 68, flowstate::SyntaxTokenKind::String),
           "java string literals should be tokenized");
    Expect(HasSpan(spans, 73, 80, flowstate::SyntaxTokenKind::Comment),
           "java comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("public class Widget extends Base {", {}, &spans);
    Expect(HasSpan(spans, 7, 12, flowstate::SyntaxTokenKind::Keyword),
           "class should be tokenized as a java keyword");
    Expect(HasSpan(spans, 13, 19, flowstate::SyntaxTokenKind::Type),
           "declared java class names should be tokenized as types");
    Expect(HasSpan(spans, 20, 27, flowstate::SyntaxTokenKind::Keyword),
           "extends should be tokenized as a java keyword");
    Expect(HasSpan(spans, 28, 32, flowstate::SyntaxTokenKind::Type),
           "extended java types should be tokenized as types");

    spans.clear();
    state = highlighter.HighlightLine("String text = \"\"\"hello", {}, &spans);
    Expect(state.value != 0, "unterminated java text blocks should carry state");
    Expect(HasSpan(spans, 14, 22, flowstate::SyntaxTokenKind::String),
           "java text block starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world\"\"\";", state, &spans);
    Expect(state.value == 0, "closed java text blocks should clear carried state");
    Expect(HasSpan(spans, 0, 8, flowstate::SyntaxTokenKind::String),
           "continued java text blocks should remain tokenized as strings");
}

void TestGoHighlighterSpans() {
    flowstate::GoHighlighter highlighter;
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state = highlighter.HighlightLine(
        "func renderValue(count int) string { return formatValue(0x2A, \"hi\") } // note", {}, &spans);
    Expect(state.value == 0, "single-line go constructs should not carry state");
    Expect(HasSpan(spans, 0, 4, flowstate::SyntaxTokenKind::Keyword),
           "func should be tokenized as a go keyword");
    Expect(HasSpan(spans, 5, 16, flowstate::SyntaxTokenKind::Function),
           "declared go function names should be tokenized as functions");
    Expect(HasSpan(spans, 23, 26, flowstate::SyntaxTokenKind::Type),
           "go parameter types should be tokenized as types");
    Expect(HasSpan(spans, 28, 34, flowstate::SyntaxTokenKind::Type),
           "go return types should be tokenized as types");
    Expect(HasSpan(spans, 37, 43, flowstate::SyntaxTokenKind::Keyword),
           "return should be tokenized as a go keyword");
    Expect(HasSpan(spans, 44, 55, flowstate::SyntaxTokenKind::Function),
           "go function calls should be tokenized as functions");
    Expect(HasSpan(spans, 56, 60, flowstate::SyntaxTokenKind::Number),
           "go numeric literals should be tokenized");
    Expect(HasSpan(spans, 62, 66, flowstate::SyntaxTokenKind::String),
           "go string literals should be tokenized");
    Expect(HasSpan(spans, 70, 77, flowstate::SyntaxTokenKind::Comment),
           "go comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("type Widget struct {}", {}, &spans);
    Expect(HasSpan(spans, 0, 4, flowstate::SyntaxTokenKind::Keyword),
           "type should be tokenized as a go keyword");
    Expect(HasSpan(spans, 5, 11, flowstate::SyntaxTokenKind::Type),
           "declared go type names should be tokenized as types");
    Expect(HasSpan(spans, 12, 18, flowstate::SyntaxTokenKind::Keyword),
           "struct should be tokenized as a go keyword");

    spans.clear();
    state = highlighter.HighlightLine("message := `hello", {}, &spans);
    Expect(state.value != 0, "unterminated go raw strings should carry state");
    Expect(HasSpan(spans, 11, 17, flowstate::SyntaxTokenKind::String),
           "go raw string starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world`", state, &spans);
    Expect(state.value == 0, "closed go raw strings should clear carried state");
    Expect(HasSpan(spans, 0, 6, flowstate::SyntaxTokenKind::String),
           "continued go raw strings should remain tokenized as strings");
}

void TestMarkdownHighlighterSpans() {
    flowstate::MarkdownHighlighter highlighter;
    std::vector<flowstate::SyntaxSpan> spans;

    flowstate::SyntaxLineState state = highlighter.HighlightLine("# Heading", {}, &spans);
    Expect(state.value == 0, "single-line markdown headings should not carry state");
    Expect(HasSpan(spans, 0, 9, flowstate::SyntaxTokenKind::Heading),
           "markdown headings should be tokenized as headings");

    spans.clear();
    state = highlighter.HighlightLine("> quoted", {}, &spans);
    Expect(HasSpan(spans, 0, 8, flowstate::SyntaxTokenKind::Quote),
           "markdown blockquotes should be tokenized as quotes");

    spans.clear();
    state = highlighter.HighlightLine("- [x] item", {}, &spans);
    Expect(HasSpan(spans, 0, 6, flowstate::SyntaxTokenKind::ListMarker),
           "markdown task list prefixes should be tokenized as list markers");

    spans.clear();
    state = highlighter.HighlightLine("Use `code` [docs](url) *style*", {}, &spans);
    Expect(HasSpan(spans, 4, 10, flowstate::SyntaxTokenKind::InlineCode),
           "markdown inline code should be tokenized");
    Expect(HasSpan(spans, 11, 17, flowstate::SyntaxTokenKind::LinkText),
           "markdown link text should be tokenized");
    Expect(HasSpan(spans, 17, 22, flowstate::SyntaxTokenKind::LinkUrl),
           "markdown link urls should be tokenized");
    Expect(HasSpan(spans, 23, 30, flowstate::SyntaxTokenKind::Emphasis),
           "markdown emphasis should be tokenized");

    spans.clear();
    state = highlighter.HighlightLine("```cpp", {}, &spans);
    Expect(state.value != 0, "opening markdown code fences should carry state");
    Expect(HasSpan(spans, 0, 6, flowstate::SyntaxTokenKind::CodeFence),
           "markdown code fence lines should be tokenized as code fences");

    spans.clear();
    state = highlighter.HighlightLine("int main() {", state, &spans);
    Expect(HasSpan(spans, 0, 3, flowstate::SyntaxTokenKind::Type),
           "fenced markdown code should delegate to the nested language highlighter for types");
    Expect(HasSpan(spans, 4, 8, flowstate::SyntaxTokenKind::Function),
           "fenced markdown code should delegate to the nested language highlighter for functions");

    spans.clear();
    state = highlighter.HighlightLine("```", state, &spans);
    Expect(state.value == 0, "closing markdown code fences should clear carried state");
    Expect(HasSpan(spans, 0, 3, flowstate::SyntaxTokenKind::CodeFence),
           "closing markdown code fences should stay tokenized as code fences");
}

void TestIncludeHighlightRendering() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("#include <iostream> // stream support\n"
                   "/* block comment\n"
                   "continues here */\n"
                   "int main() {}",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 80);

    Expect(rendered.find("1\xE2\x94\x82 ") != std::string::npos, "file view should render line numbers");
    Expect(rendered.find("2\xE2\x94\x82 ") != std::string::npos,
           "multi-line buffers should render later line numbers");
    Expect(rendered.find("\x1b[38;5;141m#include\x1b[39m") != std::string::npos,
           "include directive should be purple");
    Expect(rendered.find("\x1b[38;5;214m<iostream>\x1b[39m") != std::string::npos,
           "include target should be orange");
    Expect(rendered.find("\x1b[38;5;30m// stream support\x1b[39m") != std::string::npos,
           "line comments should be dark teal");
    Expect(rendered.find("\x1b[38;5;30m/* block comment\x1b[39m") != std::string::npos,
           "block comment start should be dark teal");
    Expect(rendered.find("\x1b[38;5;30mcontinues here */\x1b[39m") != std::string::npos,
           "continued block comments should stay dark teal");
}

void TestCppRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("#define MAX_COUNT 0x2A\n"
                   "constexpr auto value = ComputeValue(0x2A, \"hi\", 'x', true);\n"
                   "class Widget final : public Base {}",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;220mMAX_COUNT\x1b[39m") != std::string::npos,
           "macro names should render with the macro color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;179m", "0x2A"),
           "numeric literals should render with the number color");
    Expect(rendered.find("\x1b[38;5;75mconstexpr\x1b[39m") != std::string::npos,
           "keywords should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "auto"),
           "type keywords should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "ComputeValue"),
           "function identifiers should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "string literals should render with the string color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "'x'"),
           "character literals should render with the string color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "Widget"),
           "declared type names should render with the type color");
}

void TestBraceNestingRendering() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n"
                   "    if (true) {\n"
                   "        while (false) {\n"
                   "        }\n"
                   "    }\n"
                   "}\n"
                   "const char* text = \"{}\"; // {}\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    state.fileCursor() = {0, 11};
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 12, 120);

    Expect(rendered.find("\x1b[38;5;75m{\x1b[39m") != std::string::npos,
           "second-level braces should render blue");
    Expect(rendered.find("\x1b[38;5;78m{\x1b[39m") != std::string::npos,
           "third-level braces should render green");
    Expect(rendered.find("\x1b[38;5;211m\x1b[1m{\x1b[22m\x1b[39m") != std::string::npos,
           "the brace under the cursor should render bold");
    Expect(rendered.find("\x1b[38;5;211m\x1b[1m}\x1b[22m\x1b[39m") != std::string::npos,
           "the matching closing brace should stay colored and render bold");
    Expect(CountOccurrences(rendered, "\x1b[1m") == 2,
           "only the active brace pair should render bold");
    Expect(rendered.find("\x1b[38;5;221m\"{}\"\x1b[39m") != std::string::npos,
           "braces inside strings should keep the string color");
    Expect(rendered.find("\x1b[38;5;30m// {}\x1b[39m") != std::string::npos,
           "braces inside comments should keep the comment color");
}

void TestSquareAndRoundDelimiterHighlighting() {
    const std::string line = "int value = items[compute(0)];";

    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText(line + "\n"
                   "const char* text = \"[]()\"; // []()\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;

    state.fileCursor() = {0, line.find('[')};
    const std::string square_rendered = screen.Render(state, {}, 8, 120);
    Expect(square_rendered.find("\x1b[38;5;211m\x1b[1m[") != std::string::npos,
           "active square brackets should render bold in their nesting color");
    Expect(square_rendered.find("\x1b[38;5;211m\x1b[1m]") != std::string::npos,
           "matching square brackets should render bold in their nesting color");
    Expect(square_rendered.find("\x1b[38;5;75m(") != std::string::npos,
           "nested round braces should render in the next nesting color");
    Expect(square_rendered.find("\x1b[38;5;75m)") != std::string::npos,
           "nested closing round braces should keep their nesting color");
    Expect(CountOccurrences(square_rendered, "\x1b[1m") == 2,
           "only the active square-bracket pair should render bold");

    state.fileCursor() = {0, line.find('(')};
    const std::string round_rendered = screen.Render(state, {}, 8, 120);
    Expect(round_rendered.find("\x1b[38;5;75m\x1b[1m(") != std::string::npos,
           "active round braces should render bold in their nesting color");
    Expect(round_rendered.find("\x1b[38;5;75m\x1b[1m)") != std::string::npos,
           "matching round braces should render bold in their nesting color");
    Expect(CountOccurrences(round_rendered, "\x1b[1m") == 2,
           "only the active round-brace pair should render bold");
    Expect(round_rendered.find("\x1b[38;5;221m\"[]()\"\x1b[39m") != std::string::npos,
           "square and round braces inside strings should keep the string color");
    Expect(round_rendered.find("\x1b[38;5;30m// []()\x1b[39m") != std::string::npos,
           "square and round braces inside comments should keep the comment color");
}

void TestRustRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.rs");
    buffer.setText("pub fn render_value(input: i32) -> String {\n"
                   "    println!(\"{}\", 0x2A);\n"
                   "}\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75mpub\x1b[39m") != std::string::npos,
           "rust keywords should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "render_value"),
           "rust function declarations should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "i32"),
           "rust primitive types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "String"),
           "rust type names should render with the type color");
    Expect(rendered.find("\x1b[38;5;220mprintln\x1b[39m") != std::string::npos,
           "rust macros should render with the macro color");
    Expect(rendered.find("\x1b[38;5;221m\"{}\"\x1b[39m") != std::string::npos,
           "rust strings should render with the string color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;179m", "0x2A"),
           "rust numbers should render with the number color");
}

void TestPythonRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.py");
    buffer.setText("@decorator\n"
                   "async def render_value(value: int) -> str:\n"
                   "    return format_value(0x2A, \"hi\")  # note\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;220m@decorator\x1b[39m") != std::string::npos,
           "python decorators should render with the macro color");
    Expect(rendered.find("\x1b[38;5;75masync\x1b[39m") != std::string::npos,
           "python keywords should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "render_value"),
           "python function declarations should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "int"),
           "python builtin types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "format_value"),
           "python function calls should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;179m", "0x2A"),
           "python numeric literals should render with the number color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "python strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m# note\x1b[39m") != std::string::npos,
           "python comments should render with the comment color");
}

void TestJavaScriptRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.js");
    buffer.setText("export async function renderValue(input) {\n"
                   "    return formatValue(0x2A, \"hi\"); // note\n"
                   "}\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75mexport\x1b[39m") != std::string::npos,
           "javascript keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;75masync\x1b[39m") != std::string::npos,
           "javascript async should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "renderValue"),
           "javascript function declarations should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "formatValue"),
           "javascript function calls should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;179m", "0x2A"),
           "javascript numbers should render with the number color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "javascript strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m// note\x1b[39m") != std::string::npos,
           "javascript comments should render with the comment color");
}

void TestTypeScriptRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.ts");
    buffer.setText("interface WidgetProps { title: string }\n"
                   "function renderValue(value: number): Promise<string> {\n"
                   "    return formatValue(value as number);\n"
                   "}\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75minterface\x1b[39m") != std::string::npos,
           "typescript keywords should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "WidgetProps"),
           "typescript declared interface names should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "string"),
           "typescript annotation types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "renderValue"),
           "typescript function declarations should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "number"),
           "typescript numeric annotation types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "Promise"),
           "typescript generic container types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "formatValue"),
           "typescript function calls should render with the function color");
    Expect(rendered.find("\x1b[38;5;75mas\x1b[39m") != std::string::npos,
           "typescript casts should keep the as keyword highlighted");
}

void TestJavaRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.java");
    buffer.setText("@Override\n"
                   "public class Widget extends Base {\n"
                   "    public String renderValue(int count) {\n"
                   "        return formatValue(0x2A, \"hi\"); // note\n"
                   "    }\n"
                   "}\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;220m@Override\x1b[39m") != std::string::npos,
           "java annotations should render with the macro color");
    Expect(rendered.find("\x1b[38;5;75mpublic\x1b[39m") != std::string::npos,
           "java keywords should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "Widget"),
           "java declared class names should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "Base"),
           "java extended type names should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "String"),
           "java builtin reference types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "renderValue"),
           "java method declarations should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "int"),
           "java primitive types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "formatValue"),
           "java method calls should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;179m", "0x2A"),
           "java numbers should render with the number color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "java strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m// note\x1b[39m") != std::string::npos,
           "java comments should render with the comment color");
}

void TestGoRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.go");
    buffer.setText("package sample\n"
                   "type Widget struct {}\n"
                   "func renderValue(count int) string {\n"
                   "    return formatValue(0x2A, \"hi\") // note\n"
                   "}\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75mpackage\x1b[39m") != std::string::npos,
           "go keywords should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "Widget"),
           "go declared type names should render with the type color");
    Expect(rendered.find("\x1b[38;5;75mfunc\x1b[39m") != std::string::npos,
           "go func should render with the keyword color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "renderValue"),
           "go function declarations should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "int"),
           "go primitive types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "string"),
           "go return types should render with the type color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "formatValue"),
           "go function calls should render with the function color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;179m", "0x2A"),
           "go numbers should render with the number color");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "go strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m// note\x1b[39m") != std::string::npos,
           "go comments should render with the comment color");
}

void TestMarkdownRenderHighlightsExpandedTokenSet() {
    flowstate::Buffer buffer;
    buffer.setPath("notes.md");
    buffer.setText("# Heading\n"
                   "> quote\n"
                   "- [x] item\n"
                   "Use `code` and [docs](https://example.com) with *style*\n"
                   "```python\n"
                   "def render_value():\n"
                   "    return \"hi\"\n"
                   "```\n",
                   false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 12, 120);

    Expect(rendered.find("\x1b[38;5;111m# Heading\x1b[39m") != std::string::npos,
           "markdown headings should render with the heading color");
    Expect(rendered.find("\x1b[38;5;66m> quote\x1b[39m") != std::string::npos,
           "markdown blockquotes should render with the quote color");
    Expect(rendered.find("\x1b[38;5;215m- [x] \x1b[39m") != std::string::npos,
           "markdown list markers should render with the list marker color");
    Expect(rendered.find("\x1b[38;5;220m`code`\x1b[39m") != std::string::npos,
           "markdown inline code should render with the inline code color");
    Expect(rendered.find("\x1b[38;5;117m[docs]\x1b[38;5;214m(") != std::string::npos,
           "markdown link text should render with the link text color");
    Expect(rendered.find("\x1b[38;5;214m(https://example.com)\x1b[39m") != std::string::npos,
           "markdown link urls should render with the link url color");
    Expect(rendered.find("\x1b[38;5;211m*style*\x1b[39m") != std::string::npos,
           "markdown emphasis should render with the emphasis color");
    Expect(rendered.find("\x1b[38;5;141m```python\x1b[39m") != std::string::npos,
           "markdown code fences should render with the code fence color");
    Expect(rendered.find("\x1b[38;5;75mdef\x1b[39m") != std::string::npos,
           "markdown fenced code should render nested language keywords");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "render_value"),
           "markdown fenced code should render nested language functions");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "markdown fenced code should render nested language strings");
}

void TestLineNumberGutterAffectsVisibleWidth() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("abcdefghijklmnopqrstuvwxyz", false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 10);

    Expect(rendered.find("1\xE2\x94\x82 abcdefg") != std::string::npos,
           "line number gutter should reduce visible content width");
    Expect(screen.ContentColumns(state, 10) == 7, "content width should subtract the line number gutter");
}

void TestSelectionUsesStableBackgroundHighlight() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("abcdef", false);

    flowstate::EditorState state(std::move(buffer));
    state.selection() = {
        .active = true,
        .anchor = {.row = 0, .col = 1},
        .head = {.row = 0, .col = 4},
    };

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 80);

    Expect(rendered.find("\x1b[48;5;238mbcd") != std::string::npos,
           "selection should render with a stable background color");
    Expect(rendered.find("\x1b[7mbcd") == std::string::npos,
           "selection should not use reverse video because it conflicts with cursor blinking");
}

void TestCompletionPopupDoesNotMoveStatusBar() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    valu\n}", false);

    flowstate::EditorState state(std::move(buffer));
    state.fileCursor() = {.row = 1, .col = 8};
    state.setCompletionSession({
        .active = true,
        .replace_start = {.row = 1, .col = 4},
        .replace_end = {.row = 1, .col = 8},
        .items = {flowstate::CompletionItem{.label = "value"},
                  flowstate::CompletionItem{.label = "value_or"}},
    });

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 80);

    Expect(rendered.find("\x1b[7;1H\x1b[7m") != std::string::npos,
           "status bar should be positioned at the bottom even after drawing the completion popup");
    Expect(rendered.find("\x1b[8;1H") != std::string::npos,
           "message row should be positioned at the bottom even after drawing the completion popup");
}

void TestAiScratchDoesNotRenderLineNumbers() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setAiText("Explaining selection via codex");
    state.setActiveView(flowstate::ViewKind::AiScratch);

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 20);

    Expect(rendered.find("1\xE2\x94\x82 ") == std::string::npos,
           "AI scratch should not render file line numbers");
    Expect(rendered.find("Explaining selection") != std::string::npos,
           "AI scratch should use the full line width for response text");
    Expect(screen.ContentColumns(state, 20) == 20, "AI scratch width should not reserve gutter space");
}

void TestAiScratchBoldsUserMessages() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setAiText("AI: Initial explanation\n\nYou: Can you clarify?\n\nAI: Yes");
    state.setActiveView(flowstate::ViewKind::AiScratch);

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 80);

    Expect(rendered.find("\x1b[1mYou: Can you clarify?") != std::string::npos,
           "AI scratch should bold user-sent messages");
    Expect(rendered.find("\x1b[1mAI: Initial explanation") == std::string::npos,
           "AI scratch should not bold AI response lines");
}

void TestInlineAiExplainRendersInFileView() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    return 0;\n}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 0,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = "This explains the selected code without leaving the file view.",
    });

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 10, 80);

    Expect(state.activeView() == flowstate::ViewKind::File,
           "inline AI explain should keep the file view active");
    Expect(rendered.find("1\xE2\x94\x82 ") != std::string::npos &&
               rendered.find("main") != std::string::npos,
           "inline AI explain should render inside the file view");
    Expect(rendered.find("\x1b[1mAI Explain\x1b[22m") != std::string::npos,
           "inline AI explain title should be bold");
    Expect(rendered.find("┌─ ") != std::string::npos && rendered.find("┐") != std::string::npos,
           "inline AI explain should render a clear top border");
    Expect(rendered.find("This explains the selected code") != std::string::npos,
           "inline AI explain should render the response text inline");
    Expect(rendered.find("├") != std::string::npos && rendered.find("> ") != std::string::npos,
           "inline AI explain should render a separated follow-up input area");
    Expect(rendered.find("Enter sends follow-up.") == std::string::npos,
           "inline AI explain should keep the input row empty except for the prompt");
    Expect(screen.InlineAiRowCount(state, screen.ContentColumns(state, 80)) >= 5,
           "inline AI explain should contribute virtual rows to the file view");
}

void TestInlineAiExplainBoldsUserMessages() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    return 0;\n}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 0,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = "AI: This is the original explanation.\n\nYou: What does this mean?\n\nAI: It means the "
                "function exits successfully.",
    });

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 12, 80);

    Expect(rendered.find("\x1b[1mYou: What does this mean?") != std::string::npos,
           "inline AI explain should bold user-sent messages");
    Expect(rendered.find("\x1b[1mAI: This is the original explanation.") == std::string::npos,
           "inline AI explain should not bold AI response lines");
}

void TestInlineAiExplainHighlightsFencedCode() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    return 0;\n}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 0,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = "Use this:\n```cpp\nint main() {\n    return 0;\n}\n```",
    });

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 12, 80);

    Expect(rendered.find("```cpp") == std::string::npos && rendered.find("```") == std::string::npos,
           "inline AI explain should consume markdown code fence markers");
    Expect(rendered.find(std::string(flowstate::ColorCodeForToken(flowstate::SyntaxTokenKind::Type)) + "int") !=
               std::string::npos,
           "inline AI explain should syntax-highlight fenced code using the fence language");
}

void TestInlineAiExplainPanelIsScrollable() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    return 0;\n}", false);

    std::string long_explanation;
    for (int index = 0; index < 20; ++index) {
        if (!long_explanation.empty()) {
            long_explanation.push_back('\n');
        }
        long_explanation += "line ";
        if (index < 10) {
            long_explanation.push_back('0');
        }
        long_explanation += std::to_string(index);
    }

    flowstate::EditorState state(std::move(buffer));
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 0,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = long_explanation,
    });

    flowstate::Screen screen;
    const size_t content_cols = screen.ContentColumns(state, 80);
    Expect(screen.InlineAiBodyRowCount(state, content_cols) == 20,
           "inline AI explain should keep the full wrapped body available");
    Expect(screen.InlineAiVisibleBodyRowCount(state, content_cols) == 12,
           "inline AI explain should cap the visible body rows");
    Expect(screen.InlineAiRowCount(state, content_cols) == 16,
           "inline AI explain virtual row count should include the capped body, input, and borders");

    const std::string top_rendered = screen.Render(state, {}, 18, 80);
    Expect(top_rendered.find("line 00") != std::string::npos,
           "inline AI explain should initially render the top of the response");
    Expect(top_rendered.find("line 12") == std::string::npos,
           "inline AI explain should not render body rows beyond the visible cap");

    state.inlineAiSession()->scroll_row = 5;
    const std::string scrolled_rendered = screen.Render(state, {}, 18, 80);
    Expect(scrolled_rendered.find("line 00") == std::string::npos,
           "scrolling the inline AI explain should hide earlier body rows");
    Expect(scrolled_rendered.find("line 05") != std::string::npos,
           "scrolling the inline AI explain should render from the scroll offset");
    Expect(scrolled_rendered.find("line 16") != std::string::npos,
           "scrolling the inline AI explain should render the capped window after the offset");

    state.inlineAiSession()->focused = true;
    state.inlineAiSession()->cursor_body_row = 6;
    const std::string focused_rendered = screen.Render(state, {}, 18, 80);
    Expect(focused_rendered.find("\x1b[7mline 06") == std::string::npos,
           "focused inline AI rows should rely on the normal terminal cursor instead of inverse blinking");
}

void TestInlineAiExplainDownAtBottomReturnsToFileWithoutClosing() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("line 0\nline 1\nline 2\nline 3", false);

    auto ai_client = std::make_unique<flowstate::NoAiClient>();
    flowstate::EditorApp app(std::move(buffer), std::move(ai_client), "", "NO AI", "c++17");
    flowstate::EditorState& state = flowstate::EditorAppTestPeer::State(app);

    std::string long_explanation;
    for (int index = 0; index < 20; ++index) {
        if (!long_explanation.empty()) {
            long_explanation.push_back('\n');
        }
        long_explanation += "line " + std::to_string(index);
    }

    state.fileCursor().row = 1;
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 1,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = long_explanation,
    });
    state.inlineAiSession()->focused = true;

    flowstate::Screen screen;
    const size_t content_cols = screen.ContentColumns(state, 80);
    const size_t body_rows = screen.InlineAiBodyRowCount(state, content_cols);
    const size_t visible_rows = screen.InlineAiVisibleBodyRowCount(state, content_cols);
    state.inlineAiSession()->scroll_row = body_rows - visible_rows;
    state.inlineAiSession()->cursor_body_row = body_rows - 1;

    const bool handled = flowstate::EditorAppTestPeer::HandleInlineAiKey(
        app, flowstate::KeyPress{.type = flowstate::KeyType::ArrowDown});

    Expect(handled, "down at the bottom of inline AI should be handled");
    Expect(state.inlineAiSession().has_value(),
           "down at the bottom of inline AI should keep the panel open");
    Expect(!state.inlineAiSession()->focused,
           "down at the bottom of inline AI should return focus to the file");
    Expect(state.fileCursor().row == 2,
           "down at the bottom of inline AI should move to the next file row after the panel");
}

void TestInlineAiExplainUpAtTopReturnsToFileWithoutClosing() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("line 0\nline 1\nline 2\nline 3", false);

    auto ai_client = std::make_unique<flowstate::NoAiClient>();
    flowstate::EditorApp app(std::move(buffer), std::move(ai_client), "", "NO AI", "c++17");
    flowstate::EditorState& state = flowstate::EditorAppTestPeer::State(app);

    state.fileCursor().row = 1;
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 1,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = "line 0\nline 1\nline 2",
    });
    state.inlineAiSession()->focused = true;
    state.inlineAiSession()->scroll_row = 0;
    state.inlineAiSession()->cursor_body_row = 0;

    const bool handled = flowstate::EditorAppTestPeer::HandleInlineAiKey(
        app, flowstate::KeyPress{.type = flowstate::KeyType::ArrowUp});

    Expect(handled, "up at the top of inline AI should be handled");
    Expect(state.inlineAiSession().has_value(),
           "up at the top of inline AI should keep the panel open");
    Expect(!state.inlineAiSession()->focused,
           "up at the top of inline AI should return focus to the file");
    Expect(state.fileCursor().row == 1,
           "up at the top of inline AI should move to the file row before the panel");
}

flowstate::RateLimitSnapshotInfo TestRateLimits(double five_hour_percent, double weekly_percent) {
    flowstate::RateLimitSnapshotInfo rate_limits;
    rate_limits.available = true;
    rate_limits.limit_id = "codex";
    rate_limits.primary.available = true;
    rate_limits.primary.used_percent = five_hour_percent;
    rate_limits.primary.window_duration_mins = 300;
    rate_limits.secondary.available = true;
    rate_limits.secondary.used_percent = weekly_percent;
    rate_limits.secondary.window_duration_mins = 10080;
    return rate_limits;
}

void TestInlineAiExplainFooterShowsCodexUsageBars() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    return 0;\n}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setInlineAiSession(flowstate::InlineAiSession{
        .anchor_row = 0,
        .title = "AI Explain",
        .provider_name = "CODEX",
        .state_label = "COMPLETE",
        .text = "This explains the selected code.",
    });
    state.setAiRateLimits(TestRateLimits(25.0, 60.0));

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 10, 100);

    Expect(rendered.find("5h [##------] 25%") != std::string::npos,
           "inline AI footer should show the 5h Codex usage bar");
    Expect(rendered.find("wk [#####---] 60%") != std::string::npos,
           "inline AI footer should show the weekly Codex usage bar");
    Expect(rendered.find("wk [#####---] 60%─┘") != std::string::npos,
           "inline AI footer should reconnect the bottom-right corner after the usage entry");
}

void TestAiScratchDiffHunksUseFileSyntaxHighlighting() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {}", false);

    flowstate::EditorState state(std::move(buffer));
    state.setAiText("Here is the patch:\n\n"
                    "@@ -1,1 +1,1 @@\n"
                    "-int total = 0;\n"
                    "+int total = ComputeValue(0x2A, \"hi\");\n");
    state.setActiveView(flowstate::ViewKind::AiScratch);

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[36m@@ -1,1 +1,1 @@\x1b[39m") != std::string::npos,
           "AI diff hunk headers should still be decorated");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;81m", "int"),
           "AI diff code should use the file syntax policy for types");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "ComputeValue"),
           "AI diff code should highlight function identifiers");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "AI diff code should highlight string literals");
}

void TestPatchPreviewAddedLinesUseFileSyntaxHighlighting() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int total = 0;\n", false);

    const std::string diff_text =
        "@@ -1,1 +1,1 @@\n"
        "-int total = 0;\n"
        "+int total = ComputeValue(0x2A, \"hi\");\n";

    const flowstate::PatchSet patch = flowstate::ParseUnifiedDiff(diff_text);
    flowstate::EditorState state(std::move(buffer));
    state.setPatchSession(flowstate::CreatePatchSession(patch, state.fileBuffer()));
    state.setActiveView(flowstate::ViewKind::PatchPreview);

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[36m@@ -1,1 +1,1 @@ [PENDING]\x1b[39m") != std::string::npos,
           "patch preview hunk headers should stay decorated");
    Expect(rendered.find("\x1b[31m-int total = 0;\x1b[39m") != std::string::npos,
           "removed lines in patch preview may stay plain red");
    Expect(rendered.find("\x1b[32m+\x1b[39m") != std::string::npos &&
               ContainsColoredText(rendered, "\x1b[38;5;81m", "int"),
           "added lines in patch preview should use file syntax highlighting");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;117m", "ComputeValue"),
           "patch preview should highlight functions in added lines");
    Expect(ContainsColoredText(rendered, "\x1b[38;5;221m", "\"hi\""),
           "patch preview should highlight strings in added lines");
}

void TestPlainTextFallbackAvoidsCppMiscoloring() {
    flowstate::Buffer buffer;
    buffer.setPath("notes.custom");
    buffer.setText("#include <iostream>\n", false);

    flowstate::EditorState state(std::move(buffer));
    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 80);

    Expect(rendered.find("\x1b[38;5;141m#include\x1b[39m") == std::string::npos,
           "unknown languages should not inherit C++ include highlighting");
    Expect(rendered.find("\x1b[38;5;214m<iostream>\x1b[39m") == std::string::npos,
           "plain-text fallback should avoid include-path miscoloring");
}

void TestNoAiClientDisabled() {
    flowstate::NoAiClient client;
    std::string error;
    Expect(!client.StartRequest({.kind = flowstate::AiRequestKind::Explain}, &error),
           "no-AI client should reject AI requests");
    Expect(error.find("disabled") != std::string::npos,
           "no-AI client should explain that AI is disabled");
    Expect(client.PollEvents().empty(), "no-AI client should not emit events");
    Expect(!client.HasActiveRequest(), "no-AI client should never report active requests");
}

void TestJsonParsing() {
    const std::string payload = R"({"id":7,"method":"item/agentMessage/delta","params":{"delta":"hello\nworld"}})";
    std::string error;
    const std::optional<flowstate::JsonValue> json = flowstate::JsonValue::Parse(payload, &error);
    Expect(json.has_value(), "json payload should parse");
    Expect(json->find("method") != nullptr && json->find("method")->stringValue() == "item/agentMessage/delta",
           "json parser should preserve strings");
    const flowstate::JsonValue* params = json->find("params");
    Expect(params != nullptr && params->find("delta") != nullptr,
           "json parser should expose nested objects");
    Expect(params->find("delta")->stringValue() == "hello\nworld",
           "json parser should unescape newlines");
}

void TestCompletionPrefixAndTriggers() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("object.member\nptr->value\nstd::", false);

    const flowstate::Cursor prefix = flowstate::CompletionPrefixStart(buffer, {.row = 0, .col = 13});
    Expect(prefix.row == 0 && prefix.col == 7, "completion prefix should start at the current identifier");
    Expect(flowstate::IsCompletionAutoTrigger(buffer, {.row = 0, .col = 13}),
           "identifier characters should trigger C++ completion while typing");
    Expect(flowstate::IsCompletionAutoTrigger(buffer, {.row = 0, .col = 7}),
           "dot should trigger C++ completion");
    Expect(flowstate::IsCompletionAutoTrigger(buffer, {.row = 1, .col = 5}),
           "arrow should trigger C++ completion after the greater-than character");
    Expect(flowstate::IsCompletionAutoTrigger(buffer, {.row = 2, .col = 5}),
           "scope operator should trigger C++ completion after the second colon");

    flowstate::Buffer python_buffer;
    python_buffer.setPath("sample.py");
    python_buffer.setText("self.value\nres", false);
    Expect(flowstate::IsCompletionAutoTrigger(python_buffer, {.row = 0, .col = 5}),
           "dot should trigger Python completion");
    Expect(flowstate::IsCompletionAutoTrigger(python_buffer, {.row = 1, .col = 3}),
           "identifier characters should trigger Python completion while typing");

    flowstate::Buffer javascript_buffer;
    javascript_buffer.setPath("sample.js");
    javascript_buffer.setText("object.member\nres", false);
    Expect(flowstate::IsCompletionAutoTrigger(javascript_buffer, {.row = 0, .col = 7}),
           "dot should trigger JavaScript completion");
    Expect(flowstate::IsCompletionAutoTrigger(javascript_buffer, {.row = 1, .col = 3}),
           "identifier characters should trigger JavaScript completion while typing");

    flowstate::Buffer typescript_buffer;
    typescript_buffer.setPath("sample.ts");
    typescript_buffer.setText("object.member\nres", false);
    Expect(flowstate::IsCompletionAutoTrigger(typescript_buffer, {.row = 0, .col = 7}),
           "dot should trigger TypeScript completion");
    Expect(flowstate::IsCompletionAutoTrigger(typescript_buffer, {.row = 1, .col = 3}),
           "identifier characters should trigger TypeScript completion while typing");

    flowstate::Buffer text_buffer;
    text_buffer.setPath("notes.txt");
    text_buffer.setText("object.", false);
    Expect(!flowstate::IsCompletionAutoTrigger(text_buffer, {.row = 0, .col = 7}),
           "unsupported files should not auto-trigger IntelliSense");
}

void TestLanguageServerRouting() {
    const std::optional<flowstate::LanguageServerKind> python_server =
        flowstate::LanguageServerKindForLanguage(flowstate::LanguageId::Python);
    Expect(python_server.has_value() && *python_server == flowstate::LanguageServerKind::Python,
           "Python should route to the Python language server");

    const std::optional<flowstate::LanguageServerKind> javascript_server =
        flowstate::LanguageServerKindForLanguage(flowstate::LanguageId::JavaScript);
    Expect(javascript_server.has_value() && *javascript_server == flowstate::LanguageServerKind::TypeScript,
           "JavaScript should route to the TypeScript language server");

    const std::optional<flowstate::LanguageServerKind> typescript_server =
        flowstate::LanguageServerKindForLanguage(flowstate::LanguageId::TypeScript);
    Expect(typescript_server.has_value() && *typescript_server == flowstate::LanguageServerKind::TypeScript,
           "TypeScript should route to the TypeScript language server");

    const std::optional<flowstate::LanguageServerKind> cpp_server =
        flowstate::LanguageServerKindForLanguage(flowstate::LanguageId::Cpp);
    Expect(cpp_server.has_value() && *cpp_server == flowstate::LanguageServerKind::Clangd,
           "C++ should route to clangd");

    Expect(!flowstate::LanguageServerKindForLanguage(flowstate::LanguageId::Markdown).has_value(),
           "unsupported languages should not route to a language server");
}

void TestApplyCompletionItem() {
    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() { ret }", false);
    flowstate::Cursor cursor{0, 16};

    const flowstate::CompletionItem item{
        .label = "return",
        .insert_text = "return",
    };
    Expect(flowstate::ApplyCompletionItem(buffer, cursor, item, {.row = 0, .col = 13}, {.row = 0, .col = 16}),
           "completion should apply a fallback replacement range");
    Expect(buffer.text() == "int main() { return }", "completion should replace the current identifier prefix");
    Expect(cursor.row == 0 && cursor.col == 19, "completion should place the cursor after inserted text");

    const flowstate::CompletionItem edit_item{
        .label = "co_return",
        .text_edit = flowstate::CompletionTextEdit{
            .start = {.row = 0, .col = 13},
            .end = {.row = 0, .col = 19},
            .new_text = "co_return",
        },
    };
    Expect(flowstate::ApplyCompletionItem(buffer, cursor, edit_item, {.row = 0, .col = 0}, {.row = 0, .col = 0}),
           "completion should prefer a valid LSP textEdit");
    Expect(buffer.text() == "int main() { co_return }", "completion textEdit should replace its explicit range");
}

void TestCompletionParsing() {
    const std::string payload =
        "{\"items\":["
        "{\"label\":\"push_back\",\"detail\":\"void vector::push_back(int)\",\"insertText\":\"push_back\"},"
        "{\"label\":\"snippet\",\"insertText\":\"snippet(${1:x})\",\"insertTextFormat\":2},"
        "{\"label\":\"size\",\"textEdit\":{\"range\":{\"start\":{\"line\":3,\"character\":4},"
        "\"end\":{\"line\":3,\"character\":8}},\"newText\":\"size\"}}]}";
    std::string error;
    const std::optional<flowstate::JsonValue> json = flowstate::JsonValue::Parse(payload, &error);
    Expect(json.has_value(), "completion fixture should parse as JSON");

    const std::vector<flowstate::CompletionItem> items = flowstate::ParseCompletionItemsForTest(*json);
    Expect(items.size() == 3, "completion parser should read CompletionList items");
    Expect(items[0].label == "push_back" && items[0].detail.find("vector") != std::string::npos,
           "completion parser should preserve label and detail");
    Expect(items[1].insert_text == "snippet", "snippet completions should fall back to plain labels");
    Expect(items[2].text_edit.has_value() && items[2].text_edit->start.row == 3 &&
               items[2].text_edit->start.col == 4,
           "completion parser should preserve LSP textEdit ranges");
}

void TestDiagnosticParsingAndRendering() {
    const std::string payload =
        "{\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,\"character\":4},"
        "\"end\":{\"line\":0,\"character\":8}},\"severity\":1,"
        "\"message\":\"expected ';'\",\"source\":\"clangd\"}]}";
    std::string error;
    const std::optional<flowstate::JsonValue> json = flowstate::JsonValue::Parse(payload, &error);
    Expect(json.has_value(), "diagnostic fixture should parse as JSON");

    const std::vector<flowstate::Diagnostic> diagnostics = flowstate::ParseDiagnosticsForTest(*json);
    Expect(diagnostics.size() == 1, "diagnostic parser should read publishDiagnostics entries");
    Expect(diagnostics[0].range.start.row == 0 && diagnostics[0].range.start.col == 4,
           "diagnostic parser should preserve start positions");
    Expect(diagnostics[0].message == "expected ';'", "diagnostic parser should preserve messages");
    Expect(diagnostics[0].source == "clangd", "diagnostic parser should preserve sources");
    Expect(flowstate::HasErrorDiagnosticAt(diagnostics, 0, 4),
           "error diagnostics should cover their range start");
    Expect(flowstate::ErrorDiagnosticAt(diagnostics, 0, 4) != nullptr &&
               flowstate::ErrorDiagnosticAt(diagnostics, 0, 4)->message == "expected ';'",
           "diagnostic lookup should expose the message at the cursor");
    Expect(!flowstate::HasErrorDiagnosticAt(diagnostics, 0, 8),
           "diagnostic range end should remain exclusive");

    flowstate::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main", false);
    flowstate::EditorState state(std::move(buffer));
    state.fileCursor() = {.row = 0, .col = 4};
    state.setDiagnostics(diagnostics);

    flowstate::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 80);
    Expect(rendered.find("\x1b[4m\x1b[58;5;196m") != std::string::npos,
           "file rendering should apply red underline to clangd errors");
    Expect(rendered.find("\x1b[48;5;52m") != std::string::npos,
           "file rendering should apply a dark red background to clangd errors");
    Expect(rendered.find("\x1b[24m\x1b[59m") != std::string::npos,
           "file rendering should reset diagnostic underline after the range");
    Expect(rendered.find("\x1b[49m") != std::string::npos,
           "file rendering should reset diagnostic background after the range");
    Expect(rendered.find("clangd: expected ';'") != std::string::npos,
           "file rendering should show a diagnostic bubble when the cursor is on an error");
    Expect(rendered.find("\x1b[48;5;52m\x1b[38;5;231m") != std::string::npos,
           "diagnostic bubble should use a red-accented style");
}

class FakeLocalAgentClient : public flowstate::ILocalAgentClient {
  public:
    bool StartSession(const flowstate::LocalAgentSessionConfig& config,
                      std::string* session_id,
                      std::string* error) override {
        (void)config;
        (void)error;
        current_session_id_ = "session-1";
        if (session_id != nullptr) {
            *session_id = current_session_id_;
        }
        events_.push_back({.kind = flowstate::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Connecting});
        events_.push_back({.kind = flowstate::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Idle});
        return true;
    }

    bool SendMessage(const std::string& session_id,
                     const flowstate::LocalAgentRequest& request,
                     std::string* error) override {
        (void)request;
        (void)error;
        if (session_id != current_session_id_) {
            return false;
        }
        events_.push_back({.kind = flowstate::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Active});
        events_.push_back({.kind = flowstate::LocalAgentEventKind::TextDelta,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Active,
                           .text_delta = "hello"});
        events_.push_back({.kind = flowstate::LocalAgentEventKind::FinalText,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Active,
                           .final_text = "hello"});
        events_.push_back({.kind = flowstate::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Idle});
        active_ = true;
        return true;
    }

    std::vector<flowstate::LocalAgentEvent> PollEvents() override {
        active_ = false;
        std::vector<flowstate::LocalAgentEvent> result = std::move(events_);
        events_.clear();
        return result;
    }

    bool RefreshRateLimits(std::string* error) override {
        (void)error;
        events_.push_back({.kind = flowstate::LocalAgentEventKind::RateLimitsUpdated,
                           .session_id = current_session_id_,
                           .session_state = flowstate::LocalAgentSessionState::Idle,
                           .rate_limits = TestRateLimits(25.0, 60.0)});
        return true;
    }

    bool HasActiveMessage() const override { return active_; }

    void CloseSession(const std::string& session_id) override {
        if (session_id == current_session_id_) {
            current_session_id_.clear();
        }
    }

    void Shutdown() override {
        events_.clear();
        current_session_id_.clear();
        active_ = false;
    }

  private:
    std::string current_session_id_;
    std::vector<flowstate::LocalAgentEvent> events_;
    bool active_ = false;
};

void TestCodexClientIgnoresInitialIdleBeforeFirstTurn() {
    flowstate::CodexClient client(std::make_unique<FakeLocalAgentClient>());
    std::string error;
    Expect(client.StartRequest({.kind = flowstate::AiRequestKind::Explain, .file_path = "sample.cpp"}, &error),
           "codex request should start");

    const std::vector<flowstate::AiEvent> events = client.PollEvents();
    bool saw_completed = false;
    bool saw_error = false;
    bool saw_text_delta = false;
    for (const flowstate::AiEvent& event : events) {
        if (event.kind == flowstate::AiEventKind::TextDelta) {
            saw_text_delta = true;
            Expect(event.text_delta == "hello", "codex adapter should preserve streamed text");
        }
        if (event.kind == flowstate::AiEventKind::Completed) {
            saw_completed = true;
            Expect(event.response.raw_text == "hello", "codex adapter should preserve final text");
        }
        if (event.kind == flowstate::AiEventKind::Error) {
            saw_error = true;
        }
    }

    Expect(saw_text_delta, "codex adapter should emit a text delta");
    Expect(saw_completed, "codex adapter should complete after the first request");
    Expect(!saw_error, "codex adapter should not fail on the initial idle session event");
}

void TestCodexClientForwardsRateLimitUpdates() {
    flowstate::CodexClient client(std::make_unique<FakeLocalAgentClient>());
    std::string error;
    Expect(client.StartRequest({.kind = flowstate::AiRequestKind::Explain, .file_path = "sample.cpp"}, &error),
           "codex request should start before forwarding rate limits");

    const std::vector<flowstate::AiEvent> events = client.PollEvents();
    bool saw_rate_limits = false;
    for (const flowstate::AiEvent& event : events) {
        if (event.kind != flowstate::AiEventKind::RateLimitsUpdated) {
            continue;
        }
        saw_rate_limits = true;
        Expect(event.rate_limits.available, "codex adapter should preserve rate limit availability");
        Expect(event.rate_limits.primary.available && event.rate_limits.primary.used_percent == 25.0,
               "codex adapter should preserve the 5h usage window");
        Expect(event.rate_limits.secondary.available && event.rate_limits.secondary.used_percent == 60.0,
               "codex adapter should preserve the weekly usage window");
    }

    Expect(saw_rate_limits, "codex adapter should forward rate limit updates");
}

}  // namespace

int main() {
    try {
        TestBufferEditing();
        TestLoadFileRejectsUnreadableContent();
        TestSelectionExtraction();
        TestSelectionRangeHelpers();
        TestBufferRangeEditing();
        TestDeleteRangePlacesCursorAtSelectionStart();
        TestIndentedNewlineAndBackspace();
        TestInsertIndentUsesTabStops();
        TestLineCommentToggle();
        TestPairedCharacterInsertion();
        TestEditorStateUndoRedo();
        TestGitChangePeekExpansionState();
        TestCommandParsing();
        TestDiffParsingAndPatchApply();
        TestDiffExtractionWithProse();
        TestGitDiffMarkerParsing();
        TestBuildRunner();
        TestLanguageDetection();
        TestCppHighlighterSpans();
        TestGoHighlighterSpans();
        TestJavaScriptHighlighterSpans();
        TestJavaHighlighterSpans();
        TestMarkdownHighlighterSpans();
        TestPythonHighlighterSpans();
        TestRustHighlighterSpans();
        TestTypeScriptHighlighterSpans();
        TestIncludeHighlightRendering();
        TestCppRenderHighlightsExpandedTokenSet();
        TestBraceNestingRendering();
        TestSquareAndRoundDelimiterHighlighting();
        TestGoRenderHighlightsExpandedTokenSet();
        TestJavaScriptRenderHighlightsExpandedTokenSet();
        TestJavaRenderHighlightsExpandedTokenSet();
        TestMarkdownRenderHighlightsExpandedTokenSet();
        TestPythonRenderHighlightsExpandedTokenSet();
        TestRustRenderHighlightsExpandedTokenSet();
        TestTypeScriptRenderHighlightsExpandedTokenSet();
        TestLineNumberGutterAffectsVisibleWidth();
        TestSelectionUsesStableBackgroundHighlight();
        TestCompletionPopupDoesNotMoveStatusBar();
        TestAiScratchDoesNotRenderLineNumbers();
        TestAiScratchBoldsUserMessages();
        TestInlineAiExplainRendersInFileView();
        TestInlineAiExplainBoldsUserMessages();
        TestInlineAiExplainHighlightsFencedCode();
        TestInlineAiExplainPanelIsScrollable();
        TestInlineAiExplainDownAtBottomReturnsToFileWithoutClosing();
        TestInlineAiExplainUpAtTopReturnsToFileWithoutClosing();
        TestInlineAiExplainFooterShowsCodexUsageBars();
        TestAiScratchDiffHunksUseFileSyntaxHighlighting();
        TestPatchPreviewAddedLinesUseFileSyntaxHighlighting();
        TestPlainTextFallbackAvoidsCppMiscoloring();
        TestNoAiClientDisabled();
        TestJsonParsing();
        TestCompletionPrefixAndTriggers();
        TestLanguageServerRouting();
        TestApplyCompletionItem();
        TestCompletionParsing();
        TestDiagnosticParsingAndRendering();
        TestCodexClientIgnoresInitialIdleBeforeFirstTurn();
        TestCodexClientForwardsRateLimitUpdates();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
