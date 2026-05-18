#pragma once

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "buffer.h"
#include "intellisense/completion.h"
#include "intellisense/diagnostic.h"
#include "json.h"

namespace flowstate {

enum class CompletionEventKind {
    Completed,
    Diagnostics,
    Error,
};

enum class LanguageServerKind {
    Clangd,
    Python,
    TypeScript,
};

struct CompletionEvent {
    CompletionEventKind kind = CompletionEventKind::Completed;
    int request_id = 0;
    std::vector<CompletionItem> items;
    std::vector<Diagnostic> diagnostics;
    std::string error_message;
};

class LanguageServerClient {
  public:
    LanguageServerClient() = default;
    explicit LanguageServerClient(std::string cpp_standard);
    ~LanguageServerClient();

    bool Start(const Buffer& buffer, std::string* error);
    bool IsStarted() const;
    bool IsStartedFor(LanguageId language_id) const;
    std::string displayName() const;
    bool SyncDocument(const Buffer& buffer, std::string* error);
    std::optional<int> RequestCompletion(const Buffer& buffer, Cursor cursor, std::string* error);
    std::vector<CompletionEvent> PollEvents();
    void Shutdown();

  private:
    bool StartServer(LanguageServerKind kind, const std::filesystem::path& project_root, std::string* error);
    bool SendMessage(const std::string& json, std::string* error);
    bool SendRequest(int id, const std::string& method, JsonValue params, std::string* error);
    bool SendResponse(int id, JsonValue result, std::string* error);
    bool SendNotification(const std::string& method, JsonValue params, std::string* error);
    void ReadAvailableMessages();
    void HandleMessage(const JsonValue& message);
    void HandleServerRequest(int request_id, const JsonValue& message);
    void HandleCompletionResponse(int request_id, const JsonValue& result);
    void HandleNotification(const JsonValue& message);
    void HandlePublishDiagnostics(const JsonValue& params);
    void CheckProcessExit();
    bool SendDidOpen(const Buffer& buffer, const std::string& uri, std::string* error);
    bool SendDidChange(const Buffer& buffer, const std::string& uri, std::string* error);
    void SendDidClose();

    int input_fd_ = -1;
    int output_fd_ = -1;
    int child_pid_ = -1;
    int next_request_id_ = 1;
    int document_version_ = 0;
    std::string cpp_standard_;
    std::optional<LanguageServerKind> active_kind_;
    std::string display_name_;
    std::string current_uri_;
    std::string read_buffer_;
    std::deque<CompletionEvent> queued_events_;
};

std::filesystem::path ResolveClangdProjectRoot(const Buffer& buffer);
std::filesystem::path ResolveLanguageServerProjectRoot(const Buffer& buffer);
std::optional<LanguageServerKind> LanguageServerKindForLanguage(LanguageId language_id);
std::string FileUriFromPath(const std::filesystem::path& path);
std::vector<CompletionItem> ParseCompletionItemsForTest(const JsonValue& result);
std::vector<Diagnostic> ParseDiagnosticsForTest(const JsonValue& params);

}  // namespace flowstate
