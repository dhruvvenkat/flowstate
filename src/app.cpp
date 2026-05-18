#include "app.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <limits>
#include <sstream>

#include "ai/codex_client.h"
#include "ai/openai_client.h"
#include "command.h"
#include "cursor.h"
#include "diff.h"
#include "git_status.h"
#include "intellisense/completion.h"
#include "patch.h"
#include "selection.h"

namespace flowstate {

namespace {

constexpr size_t kContextLines = 3;
constexpr size_t kPageMoveDistance = 10;
constexpr size_t kMouseScrollDistance = 4;
constexpr std::chrono::milliseconds kAiLoadingTick(120);

std::optional<char> ClosingBracketFor(char ch) {
    switch (ch) {
        case '(':
            return ')';
        case '[':
            return ']';
        case '{':
            return '}';
        default:
            return std::nullopt;
    }
}

bool IsAutoclosedPair(char open, char close) {
    const std::optional<char> expected_close = ClosingBracketFor(open);
    return expected_close.has_value() && *expected_close == close;
}

std::string JoinRange(const Buffer& buffer, size_t start, size_t end) {
    if (buffer.lineCount() == 0 || start >= buffer.lineCount() || start > end) {
        return {};
    }

    std::ostringstream output;
    for (size_t row = start; row <= end && row < buffer.lineCount(); ++row) {
        if (row > start) {
            output << '\n';
        }
        output << buffer.line(row);
    }
    return output.str();
}

size_t SelectionStartRow(const EditorState& state) {
    if (HasSelection(state.selection())) {
        return NormalizeSelection(state.selection()).start.row;
    }
    return state.fileCursor().row;
}

size_t SelectionEndRow(const EditorState& state) {
    if (HasSelection(state.selection())) {
        return NormalizeSelection(state.selection()).end.row;
    }
    return state.fileCursor().row;
}

std::string AiRequestStateLabel(AiRequestState state) {
    switch (state) {
        case AiRequestState::Connecting:
            return "CONNECTING";
        case AiRequestState::Streaming:
            return "STREAMING";
        case AiRequestState::ParsingPatch:
            return "PARSING PATCH";
        case AiRequestState::Failed:
            return "FAILED";
        case AiRequestState::Complete:
            return "COMPLETE";
    }
    return {};
}

std::string CollapseStatusText(const std::string& text) {
    std::string collapsed;
    collapsed.reserve(text.size());

    bool last_was_space = false;
    for (char ch : text) {
        const bool is_space = (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ');
        if (is_space) {
            if (!collapsed.empty() && !last_was_space) {
                collapsed.push_back(' ');
            }
            last_was_space = true;
            continue;
        }

        collapsed.push_back(ch);
        last_was_space = false;
    }

    while (!collapsed.empty() && collapsed.front() == ' ') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

std::string AiFailureStatus(const std::string& error_message, bool backgrounded) {
    std::string status = CollapseStatusText(error_message);
    if (status.empty()) {
        status = "AI request failed.";
    }
    if (backgrounded) {
        status += " Press Alt+E to reopen AI scratch.";
    }
    return status;
}

std::optional<size_t> ParseOneBasedLineNumber(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    size_t parsed = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

std::string Lowercase(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const unsigned char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

bool IsFilePickerMatch(std::string_view path, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    return Lowercase(path).find(Lowercase(query)) != std::string::npos;
}

bool IsSkippedPickerDirectory(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == "build" || name == ".cache" || name == "node_modules";
}

std::string ScaffoldedPickerPath(std::string_view path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return std::string(path);
    }

    size_t depth = 0;
    for (size_t index = 0; index < slash; ++index) {
        if (path[index] == '/') {
            ++depth;
        }
    }
    return std::string((depth + 1) * 2, ' ') + std::string(path.substr(slash + 1));
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::filesystem::current_path();
    }
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::optional<std::filesystem::path> FindContainingGitRoot(std::filesystem::path path) {
    path = AbsolutePath(path);
    if (!std::filesystem::is_directory(path)) {
        path = path.parent_path();
    }

    while (!path.empty()) {
        if (std::filesystem::exists(path / ".git")) {
            return path;
        }
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return std::nullopt;
}

size_t ExpandedGitRowsBetween(const EditorState& state,
                              const GitLineStatus& git_status,
                              size_t start_row,
                              size_t end_row) {
    size_t count = 0;
    for (size_t row = start_row; row < end_row && row < git_status.lines.size(); ++row) {
        if (state.isGitChangePeekExpanded(row)) {
            count += git_status.lines[row].previous_lines.size();
        }
    }
    return count;
}

}  // namespace

EditorApp::EditorApp(Buffer file_buffer,
                     std::unique_ptr<IAiClient> ai_client,
                     std::string build_command,
                     std::string ai_provider_name,
                     std::string cpp_standard)
    : state_(std::move(file_buffer)),
      ai_client_(std::move(ai_client)),
      language_server_client_(std::move(cpp_standard)) {
    state_.setBuildCommand(std::move(build_command));
    state_.setAiProviderName(std::move(ai_provider_name));
    state_.setStatus("Alt+C commands, Ctrl+F finds, Shift+Arrow selects, Ctrl+/ comments, Ctrl+C copies.");
}

int EditorApp::Run() {
    std::string terminal_error;
    if (!terminal_.EnableRawMode(&terminal_error)) {
        terminal_.Write("Failed to initialize terminal raw mode: " + terminal_error + "\n");
        return 1;
    }

    while (running_) {
        EnsureRealtimeDiagnostics();
        PollAiRequest();
        PollCompletionRequest();
        RefreshScreen();
        const KeyPress key = terminal_.ReadKey();
        EnsureRealtimeDiagnostics();
        PollAiRequest();
        PollCompletionRequest();
        if (key.type == KeyType::Unknown) {
            continue;
        }
        if (HandleMouseScroll(key)) {
            continue;
        }
        if (command_mode_) {
            HandleCommandKey(key);
        } else if (file_picker_mode_) {
            HandleFilePickerKey(key);
        } else {
            HandleNormalKey(key);
        }
    }

    terminal_.Write("\x1b[m\x1b[2J\x1b[H");
    return 0;
}

void EditorApp::RefreshScreen() {
    const auto [screen_rows, screen_cols] = terminal_.WindowSize();
    ScrollToCursor(screen_rows, screen_cols);
    terminal_.Write(screen_.Render(state_,
                                   {.command_mode = command_mode_,
                                    .command_input = command_input_,
                                    .file_picker_mode = file_picker_mode_,
                                    .file_picker_query = file_picker_query_,
                                    .file_picker_selected = file_picker_selected_},
                                   screen_rows,
                                   screen_cols));
}

void EditorApp::ScrollToCursor(int screen_rows, int screen_cols) {
    Viewport& viewport = state_.activeViewport();
    const int content_rows = std::max(1, screen_rows - 2);
    const size_t content_cols = screen_.ContentColumns(state_, screen_cols);

    if (viewport.cursor.row < viewport.row_offset) {
        viewport.row_offset = viewport.cursor.row;
    }
    if (viewport.cursor.row >= viewport.row_offset + static_cast<size_t>(content_rows)) {
        viewport.row_offset = viewport.cursor.row - static_cast<size_t>(content_rows) + 1;
    }
    if (state_.activeView() == ViewKind::File) {
        std::optional<GitLineStatus> git_status;
        if (state_.hasGitChangePeekExpansions()) {
            git_status = LoadGitLineStatus(state_.fileBuffer().path().value_or(std::filesystem::path()),
                                           state_.fileBuffer().lineCount());
        }
        while (viewport.cursor.row > viewport.row_offset &&
               [&]() {
                   size_t extra_rows =
                       screen_.InlineAiRowsBetween(state_, content_cols, viewport.row_offset, viewport.cursor.row);
                   if (git_status.has_value()) {
                       extra_rows +=
                           ExpandedGitRowsBetween(state_, *git_status, viewport.row_offset, viewport.cursor.row);
                   }
                   return viewport.cursor.row - viewport.row_offset + extra_rows >=
                          static_cast<size_t>(content_rows);
               }()) {
            ++viewport.row_offset;
        }
        while (viewport.cursor.row > viewport.row_offset && state_.inlineAiSession().has_value() &&
               state_.inlineAiSession()->focused &&
               [&]() {
                   const InlineAiSession& session = *state_.inlineAiSession();
                   const size_t body_rows = screen_.InlineAiBodyRowCount(state_, content_cols);
                   if (body_rows == 0) {
                       return false;
                   }
                   const size_t visible_rows = screen_.InlineAiVisibleBodyRowCount(state_, content_cols);
                   size_t extra_rows = visible_rows + 3;
                   if (git_status.has_value()) {
                       extra_rows +=
                           ExpandedGitRowsBetween(state_, *git_status, viewport.row_offset, viewport.cursor.row);
                       if (viewport.cursor.row < git_status->lines.size()) {
                           extra_rows += git_status->lines[viewport.cursor.row].previous_lines.size();
                       }
                   }
                   return viewport.cursor.row - viewport.row_offset + extra_rows >=
                          static_cast<size_t>(content_rows);
               }()) {
            ++viewport.row_offset;
        }
    }
    if (viewport.cursor.col < viewport.col_offset) {
        viewport.col_offset = viewport.cursor.col;
    }
    if (viewport.cursor.col >= viewport.col_offset + content_cols) {
        viewport.col_offset = viewport.cursor.col - content_cols + 1;
    }
}

void EditorApp::HandleNormalKey(const KeyPress& key) {
    if (!(key.ctrl && key.ch == 'q')) {
        pending_quit_confirm_ = false;
    }

    if (state_.completionSession().active && HandleCompletionKey(key)) {
        return;
    }

    if (key.ctrl) {
        switch (key.ch) {
            case 'q':
                if (state_.fileBuffer().dirty() && !pending_quit_confirm_) {
                    pending_quit_confirm_ = true;
                    state_.setStatus("Unsaved changes. Press Ctrl+Q again to quit.");
                } else {
                    QuitEditor();
                }
                return;
            case 's':
                SaveFile();
                return;
            case 'c':
                CopySelectionOrLine();
                return;
            case 'e':
                RunAiRequest(AiRequestKind::Explain, "Explain this code.");
                return;
            case 'f':
                StartFindPrompt();
                return;
            case 'o':
                StartFilePicker();
                return;
            case 'r':
                RunAiRequest(AiRequestKind::Fix, "Fix bugs in this code and keep the patch minimal.");
                return;
            case 't':
                RunBuild();
                return;
            case 'v':
                PasteClipboard();
                return;
            case 'x':
                CutSelectionOrLine();
                return;
            case 'y':
                RedoFileEdit();
                return;
            case 'z':
                UndoFileEdit();
                return;
            case '/':
                ToggleLineComment();
                return;
        }
    }

    if (key.alt) {
        if (key.ch == 'a') {
            HandlePatchAction(CommandType::PatchAccept);
            return;
        }
        if (key.ch == 'c') {
            StartCommandPrompt();
            return;
        }
        if (key.ch == 'd') {
            ToggleGitPreviousLines();
            return;
        }
        if (key.ch == 'e') {
            ReopenAiScratch();
            return;
        }
        if (key.ch == 'i') {
            RequestCompletion(false);
            return;
        }
        if (key.ch == 'p') {
            ReopenPatchPreview();
            return;
        }
        if (key.ch == 'r') {
            HandlePatchAction(CommandType::PatchReject);
            return;
        }
    }

    if (HandleInlineAiKey(key)) {
        return;
    }

    switch (key.type) {
        case KeyType::ArrowLeft:
        case KeyType::ArrowRight:
        case KeyType::ArrowUp:
        case KeyType::ArrowDown:
            if (key.shift && state_.activeView() == ViewKind::File) {
                ExtendSelection(key.type);
                return;
            }
            MoveCursor(key.type);
            return;
        case KeyType::Home:
            if (key.shift && state_.activeView() == ViewKind::File) {
                ExtendSelectionToLineBoundary(key.type);
                return;
            }
            CursorController::moveHome(state_.activeViewport().cursor);
            UpdateSelectionHead();
            return;
        case KeyType::End:
            if (key.shift && state_.activeView() == ViewKind::File) {
                ExtendSelectionToLineBoundary(key.type);
                return;
            }
            CursorController::moveEnd(state_.activeViewport().cursor, state_.activeBuffer());
            UpdateSelectionHead();
            return;
        case KeyType::PageUp:
            MoveCursor(KeyType::ArrowUp, kPageMoveDistance);
            return;
        case KeyType::PageDown:
            MoveCursor(KeyType::ArrowDown, kPageMoveDistance);
            return;
        case KeyType::Escape:
            if (state_.activeView() != ViewKind::File) {
                state_.setActiveView(ViewKind::File);
                if (active_ai_request_.has_value() || ai_client_->HasActiveRequest()) {
                    ai_request_backgrounded_ = true;
                    state_.setStatus("Returned to file buffer. AI request continues in background.", 60);
                } else {
                    state_.setStatus("Returned to file buffer.");
                }
            } else if (state_.inlineAiSession().has_value()) {
                const bool request_running =
                    active_ai_request_.has_value() && active_ai_request_->inline_viewer;
                state_.clearInlineAiSession();
                if (request_running) {
                    ai_request_backgrounded_ = true;
                    state_.setStatus("Closed inline AI viewer. AI request continues in background.", 60);
                } else {
                    state_.clearAiRequestState();
                    state_.setStatus("Closed inline AI viewer.");
                }
            } else if (state_.selection().active) {
                state_.clearSelection();
                state_.setStatus("Selection cleared.");
            }
            return;
        case KeyType::Backspace:
            if (state_.activeView() == ViewKind::File) {
                if (DeleteSelectionIfActive()) {
                    return;
                }
                if (DeleteAutoclosedPairIfActive()) {
                    return;
                }
                state_.BeginFileEdit();
                state_.fileBuffer().deleteCharBefore(state_.fileCursor());
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::DeleteKey:
            if (state_.activeView() == ViewKind::File) {
                if (DeleteSelectionIfActive()) {
                    return;
                }
                state_.BeginFileEdit();
                state_.fileBuffer().deleteCharAt(state_.fileCursor());
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::Tab:
            if (state_.activeView() == ViewKind::File) {
                state_.BeginFileEdit();
                state_.fileBuffer().insertIndent(state_.fileCursor());
                UpdateSelectionHead();
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::Enter:
            if (state_.activeView() == ViewKind::File) {
                state_.BeginFileEdit();
                state_.fileBuffer().insertNewline(state_.fileCursor());
                UpdateSelectionHead();
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::Character:
            if (state_.activeView() == ViewKind::File && !key.ctrl &&
                static_cast<unsigned char>(key.ch) >= 32) {
                InsertCharacter(key.ch);
            }
            return;
        case KeyType::Unknown:
        case KeyType::MouseWheelUp:
        case KeyType::MouseWheelDown:
            return;
    }
}

bool EditorApp::HandleMouseScroll(const KeyPress& key) {
    if (key.type != KeyType::MouseWheelUp && key.type != KeyType::MouseWheelDown) {
        return false;
    }

    if (file_picker_mode_) {
        if (!file_picker_matches_.empty()) {
            if (key.type == KeyType::MouseWheelUp) {
                file_picker_selected_ =
                    file_picker_selected_ > kMouseScrollDistance ? file_picker_selected_ - kMouseScrollDistance : 0;
            } else {
                file_picker_selected_ =
                    std::min(file_picker_matches_.size() - 1, file_picker_selected_ + kMouseScrollDistance);
            }
            state_.activeViewport().cursor.row = file_picker_selected_ + 1;
        }
        return true;
    }

    Buffer& buffer = state_.activeBuffer();
    if (buffer.lineCount() == 0) {
        return true;
    }

    const auto window_size = terminal_.WindowSize();
    const size_t content_rows = static_cast<size_t>(std::max(1, window_size.first - 2));
    Viewport& viewport = state_.activeViewport();
    const size_t max_row_offset = buffer.lineCount() > content_rows ? buffer.lineCount() - content_rows : 0;

    if (key.type == KeyType::MouseWheelUp) {
        viewport.row_offset =
            viewport.row_offset > kMouseScrollDistance ? viewport.row_offset - kMouseScrollDistance : 0;
    } else {
        viewport.row_offset = std::min(max_row_offset, viewport.row_offset + kMouseScrollDistance);
    }
    viewport.row_offset = std::min(viewport.row_offset, max_row_offset);

    const size_t visible_end =
        std::min(buffer.lineCount() - 1, viewport.row_offset + content_rows - 1);
    if (viewport.cursor.row < viewport.row_offset) {
        viewport.cursor.row = viewport.row_offset;
    }
    if (viewport.cursor.row > visible_end) {
        viewport.cursor.row = visible_end;
    }
    CursorController::clamp(viewport.cursor, buffer);

    if (state_.activeView() == ViewKind::PatchPreview && state_.patchSession().has_value()) {
        PatchSession& session = *state_.patchSession();
        session.current_hunk = HunkIndexForPreviewRow(session, state_.viewport(ViewKind::PatchPreview).cursor.row);
    }
    return true;
}

bool EditorApp::HandleInlineAiKey(const KeyPress& key) {
    if (state_.activeView() != ViewKind::File || !state_.inlineAiSession().has_value() || key.ctrl || key.alt) {
        return false;
    }

    const auto window_size = terminal_.WindowSize();
    const int screen_cols = window_size.second;
    const size_t content_cols = screen_.ContentColumns(state_, screen_cols);
    const size_t body_rows = screen_.InlineAiBodyRowCount(state_, content_cols);
    if (body_rows == 0 || state_.fileBuffer().lineCount() == 0) {
        return false;
    }

    const size_t anchor_row = std::min(state_.inlineAiSession()->anchor_row, state_.fileBuffer().lineCount() - 1);
    const size_t current_file_row = state_.fileCursor().row;

    if (state_.inlineAiSession()->focused) {
        InlineAiSession& session = *state_.inlineAiSession();
        switch (key.type) {
            case KeyType::ArrowUp:
                if (session.scroll_row == 0) {
                    return LeaveInlineAiBody(session.anchor_row);
                }
                return ScrollInlineAiBody(-1, content_cols);
            case KeyType::ArrowDown: {
                const size_t visible_rows = screen_.InlineAiVisibleBodyRowCount(state_, content_cols);
                const size_t max_scroll_row = body_rows > visible_rows ? body_rows - visible_rows : 0;
                session.scroll_row = std::min(session.scroll_row, max_scroll_row);
                if (session.scroll_row >= max_scroll_row) {
                    return LeaveInlineAiBody(session.anchor_row + 1);
                }
                return ScrollInlineAiBody(1, content_cols);
            }
            case KeyType::PageUp:
                return ScrollInlineAiBody(-static_cast<int>(kPageMoveDistance), content_cols);
            case KeyType::PageDown:
                return ScrollInlineAiBody(static_cast<int>(kPageMoveDistance), content_cols);
            case KeyType::Home:
                session.input_cursor_col = 0;
                return true;
            case KeyType::End:
                session.input_cursor_col = session.input_text.size();
                return true;
            case KeyType::Backspace:
                if (session.input_cursor_col > 0) {
                    session.input_text.erase(session.input_cursor_col - 1, 1);
                    --session.input_cursor_col;
                }
                return true;
            case KeyType::DeleteKey:
                if (session.input_cursor_col < session.input_text.size()) {
                    session.input_text.erase(session.input_cursor_col, 1);
                }
                return true;
            case KeyType::Enter:
                SubmitInlineAiFollowUp();
                return true;
            case KeyType::Tab:
                session.input_text.insert(session.input_cursor_col, "    ");
                session.input_cursor_col += 4;
                return true;
            case KeyType::Character:
                if (static_cast<unsigned char>(key.ch) >= 32) {
                    session.input_text.insert(session.input_cursor_col, 1, key.ch);
                    ++session.input_cursor_col;
                }
                return true;
            case KeyType::ArrowLeft:
                if (session.input_cursor_col > 0) {
                    --session.input_cursor_col;
                }
                return true;
            case KeyType::ArrowRight:
                if (session.input_cursor_col < session.input_text.size()) {
                    ++session.input_cursor_col;
                }
                return true;
            case KeyType::Escape:
            case KeyType::Unknown:
            case KeyType::MouseWheelUp:
            case KeyType::MouseWheelDown:
                return false;
        }
        return false;
    }

    if (key.shift) {
        return false;
    }

    switch (key.type) {
        case KeyType::ArrowDown:
            if (current_file_row == anchor_row) {
                return EnterInlineAiBody(0, content_cols);
            }
            return false;
        case KeyType::ArrowUp:
            if (anchor_row + 1 < state_.fileBuffer().lineCount() && current_file_row == anchor_row + 1) {
                return EnterInlineAiBody(body_rows - 1, content_cols);
            }
            return false;
        case KeyType::PageDown:
            if (current_file_row <= anchor_row && anchor_row - current_file_row <= kPageMoveDistance) {
                return EnterInlineAiBody(0, content_cols);
            }
            return false;
        case KeyType::PageUp: {
            if (current_file_row > anchor_row && current_file_row - anchor_row <= kPageMoveDistance + 1) {
                return EnterInlineAiBody(body_rows - 1, content_cols);
            }
            return false;
        }
        default:
            return false;
    }

    return false;
}

bool EditorApp::EnterInlineAiBody(size_t cursor_body_row, size_t content_cols) {
    if (!state_.inlineAiSession().has_value()) {
        return false;
    }

    const size_t body_rows = screen_.InlineAiBodyRowCount(state_, content_cols);
    if (body_rows == 0 || state_.fileBuffer().lineCount() == 0) {
        return false;
    }

    InlineAiSession& session = *state_.inlineAiSession();
    session.anchor_row = std::min(session.anchor_row, state_.fileBuffer().lineCount() - 1);
    state_.fileCursor().row = session.anchor_row;
    CursorController::clamp(state_.fileCursor(), state_.fileBuffer());
    state_.clearSelection();
    session.focused = true;
    return SetInlineAiCursorBodyRow(std::min(cursor_body_row, body_rows - 1), content_cols);
}

bool EditorApp::LeaveInlineAiBody(size_t target_file_row) {
    if (!state_.inlineAiSession().has_value() || state_.fileBuffer().lineCount() == 0) {
        return false;
    }

    InlineAiSession& session = *state_.inlineAiSession();
    session.focused = false;
    state_.fileCursor().row = std::min(target_file_row, state_.fileBuffer().lineCount() - 1);
    CursorController::clamp(state_.fileCursor(), state_.fileBuffer());
    state_.clearSelection();
    return true;
}

bool EditorApp::ScrollInlineAiBody(int delta, size_t content_cols) {
    if (!state_.inlineAiSession().has_value()) {
        return false;
    }

    const size_t body_rows = screen_.InlineAiBodyRowCount(state_, content_cols);
    if (body_rows == 0) {
        return false;
    }

    InlineAiSession& session = *state_.inlineAiSession();
    const size_t visible_rows = screen_.InlineAiVisibleBodyRowCount(state_, content_cols);
    const size_t max_scroll_row = body_rows > visible_rows ? body_rows - visible_rows : 0;
    session.scroll_row = std::min(session.scroll_row, max_scroll_row);
    if (delta < 0) {
        const size_t step = static_cast<size_t>(-delta);
        session.scroll_row = step > session.scroll_row ? 0 : session.scroll_row - step;
    } else {
        session.scroll_row = std::min(max_scroll_row, session.scroll_row + static_cast<size_t>(delta));
    }
    session.cursor_body_row = std::min(session.scroll_row, body_rows - 1);
    return true;
}

bool EditorApp::SetInlineAiCursorBodyRow(size_t cursor_body_row, size_t content_cols) {
    if (!state_.inlineAiSession().has_value()) {
        return false;
    }

    const size_t body_rows = screen_.InlineAiBodyRowCount(state_, content_cols);
    if (body_rows == 0) {
        return false;
    }

    const size_t visible_rows = screen_.InlineAiVisibleBodyRowCount(state_, content_cols);
    InlineAiSession& session = *state_.inlineAiSession();
    session.cursor_body_row = std::min(cursor_body_row, body_rows - 1);

    if (body_rows <= visible_rows) {
        session.scroll_row = 0;
        return true;
    }

    const size_t max_scroll_row = body_rows - visible_rows;
    if (session.cursor_body_row < session.scroll_row) {
        session.scroll_row = session.cursor_body_row;
    } else if (session.cursor_body_row >= session.scroll_row + visible_rows) {
        session.scroll_row = session.cursor_body_row - visible_rows + 1;
    }
    session.scroll_row = std::min(session.scroll_row, max_scroll_row);
    return true;
}

void EditorApp::HandleCommandKey(const KeyPress& key) {
    switch (key.type) {
        case KeyType::Escape:
            command_mode_ = false;
            command_input_.clear();
            state_.setStatus("Command cancelled.");
            return;
        case KeyType::Backspace:
            if (!command_input_.empty()) {
                command_input_.pop_back();
            }
            return;
        case KeyType::Enter: {
            command_mode_ = false;
            const Command command = ParseCommand(command_input_);
            command_input_.clear();
            if (!ExecuteCommand(command)) {
                state_.setStatus(command.error.empty() ? "Command failed." : command.error);
            }
            return;
        }
        case KeyType::Character:
            if (!key.ctrl && static_cast<unsigned char>(key.ch) >= 32) {
                command_input_.push_back(key.ch);
            }
            return;
        default:
            return;
    }
}

void EditorApp::HandleFilePickerKey(const KeyPress& key) {
    switch (key.type) {
        case KeyType::Escape:
            file_picker_mode_ = false;
            file_picker_query_.clear();
            file_picker_matches_.clear();
            state_.setActiveView(ViewKind::File);
            state_.setStatus("File picker cancelled.");
            return;
        case KeyType::Backspace:
            if (!file_picker_query_.empty()) {
                file_picker_query_.pop_back();
                file_picker_selected_ = 0;
                RefreshFilePickerMatches();
            }
            return;
        case KeyType::Enter:
            if (file_picker_matches_.empty()) {
                state_.setStatus("No file selected.");
                return;
            } else {
                const std::filesystem::path path = file_picker_root_ / file_picker_matches_[file_picker_selected_];
                file_picker_mode_ = false;
                file_picker_query_.clear();
                file_picker_matches_.clear();
                OpenFile(path.string());
            }
            return;
        case KeyType::ArrowUp:
            if (file_picker_selected_ > 0) {
                --file_picker_selected_;
                state_.activeViewport().cursor.row = file_picker_selected_ + 1;
            }
            return;
        case KeyType::ArrowDown:
            if (file_picker_selected_ + 1 < file_picker_matches_.size()) {
                ++file_picker_selected_;
                state_.activeViewport().cursor.row = file_picker_selected_ + 1;
            }
            return;
        case KeyType::Character:
            if (!key.ctrl && static_cast<unsigned char>(key.ch) >= 32) {
                file_picker_query_.push_back(key.ch);
                file_picker_selected_ = 0;
                RefreshFilePickerMatches();
            }
            return;
        default:
            return;
    }
}

void EditorApp::MoveCursor(KeyType key, size_t distance) {
    for (size_t step = 0; step < distance; ++step) {
        switch (key) {
            case KeyType::ArrowLeft:
                CursorController::moveLeft(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            case KeyType::ArrowRight:
                CursorController::moveRight(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            case KeyType::ArrowUp:
                CursorController::moveUp(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            case KeyType::ArrowDown:
                CursorController::moveDown(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            default:
                break;
        }
    }

    if (state_.activeView() == ViewKind::PatchPreview && state_.patchSession().has_value()) {
        PatchSession& session = *state_.patchSession();
        session.current_hunk = HunkIndexForPreviewRow(session, state_.viewport(ViewKind::PatchPreview).cursor.row);
    }
    UpdateSelectionHead();
}

void EditorApp::ExtendSelection(KeyType key) {
    if (!state_.selection().active) {
        state_.selection().active = true;
        state_.selection().extend_on_cursor_move = false;
        state_.selection().anchor = state_.fileCursor();
        state_.selection().head = state_.fileCursor();
    }
    switch (key) {
        case KeyType::ArrowLeft:
            CursorController::moveLeft(state_.fileCursor(), state_.fileBuffer());
            break;
        case KeyType::ArrowRight:
            CursorController::moveRight(state_.fileCursor(), state_.fileBuffer());
            break;
        case KeyType::ArrowUp:
            CursorController::moveUp(state_.fileCursor(), state_.fileBuffer());
            break;
        case KeyType::ArrowDown:
            CursorController::moveDown(state_.fileCursor(), state_.fileBuffer());
            break;
        default:
            break;
    }
    state_.selection().head = state_.fileCursor();
}

void EditorApp::ExtendSelectionToLineBoundary(KeyType key) {
    if (!state_.selection().active) {
        state_.selection().active = true;
        state_.selection().extend_on_cursor_move = false;
        state_.selection().anchor = state_.fileCursor();
        state_.selection().head = state_.fileCursor();
    }

    if (key == KeyType::Home) {
        CursorController::moveHome(state_.fileCursor());
    } else if (key == KeyType::End) {
        CursorController::moveEnd(state_.fileCursor(), state_.fileBuffer());
    }
    state_.selection().head = state_.fileCursor();
}

void EditorApp::UpdateSelectionHead() {
    if (state_.activeView() != ViewKind::File || !state_.selection().active) {
        return;
    }
    if (state_.selection().extend_on_cursor_move) {
        state_.selection().head = state_.fileCursor();
    } else {
        state_.clearSelection();
    }
}

bool EditorApp::ExecuteCommand(const Command& command) {
    switch (command.type) {
        case CommandType::Open:
            return OpenFile(command.argument);
        case CommandType::Write:
            SaveFile();
            return true;
        case CommandType::Quit:
            if (state_.fileBuffer().dirty() && !pending_quit_confirm_) {
                pending_quit_confirm_ = true;
                state_.setStatus("Unsaved changes. Press :quit again to discard them.");
            } else {
                QuitEditor();
            }
            return true;
        case CommandType::Build:
            RunBuild();
            return true;
        case CommandType::Find:
            return FindText(command.argument);
        case CommandType::Goto:
            return GotoLine(command.argument);
        case CommandType::EnableAi:
            return EnableAiProvider(command.argument);
        case CommandType::AiExplain:
            RunAiRequest(AiRequestKind::Explain, "Explain this code.");
            return true;
        case CommandType::AiFix:
            RunAiRequest(AiRequestKind::Fix, "Fix bugs in this code and keep the patch minimal.");
            return true;
        case CommandType::AiRefactor:
            RunAiRequest(AiRequestKind::Refactor, "Refactor this code without changing behavior.");
            return true;
        case CommandType::AiError:
            RunAiRequest(AiRequestKind::ErrorExplain, "Explain the latest build failure.");
            return true;
        case CommandType::PatchAccept:
        case CommandType::PatchReject:
        case CommandType::PatchAcceptAll:
        case CommandType::PatchRejectAll:
            HandlePatchAction(command.type);
            return true;
        case CommandType::Invalid:
            return false;
    }
    return false;
}

bool EditorApp::OpenFile(const std::string& path) {
    std::string error;
    Buffer buffer = LoadFileBuffer(std::filesystem::path(path), &error);
    if (!error.empty()) {
        state_.setStatus(error);
        return false;
    }
    state_.setFileBuffer(std::move(buffer));
    state_.clearCompletionSession();
    state_.clearDiagnostics();
    language_server_document_synced_ = false;
    diagnostics_auto_suppressed_ = false;
    completion_auto_suppressed_ = false;
    state_.setStatus("Opened " + path + ".");
    return true;
}

bool EditorApp::EnableAiProvider(const std::string& provider) {
    if (active_ai_request_.has_value() || ai_client_->HasActiveRequest()) {
        state_.setStatus("Wait for the current AI request to finish before switching providers.", 60);
        return false;
    }

    std::unique_ptr<IAiClient> next_client;
    std::string provider_name;
    if (provider == "openai") {
        next_client = std::make_unique<OpenAiClient>();
        provider_name = "OPENAI";
    } else if (provider == "codex") {
        next_client = std::make_unique<CodexClient>();
        provider_name = "CODEX";
    } else {
        state_.setStatus("Usage: :enable-ai <openai|codex>");
        return false;
    }

    ai_client_->Shutdown();
    ai_client_ = std::move(next_client);
    ai_request_backgrounded_ = false;
    active_ai_request_.reset();
    state_.clearAiRequestState();
    state_.setAiRateLimits(std::nullopt);
    state_.setAiProviderName(provider_name);
    state_.setStatus("AI enabled via " + provider_name + ".");
    return true;
}

bool EditorApp::FindText(const std::string& query) {
    if (query.empty()) {
        state_.setStatus("Usage: :find <text>");
        return false;
    }

    const Buffer& buffer = state_.fileBuffer();
    if (buffer.lineCount() == 0) {
        state_.setStatus("No match for \"" + query + "\".");
        return false;
    }

    const Cursor cursor = state_.fileCursor();
    const size_t start_row = std::min(cursor.row, buffer.lineCount() - 1);
    const size_t start_col = std::min(cursor.col + 1, buffer.line(start_row).size());

    auto find_from = [&](size_t row, size_t col) -> std::optional<Cursor> {
        const size_t found = buffer.line(row).find(query, col);
        if (found == std::string::npos) {
            return std::nullopt;
        }
        return Cursor{row, found};
    };

    std::optional<Cursor> match;
    for (size_t row = start_row; row < buffer.lineCount(); ++row) {
        match = find_from(row, row == start_row ? start_col : 0);
        if (match.has_value()) {
            break;
        }
    }
    if (!match.has_value()) {
        for (size_t row = 0; row <= start_row && row < buffer.lineCount(); ++row) {
            match = find_from(row, 0);
            if (match.has_value()) {
                break;
            }
        }
    }

    if (!match.has_value()) {
        state_.setStatus("No match for \"" + query + "\".");
        return false;
    }

    state_.setActiveView(ViewKind::File);
    state_.fileCursor() = *match;
    state_.selection().active = true;
    state_.selection().extend_on_cursor_move = false;
    state_.selection().anchor = *match;
    state_.selection().head = Cursor{match->row, match->col + query.size()};
    state_.setStatus("Found \"" + query + "\" at line " + std::to_string(match->row + 1) + ".");
    return true;
}

bool EditorApp::GotoLine(const std::string& line_text) {
    const std::optional<size_t> line_number = ParseOneBasedLineNumber(line_text);
    if (!line_number.has_value()) {
        state_.setStatus("Usage: :goto <line>");
        return false;
    }
    if (*line_number > state_.fileBuffer().lineCount()) {
        state_.setStatus("Line out of range. File has " + std::to_string(state_.fileBuffer().lineCount()) + " lines.");
        return false;
    }

    state_.setActiveView(ViewKind::File);
    state_.fileCursor().row = *line_number - 1;
    state_.fileCursor().col = 0;
    state_.clearSelection();
    state_.setStatus("Jumped to line " + std::to_string(*line_number) + ".");
    return true;
}

void EditorApp::StartCommandPrompt() {
    command_mode_ = true;
    command_input_.clear();
    state_.setStatus("Command mode.");
}

void EditorApp::StartFindPrompt() {
    command_mode_ = true;
    command_input_ = "find ";
    state_.setStatus("Find in file.");
}

void EditorApp::StartFilePicker() {
    file_picker_root_ = FilePickerRoot();
    file_picker_files_ = DiscoverFilePickerFiles();
    file_picker_query_.clear();
    file_picker_mode_ = true;
    command_mode_ = false;
    state_.buildBuffer().setName("File Picker");
    state_.setActiveView(ViewKind::BuildOutput);
    RefreshFilePickerMatches();
    state_.setStatus("Type to filter, arrows select, Enter opens, Esc cancels.", 60);
}

std::filesystem::path EditorApp::FilePickerRoot() const {
    const std::filesystem::path active_path =
        state_.fileBuffer().path().has_value() ? AbsolutePath(*state_.fileBuffer().path()) : std::filesystem::current_path();
    if (const std::optional<std::filesystem::path> git_root = FindContainingGitRoot(active_path)) {
        return *git_root;
    }
    if (std::filesystem::is_directory(active_path)) {
        return active_path;
    }
    if (active_path.has_parent_path()) {
        return active_path.parent_path();
    }
    return std::filesystem::current_path();
}

std::vector<std::string> EditorApp::DiscoverFilePickerFiles() const {
    std::vector<std::string> files;
    const std::filesystem::path root = file_picker_root_.empty() ? FilePickerRoot() : file_picker_root_;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry entry = *iterator;
        std::error_code entry_error;
        if (entry.is_directory(entry_error) && IsSkippedPickerDirectory(entry.path())) {
            iterator.disable_recursion_pending();
        } else if (entry.is_regular_file(entry_error)) {
            std::error_code relative_error;
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, relative_error);
            if (!relative_error) {
                files.push_back(relative.generic_string());
            }
        }
        iterator.increment(error);
    }
    std::sort(files.begin(), files.end());
    return files;
}

void EditorApp::RefreshFilePickerMatches() {
    file_picker_matches_.clear();
    for (const std::string& path : file_picker_files_) {
        if (IsFilePickerMatch(path, file_picker_query_)) {
            file_picker_matches_.push_back(path);
        }
        if (file_picker_matches_.size() >= 200) {
            break;
        }
    }
    file_picker_selected_ = std::min(file_picker_selected_,
                                     file_picker_matches_.empty() ? size_t{0} : file_picker_matches_.size() - 1);
    std::vector<std::string> lines;
    lines.push_back("Root: " + file_picker_root_.string());
    if (file_picker_matches_.empty()) {
        lines.push_back("No files match \"" + file_picker_query_ + "\".");
    } else {
        lines.reserve(file_picker_matches_.size() + 1);
        for (const std::string& path : file_picker_matches_) {
            lines.push_back(ScaffoldedPickerPath(path));
        }
    }
    state_.buildBuffer().setName("File Picker");
    state_.buildBuffer().setLines(std::move(lines), false);
    state_.buildBuffer().clearDirty();
    state_.viewport(ViewKind::BuildOutput).cursor.row = file_picker_selected_ + 1;
    state_.viewport(ViewKind::BuildOutput).cursor.col = 0;
}

void EditorApp::SaveFile() {
    std::string error;
    if (state_.fileBuffer().save(&error)) {
        state_.setStatus("Saved " + state_.fileBuffer().name() + ".");
    } else {
        state_.setStatus(error.empty() ? "Save failed." : error);
    }
}

void EditorApp::CopySelectionOrLine() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Copy only works in the file buffer.");
        return;
    }

    if (HasSelection(state_.selection())) {
        state_.setClipboardText(ExtractSelection(state_.fileBuffer(), state_.selection()));
        state_.setStatus("Copied selection.");
        return;
    }

    const SelectionRange range = CurrentLineRange(state_.fileBuffer(), state_.fileCursor());
    state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
    state_.setStatus("Copied line.");
}

void EditorApp::CutSelectionOrLine() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Cut only works in the file buffer.");
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        state_.setStatus("Buffer is read-only.");
        return;
    }

    Cursor& cursor = state_.fileCursor();
    state_.BeginFileEdit();
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
        state_.fileBuffer().deleteRange(cursor, range.start, range.end);
        state_.clearSelection();
        if (state_.CommitFileEdit()) {
            NotifyCompletionDocumentChanged();
            InvalidatePatchSessionForManualFileEdit();
            state_.setStatus("Cut selection.");
        }
        return;
    }

    const SelectionRange range = CurrentLineRange(state_.fileBuffer(), cursor);
    state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
    state_.fileBuffer().deleteRange(cursor, range.start, range.end);
    cursor.col = 0;
    CursorController::clamp(cursor, state_.fileBuffer());
    state_.clearSelection();
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
        state_.setStatus("Cut line.");
    }
}

void EditorApp::PasteClipboard() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Paste only works in the file buffer.");
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        state_.setStatus("Buffer is read-only.");
        return;
    }
    if (!state_.hasClipboardText()) {
        state_.setStatus("Clipboard is empty.");
        return;
    }

    Cursor& cursor = state_.fileCursor();
    state_.BeginFileEdit();
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        state_.fileBuffer().replaceRange(cursor, range.start, range.end, state_.clipboardText());
    } else {
        state_.fileBuffer().insertText(cursor, state_.clipboardText());
    }

    state_.clearSelection();
    CursorController::clamp(cursor, state_.fileBuffer());
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
        state_.setStatus("Pasted clipboard.");
    }
}

void EditorApp::ToggleLineComment() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Comments only work in the file buffer.");
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        state_.setStatus("Buffer is read-only.");
        return;
    }

    const std::optional<std::string_view> comment_prefix = LineCommentPrefix(state_.fileBuffer().languageId());
    if (!comment_prefix.has_value()) {
        state_.setStatus("Line comments are not available for " + state_.fileBuffer().guessLanguage() + ".");
        return;
    }

    size_t start_row = state_.fileCursor().row;
    size_t end_row = state_.fileCursor().row;
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        start_row = range.start.row;
        end_row = range.end.row;
        if (range.end.col == 0 && range.end.row > range.start.row) {
            end_row = range.end.row - 1;
        }
    }

    state_.BeginFileEdit();
    const LineCommentToggleResult result =
        state_.fileBuffer().toggleLineComments(start_row, end_row, *comment_prefix);
    CursorController::clamp(state_.fileCursor(), state_.fileBuffer());
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
    }

    switch (result) {
        case LineCommentToggleResult::Commented:
            state_.setStatus("Commented selected line" + std::string(start_row == end_row ? "." : "s."));
            return;
        case LineCommentToggleResult::Uncommented:
            state_.setStatus("Uncommented selected line" + std::string(start_row == end_row ? "." : "s."));
            return;
        case LineCommentToggleResult::Unchanged:
            state_.setStatus("No lines changed.");
            return;
    }
}

