#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "build.h"
#include "buffer.h"
#include "cursor.h"
#include "ai/rate_limit.h"
#include "intellisense/completion.h"
#include "intellisense/diagnostic.h"
#include "patch.h"
#include "selection.h"

namespace flowstate {

enum class ViewKind {
    File,
    AiScratch,
    PatchPreview,
    BuildOutput,
};

struct InlineAiSession {
    size_t anchor_row = 0;
    size_t scroll_row = 0;
    size_t cursor_body_row = 0;
    bool focused = false;
    std::string title;
    std::string provider_name;
    std::string state_label;
    std::string text;
    std::string input_text;
    size_t input_cursor_col = 0;
    bool waiting = false;
    bool failed = false;
};

class EditorState {
  public:
    explicit EditorState(Buffer file_buffer);

    Buffer& fileBuffer();
    const Buffer& fileBuffer() const;
    void setFileBuffer(Buffer buffer);

    Buffer& aiBuffer();
    Buffer& patchBuffer();
    Buffer& buildBuffer();
    Buffer& activeBuffer();
    const Buffer& activeBuffer() const;

    ViewKind activeView() const;
    void setActiveView(ViewKind view);

    Viewport& viewport(ViewKind view);
    const Viewport& viewport(ViewKind view) const;
    Viewport& activeViewport();
    const Viewport& activeViewport() const;

    Cursor& fileCursor();
    const Cursor& fileCursor() const;

    Selection& selection();
    const Selection& selection() const;
    void clearSelection();
    std::string selectedOrCurrentText() const;

    void setStatus(std::string text, int seconds = 5);
    std::string statusText() const;

    void setBuildCommand(std::string command);
    const std::string& buildCommand() const;
    void setLastBuild(BuildResult result);
    const std::optional<BuildResult>& lastBuild() const;

    void setAiText(const std::string& text);
    void setBuildOutput(const std::string& text);

    void setPatchSession(std::optional<PatchSession> session);
    std::optional<PatchSession>& patchSession();
    const std::optional<PatchSession>& patchSession() const;
    void syncPatchPreview();

    void setAiProviderName(std::string provider_name);
    const std::string& aiProviderName() const;
    void setAiRequestState(std::string state);
    void clearAiRequestState();
    const std::string& aiRequestState() const;
    void setAiRateLimits(std::optional<RateLimitSnapshotInfo> rate_limits);
    const std::optional<RateLimitSnapshotInfo>& aiRateLimits() const;
    void setInlineAiSession(std::optional<InlineAiSession> session);
    std::optional<InlineAiSession>& inlineAiSession();
    const std::optional<InlineAiSession>& inlineAiSession() const;
    void clearInlineAiSession();
    void setClipboardText(std::string text);
    bool hasClipboardText() const;
    std::string_view clipboardText() const;
    void clearClipboard();
    bool isGitChangePeekExpanded(size_t row) const;
    bool hasGitChangePeekExpansions() const;
    void toggleGitChangePeekExpansion(size_t row);
    void clearGitChangePeekExpansions();
    CompletionSession& completionSession();
    const CompletionSession& completionSession() const;
    void setCompletionSession(CompletionSession session);
    void clearCompletionSession();
    void setDiagnostics(std::vector<Diagnostic> diagnostics);
    const std::vector<Diagnostic>& diagnostics() const;
    void clearDiagnostics();
    void BeginFileEdit();
    bool CommitFileEdit();
    bool UndoFileEdit();
    bool RedoFileEdit();

  private:
    struct FileHistoryEntry {
        std::vector<std::string> lines;
        Cursor cursor;
        Selection selection;
        bool dirty = false;
    };

    Viewport& viewportImpl(ViewKind view);
    const Viewport& viewportImpl(ViewKind view) const;
    Buffer& bufferImpl(ViewKind view);
    const Buffer& bufferImpl(ViewKind view) const;
    FileHistoryEntry CaptureFileHistoryEntry() const;
    void RestoreFileHistoryEntry(const FileHistoryEntry& entry);
    static bool SameFileHistoryEntry(const FileHistoryEntry& left, const FileHistoryEntry& right);

    Buffer file_buffer_;
    Buffer ai_buffer_;
    Buffer patch_buffer_;
    Buffer build_buffer_;

    ViewKind active_view_ = ViewKind::File;
    Viewport file_view_;
    Viewport ai_view_;
    Viewport patch_view_;
    Viewport build_view_;

    Selection selection_;
    std::string status_text_;
    std::string ai_provider_name_ = "OFF";
    std::chrono::steady_clock::time_point status_expires_at_{};
    std::string build_command_;
    std::optional<BuildResult> last_build_;
    std::optional<PatchSession> patch_session_;
    std::optional<InlineAiSession> inline_ai_session_;
    std::optional<RateLimitSnapshotInfo> ai_rate_limits_;
    std::string ai_request_state_;
    std::optional<std::string> clipboard_text_;
    std::set<size_t> expanded_git_change_peeks_;
    CompletionSession completion_session_;
    std::vector<Diagnostic> diagnostics_;
    std::optional<FileHistoryEntry> pending_file_edit_;
    std::vector<FileHistoryEntry> undo_history_;
    std::vector<FileHistoryEntry> redo_history_;
};

}  // namespace flowstate
