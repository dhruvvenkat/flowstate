#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ai/client.h"
#include "command.h"
#include "editor_state.h"
#include "intellisense/clangd_client.h"
#include "screen.h"
#include "terminal.h"

namespace flowstate {

class EditorApp {
  public:
    EditorApp(Buffer file_buffer,
              std::unique_ptr<IAiClient> ai_client,
              std::string build_command,
              std::string ai_provider_name,
              std::string cpp_standard);

    int Run();

  private:
    friend class EditorAppTestPeer;

    void RefreshScreen();
    void ScrollToCursor(int screen_rows, int screen_cols);
    void HandleNormalKey(const KeyPress& key);
    void HandleCommandKey(const KeyPress& key);
    void HandleFilePickerKey(const KeyPress& key);
    bool HandleMouseScroll(const KeyPress& key);
    bool HandleInlineAiKey(const KeyPress& key);
    bool EnterInlineAiBody(size_t cursor_body_row, size_t content_cols);
    bool LeaveInlineAiBody(size_t target_file_row);
    bool ScrollInlineAiBody(int delta, size_t content_cols);
    bool SetInlineAiCursorBodyRow(size_t cursor_body_row, size_t content_cols);
    void MoveCursor(KeyType key, size_t distance = 1);
    void ExtendSelection(KeyType key);
    void ExtendSelectionToLineBoundary(KeyType key);
    void UpdateSelectionHead();
    bool ExecuteCommand(const Command& command);
    bool OpenFile(const std::string& path);
    bool EnableAiProvider(const std::string& provider);
    bool FindText(const std::string& query);
    bool GotoLine(const std::string& line_text);
    void StartCommandPrompt();
    void StartFindPrompt();
    void StartFilePicker();
    void RefreshFilePickerMatches();
    std::filesystem::path FilePickerRoot() const;
    std::vector<std::string> DiscoverFilePickerFiles() const;
    void SaveFile();
    void CopySelectionOrLine();
    void CutSelectionOrLine();
    void PasteClipboard();
    void ToggleLineComment();
    void ToggleGitPreviousLines();
    bool DeleteSelectionIfActive();
    bool DeleteAutoclosedPairIfActive();
    bool IndentSelectedLines(bool outdent);
    bool UnindentCurrentLine();
    void InsertCharacter(char ch);
    void UndoFileEdit();
    void RedoFileEdit();
    void RunBuild();
    void RunAiRequest(AiRequestKind kind, std::string instruction);
    void SubmitInlineAiFollowUp();
    AiRequest BuildAiRequest(AiRequestKind kind, const std::string& instruction) const;
    void PollAiRequest();
    void PollCompletionRequest();
    void RequestCompletion(bool automatic);
    void CloseCompletion();
    void DismissCompletion();
    bool IsCompletionDismissedForPrefix(Cursor replace_start) const;
    bool HandleCompletionKey(const KeyPress& key);
    void AcceptCompletion();
    void EnsureRealtimeDiagnostics();
    void NotifyCompletionDocumentChanged();
    void UpdateAiLoadingView();
    void RenderActiveAiScratch();
    void RenderActiveInlineAi();
    void HandleAiResponse(const AiResponse& response);
    void HandleAiError(const std::string& error_message);
    void ShowAiText(const std::string& text, bool switch_to_ai_buffer);
    void ShowInlineAiText(const std::string& text, std::string state_label, bool waiting, bool failed);
    void ReopenAiScratch();
    void ReopenPatchPreview();
    void QuitEditor();
    void HandlePatchAction(CommandType command_type);
    void InvalidatePatchSessionForManualFileEdit();

    struct ActiveAiRequest {
        AiRequestKind kind = AiRequestKind::Explain;
        std::string label;
        std::string streamed_text;
        std::string response_prefix;
        bool inline_viewer = false;
    };

    Terminal terminal_;
    Screen screen_;
    EditorState state_;
    std::unique_ptr<IAiClient> ai_client_;
    LanguageServerClient language_server_client_;
    bool completion_auto_suppressed_ = false;
    bool diagnostics_auto_suppressed_ = false;
    bool language_server_document_synced_ = false;
    std::optional<Cursor> dismissed_completion_prefix_;
    bool running_ = true;
    bool command_mode_ = false;
    std::string command_input_;
    bool file_picker_mode_ = false;
    std::string file_picker_query_;
    std::filesystem::path file_picker_root_;
    std::vector<std::string> file_picker_files_;
    std::vector<std::string> file_picker_matches_;
    size_t file_picker_selected_ = 0;
    bool pending_quit_confirm_ = false;
    std::optional<ActiveAiRequest> active_ai_request_;
    bool ai_request_backgrounded_ = false;
    std::chrono::steady_clock::time_point next_ai_loading_tick_{};
    size_t ai_loading_frame_ = 0;
};

}  // namespace flowstate