void EditorApp::ToggleGitPreviousLines() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Git change peek only works in the file buffer.");
        return;
    }

    const size_t row = state_.fileCursor().row;
    const GitLineStatus git_status =
        LoadGitLineStatus(state_.fileBuffer().path().value_or(std::filesystem::path()), state_.fileBuffer().lineCount());
    if (!git_status.available || row >= git_status.lines.size() || git_status.lines[row].previous_lines.empty()) {
        state_.setStatus("No previous lines at this row.");
        return;
    }

    const bool was_expanded = state_.isGitChangePeekExpanded(row);
    state_.toggleGitChangePeekExpansion(row);
    state_.setStatus((was_expanded ? "Hid " : "Showing ") +
                     std::to_string(git_status.lines[row].previous_lines.size()) +
                     " previous line" + (git_status.lines[row].previous_lines.size() == 1 ? "." : "s."));
}

bool EditorApp::DeleteSelectionIfActive() {
    if (!HasSelection(state_.selection())) {
        return false;
    }

    Cursor& cursor = state_.fileCursor();
    const SelectionRange range = NormalizeSelection(state_.selection());
    state_.BeginFileEdit();
    state_.fileBuffer().deleteRange(cursor, range.start, range.end);
    state_.clearSelection();
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
    }
    state_.setStatus("Deleted selection.");
    return true;
}

bool EditorApp::DeleteAutoclosedPairIfActive() {
    Cursor& cursor = state_.fileCursor();
    if (cursor.row >= state_.fileBuffer().lineCount() || cursor.col == 0) {
        return false;
    }

    const std::string& line = state_.fileBuffer().line(cursor.row);
    if (cursor.col >= line.size() || !IsAutoclosedPair(line[cursor.col - 1], line[cursor.col])) {
        return false;
    }

    state_.BeginFileEdit();
    state_.fileBuffer().deleteRange(cursor, {cursor.row, cursor.col - 1}, {cursor.row, cursor.col + 1});
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
    }
    return true;
}

void EditorApp::InsertCharacter(char ch) {
    if (const std::optional<char> closing_bracket = ClosingBracketFor(ch); closing_bracket.has_value()) {
        Cursor& cursor = state_.fileCursor();
        state_.BeginFileEdit();
        if (HasSelection(state_.selection())) {
            const SelectionRange range = NormalizeSelection(state_.selection());
            std::string wrapped;
            wrapped.push_back(ch);
            wrapped += ExtractRange(state_.fileBuffer(), range);
            wrapped.push_back(*closing_bracket);
            state_.fileBuffer().replaceRange(cursor, range.start, range.end, wrapped);
            state_.clearSelection();
        } else {
            state_.fileBuffer().insertPairedChars(cursor, ch, *closing_bracket);
        }
        CursorController::clamp(cursor, state_.fileBuffer());
        if (state_.CommitFileEdit()) {
            NotifyCompletionDocumentChanged();
            InvalidatePatchSessionForManualFileEdit();
        }
        return;
    }

    if (!HasSelection(state_.selection())) {
        Cursor& cursor = state_.fileCursor();
        if (cursor.row < state_.fileBuffer().lineCount()) {
            const std::string& line = state_.fileBuffer().line(cursor.row);
            if (cursor.col < line.size() && IsAutoclosedPair(cursor.col == 0 ? '\0' : line[cursor.col - 1], ch) &&
                line[cursor.col] == ch) {
                ++cursor.col;
                return;
            }
        }
    }

    state_.BeginFileEdit();
    state_.fileBuffer().insertChar(state_.fileCursor(), ch);
    ++state_.fileCursor().col;
    UpdateSelectionHead();
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
        if (IsCompletionAutoTrigger(state_.fileBuffer(), state_.fileCursor())) {
            RequestCompletion(true);
        }
    }
}

void EditorApp::UndoFileEdit() {
    if (state_.activeView() != ViewKind::File && state_.activeView() != ViewKind::PatchPreview) {
        state_.setStatus("Undo only works on the file buffer.");
        return;
    }
    if (!state_.UndoFileEdit()) {
        state_.setStatus("Nothing to undo.");
        return;
    }

    state_.setPatchSession(std::nullopt);
    state_.clearInlineAiSession();
    state_.setActiveView(ViewKind::File);
    NotifyCompletionDocumentChanged();
    state_.setStatus("Undid last file change.");
}

void EditorApp::RedoFileEdit() {
    if (state_.activeView() != ViewKind::File && state_.activeView() != ViewKind::PatchPreview) {
        state_.setStatus("Redo only works on the file buffer.");
        return;
    }
    if (!state_.RedoFileEdit()) {
        state_.setStatus("Nothing to redo.");
        return;
    }

    state_.setPatchSession(std::nullopt);
    state_.clearInlineAiSession();
    state_.setActiveView(ViewKind::File);
    NotifyCompletionDocumentChanged();
    state_.setStatus("Redid last file change.");
}

void EditorApp::RunBuild() {
    state_.buildBuffer().setName("Build Output");
    const BuildResult result = RunBuildCommand(state_.buildCommand());
    state_.setLastBuild(result);

    std::ostringstream output;
    output << "$ " << state_.buildCommand() << "\n";
    if (!result.output.empty()) {
        output << result.output;
        if (!result.output.empty() && result.output.back() != '\n') {
            output << '\n';
        }
    }
    output << "[exit " << result.exit_code << "]";

    state_.setBuildOutput(output.str());
    state_.setActiveView(ViewKind::BuildOutput);
    state_.setStatus("Build finished with exit code " + std::to_string(result.exit_code) + ".");
}

void EditorApp::SubmitInlineAiFollowUp() {
    if (!state_.inlineAiSession().has_value()) {
        return;
    }
    if (state_.aiProviderName() == "OFF") {
        state_.setStatus("AI is disabled. Use :enable-ai openai or :enable-ai codex to enable it.", 60);
        return;
    }
    if (active_ai_request_.has_value() || ai_client_->HasActiveRequest()) {
        state_.setStatus("AI request already in progress.", 60);
        return;
    }

    InlineAiSession session = *state_.inlineAiSession();
    const bool has_question =
        std::any_of(session.input_text.begin(), session.input_text.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        });
    if (!has_question) {
        state_.setStatus("Type a follow-up question first.");
        return;
    }

    const std::string previous_text = session.text;
    const std::string question = session.input_text;
    std::string response_prefix = previous_text;
    if (!response_prefix.empty() && response_prefix.back() != '\n') {
        response_prefix += '\n';
    }
    response_prefix += "\nYou: " + question + "\n\nAI: ";

    session.text = response_prefix;
    session.input_text.clear();
    session.input_cursor_col = 0;
    session.scroll_row = std::numeric_limits<size_t>::max();
    session.cursor_body_row = std::numeric_limits<size_t>::max();
    session.waiting = true;
    session.failed = false;
    session.state_label = AiRequestStateLabel(AiRequestState::Connecting);
    state_.setInlineAiSession(session);

    const std::string instruction =
        "This is a follow-up question about your previous explanation.\n\nPrevious explanation:\n" +
        previous_text + "\n\nUser follow-up:\n" + question;
    const AiRequest request = BuildAiRequest(AiRequestKind::Explain, instruction);

    active_ai_request_ = ActiveAiRequest{.kind = AiRequestKind::Explain,
                                         .label = "Answering follow-up",
                                         .streamed_text = response_prefix,
                                         .response_prefix = response_prefix,
                                         .inline_viewer = true};
    ai_request_backgrounded_ = false;
    ai_loading_frame_ = 0;
    next_ai_loading_tick_ = std::chrono::steady_clock::now();
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Connecting));
    state_.setStatus("Answering follow-up via " + state_.aiProviderName() + "...", 3600);

    std::string error;
    if (!ai_client_->StartRequest(request, &error)) {
        active_ai_request_.reset();
        session.waiting = false;
        session.failed = true;
        session.state_label = AiRequestStateLabel(AiRequestState::Failed);
        session.text = response_prefix + "\n[AI request failed]\n" +
                       (error.empty() ? std::string("AI request failed.") : error);
        state_.setInlineAiSession(session);
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
        state_.setStatus(AiFailureStatus(error, false), 60);
    }
}

AiRequest EditorApp::BuildAiRequest(AiRequestKind kind, const std::string& instruction) const {
    const Buffer& file = state_.fileBuffer();
    const size_t start_row = SelectionStartRow(state_);
    const size_t end_row = SelectionEndRow(state_);
    const size_t before_start = (start_row > kContextLines) ? start_row - kContextLines : 0;
    const size_t after_end = std::min(file.lineCount() - 1, end_row + kContextLines);

    AiRequest request;
    request.kind = kind;
    request.file_path = file.path().has_value() ? file.path()->string() : file.name();
    request.language = file.guessLanguage();
    request.cursor = state_.fileCursor();
    request.selected_text = state_.selectedOrCurrentText();
    request.context_before = (start_row > 0) ? JoinRange(file, before_start, start_row - 1) : "";
    request.context_after =
        (end_row + 1 < file.lineCount()) ? JoinRange(file, end_row + 1, after_end) : "";
    request.user_instruction = instruction;
    request.build_command = state_.buildCommand();
    request.build_output = state_.lastBuild().has_value() ? state_.lastBuild()->output : "";
    return request;
}

void EditorApp::RunAiRequest(AiRequestKind kind, std::string instruction) {
    if (state_.aiProviderName() == "OFF") {
        state_.setStatus("AI is disabled. Use :enable-ai openai or :enable-ai codex to enable it.", 60);
        return;
    }
    if (active_ai_request_.has_value() || ai_client_->HasActiveRequest()) {
        state_.setStatus("AI request already in progress.", 60);
        return;
    }
    if (kind == AiRequestKind::ErrorExplain && !state_.lastBuild().has_value()) {
        state_.setStatus("Run :build before :ai error.");
        return;
    }

    const AiRequest request = BuildAiRequest(kind, instruction);
    std::string action = "AI request";
    switch (kind) {
        case AiRequestKind::Explain:
            action = "Explaining selection";
            break;
        case AiRequestKind::Fix:
            action = "Generating fix patch";
            break;
        case AiRequestKind::Refactor:
            action = "Generating refactor patch";
            break;
        case AiRequestKind::ErrorExplain:
            action = "Explaining build error";
            break;
    }

    const bool use_inline_viewer = kind == AiRequestKind::Explain;
    state_.setPatchSession(std::nullopt);
    if (use_inline_viewer) {
        state_.setActiveView(ViewKind::File);
        const size_t anchor_row =
            state_.fileBuffer().lineCount() == 0
                ? 0
                : std::min(SelectionEndRow(state_), state_.fileBuffer().lineCount() - 1);
        state_.setInlineAiSession(InlineAiSession{
            .anchor_row = anchor_row,
            .title = "AI Explain",
            .provider_name = state_.aiProviderName(),
            .state_label = AiRequestStateLabel(AiRequestState::Connecting),
            .waiting = true,
        });
    } else {
        state_.clearInlineAiSession();
        state_.setActiveView(ViewKind::AiScratch);
    }
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Connecting));
    state_.setStatus(action + " via " + state_.aiProviderName() + "...", 3600);
    active_ai_request_ = ActiveAiRequest{.kind = kind,
                                         .label = action,
                                         .streamed_text = "",
                                         .inline_viewer = use_inline_viewer};
    ai_request_backgrounded_ = false;
    ai_loading_frame_ = 0;
    next_ai_loading_tick_ = std::chrono::steady_clock::now();
    if (use_inline_viewer) {
        RenderActiveInlineAi();
    } else {
        RenderActiveAiScratch();
    }

    std::string error;
    if (!ai_client_->StartRequest(request, &error)) {
        active_ai_request_.reset();
        ai_request_backgrounded_ = false;
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
        if (use_inline_viewer) {
            ShowInlineAiText(error.empty() ? "AI request failed." : error,
                             AiRequestStateLabel(AiRequestState::Failed),
                             false,
                             true);
        } else {
            ShowAiText(error.empty() ? "AI request failed." : error, true);
        }
        state_.setStatus(AiFailureStatus(error, false), 60);
    }
}

void EditorApp::PollAiRequest() {
    UpdateAiLoadingView();

    for (const AiEvent& event : ai_client_->PollEvents()) {
        switch (event.kind) {
            case AiEventKind::StateChanged:
                if (event.state == AiRequestState::Connecting || event.state == AiRequestState::Streaming) {
                    const std::string label = AiRequestStateLabel(event.state);
                    state_.setAiRequestState(label);
                    if (active_ai_request_.has_value() && active_ai_request_->inline_viewer &&
                        state_.inlineAiSession().has_value()) {
                        state_.inlineAiSession()->state_label = label;
                    }
                }
                break;
            case AiEventKind::TextDelta:
                if (active_ai_request_.has_value() && !event.text_delta.empty()) {
                    active_ai_request_->streamed_text += event.text_delta;
                    RenderActiveAiScratch();
                }
                break;
            case AiEventKind::RateLimitsUpdated:
                state_.setAiRateLimits(event.rate_limits);
                break;
            case AiEventKind::Completed:
                HandleAiResponse(event.response);
                active_ai_request_.reset();
                break;
            case AiEventKind::Error:
                HandleAiError(event.error_message);
                break;
        }
    }
}

void EditorApp::PollCompletionRequest() {
    for (const CompletionEvent& event : language_server_client_.PollEvents()) {
        if (event.kind == CompletionEventKind::Diagnostics) {
            state_.setDiagnostics(event.diagnostics);
            continue;
        }

        if (!state_.completionSession().active || !state_.completionSession().waiting) {
            if (event.kind == CompletionEventKind::Error && event.request_id == 0) {
                state_.clearDiagnostics();
            }
            continue;
        }

        CompletionSession session = state_.completionSession();
        if (event.request_id != 0 && event.request_id != session.request_id) {
            continue;
        }

        session.waiting = false;
        if (event.kind == CompletionEventKind::Error) {
            state_.clearCompletionSession();
            state_.setStatus(event.error_message.empty() ? "Completion request failed." : event.error_message, 10);
            return;
        }

        session.items = event.items;
        session.selected = 0;
        if (session.items.empty()) {
            state_.clearCompletionSession();
            state_.setStatus("No completions.", 3);
            return;
        }
        session.message.clear();
        state_.setCompletionSession(std::move(session));
    }
}

void EditorApp::RequestCompletion(bool automatic) {
    if (automatic && completion_auto_suppressed_) {
        return;
    }
    if (!automatic) {
        completion_auto_suppressed_ = false;
    }

    if (state_.activeView() != ViewKind::File) {
        if (!automatic) {
            state_.setStatus("Completion only works in the file buffer.");
        }
        return;
    }
    if (!IsCompletionLanguage(state_.fileBuffer().languageId())) {
        state_.clearDiagnostics();
        if (!automatic) {
            state_.setStatus("IntelliSense is only enabled for C/C++ and Python right now.");
        }
        return;
    }

    Cursor cursor = state_.fileCursor();
    CursorController::clamp(cursor, state_.fileBuffer());
    const Cursor replace_start = CompletionPrefixStart(state_.fileBuffer(), cursor);

    std::string error;
    if (!language_server_client_.Start(state_.fileBuffer(), &error)) {
        state_.clearCompletionSession();
        state_.clearDiagnostics();
        language_server_document_synced_ = false;
        if (automatic) {
            completion_auto_suppressed_ = true;
            diagnostics_auto_suppressed_ = true;
        }
        state_.setStatus(error.empty() ? "Unable to start language server." : error, 10);
        return;
    }
    if (!language_server_client_.SyncDocument(state_.fileBuffer(), &error)) {
        state_.clearCompletionSession();
        state_.clearDiagnostics();
        language_server_document_synced_ = false;
        if (automatic) {
            completion_auto_suppressed_ = true;
            diagnostics_auto_suppressed_ = true;
        }
        state_.setStatus(error.empty() ? "Unable to sync file with language server." : error, 10);
        return;
    }
    language_server_document_synced_ = true;
    diagnostics_auto_suppressed_ = false;

    const std::optional<int> request_id = language_server_client_.RequestCompletion(state_.fileBuffer(), cursor, &error);
    if (!request_id.has_value()) {
        state_.clearCompletionSession();
        if (automatic) {
            completion_auto_suppressed_ = true;
        }
        state_.setStatus(error.empty() ? "Unable to request completions." : error, 10);
        return;
    }

    state_.setCompletionSession({
        .active = true,
        .waiting = true,
        .request_id = *request_id,
        .replace_start = replace_start,
        .replace_end = cursor,
        .message = "Completing...",
    });
    if (!automatic) {
        state_.setStatus("Completing with " + language_server_client_.displayName() + "...", 3);
    }
}

void EditorApp::CloseCompletion() { state_.clearCompletionSession(); }

bool EditorApp::HandleCompletionKey(const KeyPress& key) {
    CompletionSession session = state_.completionSession();
    if (!session.active) {
        return false;
    }

    switch (key.type) {
        case KeyType::Escape:
            CloseCompletion();
            state_.setStatus("Completion cancelled.");
            return true;
        case KeyType::ArrowUp:
            if (!session.items.empty() && session.selected > 0) {
                --session.selected;
                state_.setCompletionSession(std::move(session));
            }
            return true;
        case KeyType::ArrowDown:
            if (!session.items.empty() && session.selected + 1 < session.items.size()) {
                ++session.selected;
                state_.setCompletionSession(std::move(session));
            }
            return true;
        case KeyType::Enter:
        case KeyType::Tab:
            AcceptCompletion();
            return true;
        case KeyType::Character:
            if (!key.ctrl) {
                CloseCompletion();
            }
            return false;
        case KeyType::Backspace:
        case KeyType::DeleteKey:
        case KeyType::Home:
        case KeyType::End:
        case KeyType::PageUp:
        case KeyType::PageDown:
        case KeyType::ArrowLeft:
        case KeyType::ArrowRight:
            CloseCompletion();
            return false;
        case KeyType::Unknown:
            return false;
    }
    return false;
}

void EditorApp::AcceptCompletion() {
    CompletionSession session = state_.completionSession();
    if (!session.active || session.waiting || session.items.empty() || session.selected >= session.items.size()) {
        CloseCompletion();
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        CloseCompletion();
        state_.setStatus("Buffer is read-only.");
        return;
    }

    state_.BeginFileEdit();
    const CompletionItem item = session.items[session.selected];
    if (!ApplyCompletionItem(state_.fileBuffer(),
                             state_.fileCursor(),
                             item,
                             session.replace_start,
                             session.replace_end)) {
        state_.CommitFileEdit();
        CloseCompletion();
        state_.setStatus("Unable to apply completion.");
        return;
    }

    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
    }
    state_.setStatus("Completed " + item.label + ".");
}

void EditorApp::EnsureRealtimeDiagnostics() {
    if (state_.activeView() != ViewKind::File || !IsCompletionLanguage(state_.fileBuffer().languageId())) {
        state_.clearDiagnostics();
        language_server_document_synced_ = false;
        return;
    }
    if (language_server_document_synced_ || diagnostics_auto_suppressed_) {
        return;
    }

    std::string error;
    if (!language_server_client_.Start(state_.fileBuffer(), &error)) {
        state_.clearDiagnostics();
        diagnostics_auto_suppressed_ = true;
        return;
    }
    if (!language_server_client_.SyncDocument(state_.fileBuffer(), &error)) {
        state_.clearDiagnostics();
        diagnostics_auto_suppressed_ = true;
        return;
    }

    language_server_document_synced_ = true;
}

void EditorApp::NotifyCompletionDocumentChanged() {
    language_server_document_synced_ = false;
    if (!IsCompletionLanguage(state_.fileBuffer().languageId())) {
        state_.clearDiagnostics();
        return;
    }
    diagnostics_auto_suppressed_ = false;
    if (!language_server_client_.IsStartedFor(state_.fileBuffer().languageId())) {
        return;
    }
    std::string error;
    if (language_server_client_.SyncDocument(state_.fileBuffer(), &error)) {
        language_server_document_synced_ = true;
    } else {
        state_.clearDiagnostics();
        diagnostics_auto_suppressed_ = true;
    }
}

void EditorApp::UpdateAiLoadingView() {
    if (!active_ai_request_.has_value()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_ai_loading_tick_) {
        return;
    }

    RenderActiveAiScratch();
    next_ai_loading_tick_ = now + kAiLoadingTick;
    ++ai_loading_frame_;
}

void EditorApp::RenderActiveAiScratch() {
    if (!active_ai_request_.has_value()) {
        return;
    }
    if (active_ai_request_->inline_viewer) {
        RenderActiveInlineAi();
        return;
    }

    static constexpr const char* kFrames[] = {"|", "/", "-", "\\"};
    const std::string header =
        active_ai_request_->label + " via " + state_.aiProviderName() + " " + kFrames[ai_loading_frame_ % 4];

    std::string text = header + "\n\n";
    if (active_ai_request_->streamed_text.empty()) {
        text += "Waiting for response.\nPress Esc to return to the file buffer.";
    } else {
        text += active_ai_request_->streamed_text;
    }
    ShowAiText(text, false);
}

void EditorApp::RenderActiveInlineAi() {
    if (!active_ai_request_.has_value() || !active_ai_request_->inline_viewer ||
        !state_.inlineAiSession().has_value()) {
        return;
    }

    static constexpr const char* kFrames[] = {"|", "/", "-", "\\"};
    if (active_ai_request_->streamed_text.empty()) {
        ShowInlineAiText(std::string("Waiting for response ") + kFrames[ai_loading_frame_ % 4],
                         state_.aiRequestState().empty()
                             ? AiRequestStateLabel(AiRequestState::Connecting)
                             : state_.aiRequestState(),
                         true,
                         false);
        return;
    }

    ShowInlineAiText(active_ai_request_->streamed_text,
                     state_.aiRequestState().empty()
                         ? AiRequestStateLabel(AiRequestState::Streaming)
                         : state_.aiRequestState(),
                     true,
                     false);
}

void EditorApp::HandleAiResponse(const AiResponse& response) {
    if (response.kind == AiResponseKind::Error) {
        HandleAiError(response.error_message);
        return;
    }

    const bool inline_explain = active_ai_request_.has_value() && active_ai_request_->inline_viewer &&
                                active_ai_request_->kind == AiRequestKind::Explain;
    if (inline_explain) {
        const std::string inline_text = active_ai_request_->response_prefix + response.raw_text;
        state_.setAiText(inline_text);
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Complete));
        if (!ai_request_backgrounded_ && state_.inlineAiSession().has_value()) {
            ShowInlineAiText(inline_text, AiRequestStateLabel(AiRequestState::Complete), false, false);
            state_.setStatus("AI explanation complete. Follow-up prompt is ready.", 60);
        } else {
            state_.setStatus("AI explanation complete. Press Alt+E to reopen AI scratch.", 60);
        }
        ai_request_backgrounded_ = false;
        return;
    }

    if (response.diff_text.has_value()) {
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::ParsingPatch));
        const PatchSet patch = ParseUnifiedDiff(*response.diff_text);
        if (!patch.valid()) {
            ShowAiText(response.raw_text, !ai_request_backgrounded_);
            state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
            state_.setStatus(ai_request_backgrounded_ ? "AI returned a malformed patch. Press Alt+E to reopen AI scratch."
                                                      : "AI returned a malformed patch.",
                             60);
            ai_request_backgrounded_ = false;
            return;
        }
        if (!PatchTargetsBuffer(patch, state_.fileBuffer())) {
            ShowAiText(response.raw_text, !ai_request_backgrounded_);
            state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
            state_.setStatus(ai_request_backgrounded_
                                 ? "AI patch does not target the current file. Press Alt+E to reopen AI scratch."
                                 : "AI patch does not target the current file.",
                             60);
            ai_request_backgrounded_ = false;
            return;
        }

        state_.setAiText(response.raw_text);
        state_.setPatchSession(CreatePatchSession(patch, state_.fileBuffer()));
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Complete));
        if (ai_request_backgrounded_) {
            state_.setStatus("AI patch ready. Press Alt+P to review.", 60);
        } else {
            state_.setActiveView(ViewKind::PatchPreview);
            state_.setStatus("AI patch ready for review.");
        }
        ai_request_backgrounded_ = false;
        return;
    }

    ShowAiText(response.raw_text, !ai_request_backgrounded_);
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Complete));
    state_.setStatus(ai_request_backgrounded_ ? "AI response complete. Press Alt+E to reopen AI scratch."
                                              : "AI response complete.",
                     60);
    ai_request_backgrounded_ = false;
}

void EditorApp::HandleAiError(const std::string& error_message) {
    if (active_ai_request_.has_value() && active_ai_request_->inline_viewer) {
        std::string text = active_ai_request_->streamed_text;
        if (!text.empty() && text.back() != '\n') {
            text += '\n';
        }
        if (!text.empty()) {
            text += '\n';
        }
        text += "[AI request failed]\n";
        text += error_message;
        state_.setAiText(text);
        if (!ai_request_backgrounded_ && state_.inlineAiSession().has_value()) {
            ShowInlineAiText(text, AiRequestStateLabel(AiRequestState::Failed), false, true);
        }
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
        state_.setStatus(AiFailureStatus(error_message, ai_request_backgrounded_), 60);
        active_ai_request_.reset();
        ai_request_backgrounded_ = false;
        return;
    }

    if (active_ai_request_.has_value() && !active_ai_request_->streamed_text.empty()) {
        std::string combined = active_ai_request_->streamed_text;
        if (!combined.empty() && combined.back() != '\n') {
            combined += '\n';
        }
        combined += "\n[AI request failed]\n";
        combined += error_message;
        ShowAiText(combined, !ai_request_backgrounded_);
    } else {
        ShowAiText(error_message, !ai_request_backgrounded_);
    }
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
    state_.setStatus(AiFailureStatus(error_message, ai_request_backgrounded_), 60);
    active_ai_request_.reset();
    ai_request_backgrounded_ = false;
}

void EditorApp::ShowAiText(const std::string& text, bool switch_to_ai_buffer) {
    state_.setAiText(text);
    if (switch_to_ai_buffer) {
        state_.setActiveView(ViewKind::AiScratch);
    }
}

void EditorApp::ShowInlineAiText(const std::string& text,
                                 std::string state_label,
                                 bool waiting,
                                 bool failed) {
    if (!state_.inlineAiSession().has_value()) {
        return;
    }

    InlineAiSession session = *state_.inlineAiSession();
    session.text = text;
    session.state_label = std::move(state_label);
    session.waiting = waiting;
    session.failed = failed;
    state_.setInlineAiSession(std::move(session));
}

void EditorApp::ReopenAiScratch() {
    state_.setActiveView(ViewKind::AiScratch);
    state_.setStatus("Opened AI scratch.");
}

void EditorApp::ReopenPatchPreview() {
    if (!state_.patchSession().has_value()) {
        state_.setStatus("No patch preview available.");
        return;
    }
    state_.setActiveView(ViewKind::PatchPreview);
    state_.setStatus("Opened patch preview.");
}

void EditorApp::InvalidatePatchSessionForManualFileEdit() {
    if (state_.patchSession().has_value()) {
        state_.setPatchSession(std::nullopt);
    }
    state_.clearInlineAiSession();
}

void EditorApp::QuitEditor() {
    ai_client_->Shutdown();
    language_server_client_.Shutdown();
    active_ai_request_.reset();
    ai_request_backgrounded_ = false;
    state_.clearAiRequestState();
    state_.clearCompletionSession();
    running_ = false;
}

void EditorApp::HandlePatchAction(CommandType command_type) {
    if (!state_.patchSession().has_value()) {
        state_.setStatus("No patch session.");
        return;
    }

    PatchApplyResult result;
    switch (command_type) {
        case CommandType::PatchAccept:
            state_.BeginFileEdit();
            result = AcceptCurrentHunk(state_.fileBuffer(), *state_.patchSession());
            if (state_.CommitFileEdit()) {
                NotifyCompletionDocumentChanged();
            }
            break;
        case CommandType::PatchReject:
            RejectCurrentHunk(*state_.patchSession());
            result = {.success = true, .message = "Hunk rejected."};
            break;
        case CommandType::PatchAcceptAll:
            state_.BeginFileEdit();
            result = AcceptAllHunks(state_.fileBuffer(), *state_.patchSession());
            if (state_.CommitFileEdit()) {
                NotifyCompletionDocumentChanged();
            }
            break;
        case CommandType::PatchRejectAll:
            RejectAllHunks(*state_.patchSession());
            result = {.success = true, .message = "All pending hunks rejected."};
            break;
        default:
            return;
    }

    state_.syncPatchPreview();
    state_.setActiveView(ViewKind::PatchPreview);
    state_.setStatus(result.message.empty() ? "Patch action completed." : result.message);
}

}  // namespace flowstate
