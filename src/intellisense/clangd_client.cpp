#include "intellisense/clangd_client.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace flowstate {

namespace {

bool IsExecutable(const std::filesystem::path& path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

std::optional<std::filesystem::path> FindExecutable(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path requested(name);
    if (requested.has_parent_path()) {
        return IsExecutable(requested) ? std::optional<std::filesystem::path>(requested) : std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return std::nullopt;
    }

    std::string_view paths(path_env);
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(':', start);
        const std::string_view entry =
            end == std::string_view::npos ? paths.substr(start) : paths.substr(start, end - start);
        const std::filesystem::path candidate =
            (entry.empty() ? std::filesystem::current_path() : std::filesystem::path(entry)) / std::string(name);
        if (IsExecutable(candidate)) {
            return candidate;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> CurrentExecutablePath() {
    std::vector<char> buffer(4096);
    const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0) {
        return std::nullopt;
    }
    buffer[static_cast<size_t>(size)] = '\0';
    return std::filesystem::path(buffer.data());
}

std::optional<std::filesystem::path> FindLocalNodeExecutableFrom(std::filesystem::path current,
                                                                 std::string_view name) {
    while (!current.empty()) {
        const std::filesystem::path candidate = current / "node_modules" / ".bin" / std::string(name);
        if (IsExecutable(candidate)) {
            return candidate;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FindLocalNodeExecutable(std::string_view name) {
    if (const std::optional<std::filesystem::path> executable =
            FindLocalNodeExecutableFrom(std::filesystem::current_path(), name);
        executable.has_value()) {
        return executable;
    }

    const std::optional<std::filesystem::path> current_executable = CurrentExecutablePath();
    if (!current_executable.has_value()) {
        return std::nullopt;
    }
    return FindLocalNodeExecutableFrom(current_executable->parent_path(), name);
}

std::optional<std::filesystem::path> FindGoBinExecutable(std::string_view name) {
    const char* gobin = std::getenv("GOBIN");
    if (gobin != nullptr && *gobin != '\0') {
        const std::filesystem::path candidate = std::filesystem::path(gobin) / std::string(name);
        if (IsExecutable(candidate)) {
            return candidate;
        }
    }

    const char* gopath = std::getenv("GOPATH");
    if (gopath != nullptr && *gopath != '\0') {
        std::string_view paths(gopath);
        size_t start = 0;
        while (start <= paths.size()) {
            const size_t end = paths.find(':', start);
            const std::string_view entry =
                end == std::string_view::npos ? paths.substr(start) : paths.substr(start, end - start);
            if (!entry.empty()) {
                const std::filesystem::path candidate =
                    std::filesystem::path(std::string(entry)) / "bin" / std::string(name);
                if (IsExecutable(candidate)) {
                    return candidate;
                }
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }

    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        const std::filesystem::path candidate = std::filesystem::path(home) / "go" / "bin" / std::string(name);
        if (IsExecutable(candidate)) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveClangdExecutable() {
    const char* configured = std::getenv("FLOWSTATE_CLANGD_PATH");
    if (configured != nullptr && *configured != '\0') {
        return FindExecutable(configured);
    }
    return FindExecutable("clangd");
}

struct ServerDefinition {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::string display_name;
};

std::string FileName(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    return filename.empty() ? path.string() : filename;
}

bool UsesPyrightStdio(const std::filesystem::path& executable) {
    const std::string filename = FileName(executable);
    return filename.find("pyright") != std::string::npos;
}

std::optional<ServerDefinition> ResolvePythonServer(std::string* error) {
    const char* configured = std::getenv("FLOWSTATE_PYTHON_LSP_PATH");
    if (configured != nullptr && *configured != '\0') {
        const std::optional<std::filesystem::path> executable = FindExecutable(configured);
        if (!executable.has_value()) {
            if (error != nullptr) {
                *error = "Python language server not found at FLOWSTATE_PYTHON_LSP_PATH.";
            }
            return std::nullopt;
        }
        ServerDefinition server{.executable = *executable, .display_name = FileName(*executable)};
        if (UsesPyrightStdio(*executable)) {
            server.arguments.push_back("--stdio");
        }
        return server;
    }

    if (const std::optional<std::filesystem::path> executable = FindLocalNodeExecutable("pyright-langserver");
        executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .arguments = {"--stdio"},
            .display_name = "pyright-langserver",
        };
    }
    if (const std::optional<std::filesystem::path> executable = FindExecutable("pyright-langserver");
        executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .arguments = {"--stdio"},
            .display_name = "pyright-langserver",
        };
    }
    if (const std::optional<std::filesystem::path> executable = FindExecutable("pylsp"); executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .display_name = "pylsp",
        };
    }

    if (error != nullptr) {
        *error = "Python language server not found. Run npm install for the project pyright dependency, or set "
                 "FLOWSTATE_PYTHON_LSP_PATH.";
    }
    return std::nullopt;
}

std::optional<ServerDefinition> ResolveTypeScriptServer(std::string* error) {
    const char* configured = std::getenv("FLOWSTATE_TYPESCRIPT_LSP_PATH");
    if (configured != nullptr && *configured != '\0') {
        const std::optional<std::filesystem::path> executable = FindExecutable(configured);
        if (!executable.has_value()) {
            if (error != nullptr) {
                *error = "TypeScript language server not found at FLOWSTATE_TYPESCRIPT_LSP_PATH.";
            }
            return std::nullopt;
        }
        return ServerDefinition{
            .executable = *executable,
            .arguments = {"--stdio"},
            .display_name = FileName(*executable),
        };
    }

    if (const std::optional<std::filesystem::path> executable =
            FindLocalNodeExecutable("typescript-language-server");
        executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .arguments = {"--stdio"},
            .display_name = "typescript-language-server",
        };
    }
    if (const std::optional<std::filesystem::path> executable = FindExecutable("typescript-language-server");
        executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .arguments = {"--stdio"},
            .display_name = "typescript-language-server",
        };
    }

    if (error != nullptr) {
        *error = "TypeScript language server not found. Run npm install for the project TypeScript IntelliSense "
                 "dependencies, or set FLOWSTATE_TYPESCRIPT_LSP_PATH.";
    }
    return std::nullopt;
}

std::optional<ServerDefinition> ResolveGoServer(std::string* error) {
    const char* configured = std::getenv("FLOWSTATE_GO_LSP_PATH");
    if (configured != nullptr && *configured != '\0') {
        const std::optional<std::filesystem::path> executable = FindExecutable(configured);
        if (!executable.has_value()) {
            if (error != nullptr) {
                *error = "Go language server not found at FLOWSTATE_GO_LSP_PATH.";
            }
            return std::nullopt;
        }
        return ServerDefinition{
            .executable = *executable,
            .display_name = FileName(*executable),
        };
    }

    if (const std::optional<std::filesystem::path> executable = FindExecutable("gopls"); executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .display_name = "gopls",
        };
    }
    if (const std::optional<std::filesystem::path> executable = FindGoBinExecutable("gopls");
        executable.has_value()) {
        return ServerDefinition{
            .executable = *executable,
            .display_name = "gopls",
        };
    }

    if (error != nullptr) {
        *error = "Go language server not found. Install gopls with `go install golang.org/x/tools/gopls@latest`, "
                 "or set FLOWSTATE_GO_LSP_PATH.";
    }
    return std::nullopt;
}

std::optional<ServerDefinition> ResolveServer(LanguageServerKind kind, std::string* error) {
    switch (kind) {
        case LanguageServerKind::Clangd: {
            const std::optional<std::filesystem::path> executable = ResolveClangdExecutable();
            if (!executable.has_value()) {
                if (error != nullptr) {
                    *error = "clangd not found. Install clangd or set FLOWSTATE_CLANGD_PATH.";
                }
                return std::nullopt;
            }
            return ServerDefinition{
                .executable = *executable,
                .arguments = {"--log=error"},
                .display_name = "clangd",
            };
        }
        case LanguageServerKind::Python:
            return ResolvePythonServer(error);
        case LanguageServerKind::TypeScript:
            return ResolveTypeScriptServer(error);
        case LanguageServerKind::Go:
            return ResolveGoServer(error);
    }
    return std::nullopt;
}

JsonValue::Object TextDocumentIdentifier(const std::string& uri) {
    JsonValue::Object text_document;
    text_document["uri"] = JsonValue(uri);
    return text_document;
}

JsonValue::Object PositionObject(Cursor cursor) {
    JsonValue::Object position;
    position["line"] = JsonValue(static_cast<int>(cursor.row));
    position["character"] = JsonValue(static_cast<int>(cursor.col));
    return position;
}

JsonValue::Object RangeObject(Cursor start, Cursor end) {
    JsonValue::Object range;
    range["start"] = JsonValue(PositionObject(start));
    range["end"] = JsonValue(PositionObject(end));
    return range;
}

std::optional<Cursor> CursorFromJson(const JsonValue* value) {
    if (value == nullptr || !value->isObject()) {
        return std::nullopt;
    }
    const JsonValue* line = value->find("line");
    const JsonValue* character = value->find("character");
    if (line == nullptr || character == nullptr || !line->isNumber() || !character->isNumber() ||
        line->intValue() < 0 || character->intValue() < 0) {
        return std::nullopt;
    }
    return Cursor{static_cast<size_t>(line->intValue()), static_cast<size_t>(character->intValue())};
}

std::optional<DiagnosticRange> DiagnosticRangeFromJson(const JsonValue* value) {
    if (value == nullptr || !value->isObject()) {
        return std::nullopt;
    }
    const std::optional<Cursor> start = CursorFromJson(value->find("start"));
    const std::optional<Cursor> end = CursorFromJson(value->find("end"));
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    return DiagnosticRange{.start = *start, .end = *end};
}

DiagnosticSeverity DiagnosticSeverityFromLsp(int severity) {
    switch (severity) {
        case 1:
            return DiagnosticSeverity::Error;
        case 2:
            return DiagnosticSeverity::Warning;
        case 3:
            return DiagnosticSeverity::Information;
        case 4:
            return DiagnosticSeverity::Hint;
        default:
            return DiagnosticSeverity::Error;
    }
}

std::optional<CompletionTextEdit> TextEditFromJson(const JsonValue* value, bool snippet_format) {
    if (value == nullptr || !value->isObject()) {
        return std::nullopt;
    }

    const JsonValue* range = value->find("range");
    const JsonValue* new_text = value->find("newText");
    if (range == nullptr || !range->isObject() || new_text == nullptr || !new_text->isString()) {
        return std::nullopt;
    }

    const std::optional<Cursor> start = CursorFromJson(range->find("start"));
    const std::optional<Cursor> end = CursorFromJson(range->find("end"));
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }

    return CompletionTextEdit{
        .start = *start,
        .end = *end,
        .new_text = snippet_format ? "" : new_text->stringValue(),
    };
}

std::string StringField(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->isString()) {
        return {};
    }
    return value->stringValue();
}

int IntField(const JsonValue& object, std::string_view key, int fallback = 0) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->isNumber()) {
        return fallback;
    }
    return value->intValue();
}

std::vector<CompletionItem> ParseCompletionItems(const JsonValue& result) {
    const JsonValue::Array* items = nullptr;
    if (result.isArray()) {
        items = &result.arrayValue();
    } else if (result.isObject()) {
        const JsonValue* item_value = result.find("items");
        if (item_value != nullptr && item_value->isArray()) {
            items = &item_value->arrayValue();
        }
    }
    if (items == nullptr) {
        return {};
    }

    std::vector<CompletionItem> parsed;
    parsed.reserve(items->size());
    for (const JsonValue& value : *items) {
        if (!value.isObject()) {
            continue;
        }
        const std::string label = StringField(value, "label");
        if (label.empty()) {
            continue;
        }

        const bool snippet_format = IntField(value, "insertTextFormat") == 2;
        CompletionItem item;
        item.label = label;
        item.detail = StringField(value, "detail");
        item.filter_text = StringField(value, "filterText");
        item.sort_text = StringField(value, "sortText");
        item.insert_text = snippet_format ? label : StringField(value, "insertText");
        if (item.insert_text.empty()) {
            item.insert_text = label;
        }

        if (const JsonValue* text_edit = value.find("textEdit"); text_edit != nullptr) {
            item.text_edit = TextEditFromJson(text_edit, snippet_format);
            if (item.text_edit.has_value() && item.text_edit->new_text.empty()) {
                item.text_edit->new_text = label;
            }
        }
        parsed.push_back(std::move(item));
        if (parsed.size() >= 100) {
            break;
        }
    }
    return parsed;
}

std::vector<Diagnostic> ParseDiagnostics(const JsonValue& params) {
    const JsonValue* diagnostics_value = params.find("diagnostics");
    if (diagnostics_value == nullptr || !diagnostics_value->isArray()) {
        return {};
    }

    std::vector<Diagnostic> diagnostics;
    for (const JsonValue& value : diagnostics_value->arrayValue()) {
        if (!value.isObject()) {
            continue;
        }
        const std::optional<DiagnosticRange> range = DiagnosticRangeFromJson(value.find("range"));
        if (!range.has_value()) {
            continue;
        }
        diagnostics.push_back({
            .range = *range,
            .severity = DiagnosticSeverityFromLsp(IntField(value, "severity", 1)),
            .message = StringField(value, "message"),
            .source = StringField(value, "source"),
        });
    }
    return diagnostics;
}

std::string HeaderForBody(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
}

bool WriteAll(int fd, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::optional<size_t> ParseContentLength(std::string_view header) {
    constexpr std::string_view kPrefix = "Content-Length:";
    size_t line_start = 0;
    while (line_start < header.size()) {
        const size_t line_end = header.find("\r\n", line_start);
        const std::string_view line =
            line_end == std::string_view::npos ? header.substr(line_start) : header.substr(line_start, line_end - line_start);
        if (line.size() >= kPrefix.size() && line.compare(0, kPrefix.size(), kPrefix) == 0) {
            size_t value_start = kPrefix.size();
            while (value_start < line.size() && line[value_start] == ' ') {
                ++value_start;
            }
            size_t value = 0;
            for (size_t index = value_start; index < line.size(); ++index) {
                if (line[index] < '0' || line[index] > '9') {
                    return std::nullopt;
                }
                value = value * 10 + static_cast<size_t>(line[index] - '0');
            }
            return value;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 2;
    }
    return std::nullopt;
}

std::string LanguageIdForBuffer(const Buffer& buffer) {
    switch (buffer.languageId()) {
        case LanguageId::C:
            return "c";
        case LanguageId::CHeader:
        case LanguageId::Cpp:
            return "cpp";
        case LanguageId::Python:
            return "python";
        case LanguageId::JavaScript:
            return "javascript";
        case LanguageId::TypeScript:
            return "typescript";
        case LanguageId::Go:
            return "go";
        default:
            return "plaintext";
    }
}

std::filesystem::path AbsolutePathForBuffer(const Buffer& buffer) {
    if (buffer.path().has_value()) {
        return std::filesystem::absolute(*buffer.path()).lexically_normal();
    }
    return std::filesystem::current_path() / buffer.name();
}

std::string StandardFlag(std::string standard) {
    if (standard.empty()) {
        return {};
    }
    if (standard.rfind("-std=", 0) == 0) {
        return standard;
    }
    return "-std=" + standard;
}

}  // namespace

LanguageServerClient::LanguageServerClient(std::string cpp_standard) : cpp_standard_(std::move(cpp_standard)) {}

LanguageServerClient::~LanguageServerClient() { Shutdown(); }

bool LanguageServerClient::Start(const Buffer& buffer, std::string* error) {
    const std::optional<LanguageServerKind> kind = LanguageServerKindForLanguage(buffer.languageId());
    if (!kind.has_value()) {
        if (error != nullptr) {
            *error = "IntelliSense is not enabled for " + std::string(LanguageDisplayName(buffer.languageId())) + ".";
        }
        return false;
    }
    if (IsStarted() && active_kind_.has_value() && *active_kind_ == *kind) {
        return true;
    }
    if (IsStarted()) {
        Shutdown();
    }
    return StartServer(*kind, ResolveLanguageServerProjectRoot(buffer), error);
}

bool LanguageServerClient::StartServer(LanguageServerKind kind,
                                       const std::filesystem::path& project_root,
                                       std::string* error) {
    if (IsStarted()) {
        return true;
    }
    ::signal(SIGPIPE, SIG_IGN);

    const std::optional<ServerDefinition> server = ResolveServer(kind, error);
    if (!server.has_value()) {
        return false;
    }
    display_name_ = server->display_name;

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) == -1 || ::pipe(stdout_pipe) == -1) {
        if (error != nullptr) {
            *error = std::string("Unable to create ") + display_name_ + " pipes: " + std::strerror(errno);
        }
        return false;
    }

    const pid_t child = ::fork();
    if (child == -1) {
        if (error != nullptr) {
            *error = std::string("Unable to start ") + display_name_ + ": " + std::strerror(errno);
        }
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return false;
    }

    if (child == 0) {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        const int dev_null = ::open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDERR_FILENO);
        }
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        if (!project_root.empty()) {
            ::chdir(project_root.c_str());
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(server->executable.c_str()));
        for (const std::string& argument : server->arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(server->executable.c_str(), argv.data());
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    input_fd_ = stdin_pipe[1];
    output_fd_ = stdout_pipe[0];
    child_pid_ = static_cast<int>(child);
    active_kind_ = kind;
    const int flags = ::fcntl(output_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(output_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    JsonValue::Object completion_item;
    completion_item["snippetSupport"] = JsonValue(false);
    JsonValue::Object completion_capabilities;
    completion_capabilities["completionItem"] = JsonValue(std::move(completion_item));
    JsonValue::Object text_document_capabilities;
    text_document_capabilities["completion"] = JsonValue(std::move(completion_capabilities));
    text_document_capabilities["publishDiagnostics"] = JsonValue(JsonValue::Object{});
    JsonValue::Object capabilities;
    capabilities["textDocument"] = JsonValue(std::move(text_document_capabilities));

    JsonValue::Object params;
    params["processId"] = JsonValue(static_cast<int>(::getpid()));
    params["rootUri"] = JsonValue(FileUriFromPath(project_root));
    params["capabilities"] = JsonValue(std::move(capabilities));
    JsonValue::Object workspace_folder;
    workspace_folder["uri"] = JsonValue(FileUriFromPath(project_root));
    workspace_folder["name"] =
        JsonValue(project_root.filename().empty() ? project_root.string() : project_root.filename().string());
    params["workspaceFolders"] = JsonValue(JsonValue::Array{JsonValue(std::move(workspace_folder))});

    const std::string standard_flag = kind == LanguageServerKind::Clangd ? StandardFlag(cpp_standard_) : "";
    if (!standard_flag.empty()) {
        JsonValue::Object initialization_options;
        initialization_options["fallbackFlags"] =
            JsonValue(JsonValue::Array{JsonValue(standard_flag)});
        params["initializationOptions"] = JsonValue(std::move(initialization_options));
    }

    std::string send_error;
    if (!SendRequest(next_request_id_++, "initialize", JsonValue(std::move(params)), &send_error) ||
        !SendNotification("initialized", JsonValue(JsonValue::Object{}), &send_error)) {
        const std::string server_name = displayName();
        Shutdown();
        if (error != nullptr) {
            *error = send_error.empty() ? "Unable to initialize " + server_name + "." : send_error;
        }
        return false;
    }
    return true;
}

bool LanguageServerClient::IsStarted() const { return input_fd_ >= 0 && output_fd_ >= 0 && child_pid_ > 0; }

bool LanguageServerClient::IsStartedFor(LanguageId language_id) const {
    const std::optional<LanguageServerKind> kind = LanguageServerKindForLanguage(language_id);
    return IsStarted() && active_kind_.has_value() && kind.has_value() && *active_kind_ == *kind;
}

std::string LanguageServerClient::displayName() const { return display_name_.empty() ? "language server" : display_name_; }

bool LanguageServerClient::SyncDocument(const Buffer& buffer, std::string* error) {
    if (!IsStarted()) {
        if (error != nullptr) {
            *error = displayName() + " is not running.";
        }
        return false;
    }

    const std::string uri = FileUriFromPath(AbsolutePathForBuffer(buffer));
    if (current_uri_ != uri) {
        SendDidClose();
        current_uri_ = uri;
        document_version_ = 1;
        return SendDidOpen(buffer, uri, error);
    }
    ++document_version_;
    return SendDidChange(buffer, uri, error);
}

std::optional<int> LanguageServerClient::RequestCompletion(const Buffer& buffer, Cursor cursor, std::string* error) {
    if (!IsStarted()) {
        if (error != nullptr) {
            *error = displayName() + " is not running.";
        }
        return std::nullopt;
    }

    const std::string uri = FileUriFromPath(AbsolutePathForBuffer(buffer));
    JsonValue::Object context;
    context["triggerKind"] = JsonValue(1);

    JsonValue::Object params;
    params["textDocument"] = JsonValue(TextDocumentIdentifier(uri));
    params["position"] = JsonValue(PositionObject(cursor));
    params["context"] = JsonValue(std::move(context));

    const int request_id = next_request_id_++;
    if (!SendRequest(request_id, "textDocument/completion", JsonValue(std::move(params)), error)) {
        return std::nullopt;
    }
    return request_id;
}

std::vector<CompletionEvent> LanguageServerClient::PollEvents() {
    CheckProcessExit();
    ReadAvailableMessages();
    std::vector<CompletionEvent> events;
    events.reserve(queued_events_.size());
    while (!queued_events_.empty()) {
        events.push_back(std::move(queued_events_.front()));
        queued_events_.pop_front();
    }
    return events;
}

void LanguageServerClient::Shutdown() {
    if (IsStarted()) {
        std::string ignored;
        SendNotification("exit", JsonValue(JsonValue::Object{}), &ignored);
    }
    if (input_fd_ >= 0) {
        ::close(input_fd_);
        input_fd_ = -1;
    }
    if (output_fd_ >= 0) {
        ::close(output_fd_);
        output_fd_ = -1;
    }
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGTERM);
        int status = 0;
        if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
            ::kill(child_pid_, SIGKILL);
            ::waitpid(child_pid_, nullptr, 0);
        }
    }
    child_pid_ = -1;
    active_kind_.reset();
    display_name_.clear();
    current_uri_.clear();
    read_buffer_.clear();
    queued_events_.clear();
}

bool LanguageServerClient::SendMessage(const std::string& json, std::string* error) {
    const std::string framed = HeaderForBody(json) + json;
    if (!WriteAll(input_fd_, framed)) {
        if (error != nullptr) {
            *error = std::string("Unable to write to ") + displayName() + ": " + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool LanguageServerClient::SendRequest(int id, const std::string& method, JsonValue params, std::string* error) {
    JsonValue::Object message;
    message["jsonrpc"] = JsonValue("2.0");
    message["id"] = JsonValue(id);
    message["method"] = JsonValue(method);
    message["params"] = std::move(params);
    return SendMessage(JsonValue(std::move(message)).Serialize(), error);
}

bool LanguageServerClient::SendResponse(int id, JsonValue result, std::string* error) {
    JsonValue::Object message;
    message["jsonrpc"] = JsonValue("2.0");
    message["id"] = JsonValue(id);
    message["result"] = std::move(result);
    return SendMessage(JsonValue(std::move(message)).Serialize(), error);
}

bool LanguageServerClient::SendNotification(const std::string& method, JsonValue params, std::string* error) {
    JsonValue::Object message;
    message["jsonrpc"] = JsonValue("2.0");
    message["method"] = JsonValue(method);
    message["params"] = std::move(params);
    return SendMessage(JsonValue(std::move(message)).Serialize(), error);
}

void LanguageServerClient::ReadAvailableMessages() {
    if (output_fd_ < 0) {
        return;
    }

    char buffer[4096];
    while (true) {
        const ssize_t bytes = ::read(output_fd_, buffer, sizeof(buffer));
        if (bytes > 0) {
            read_buffer_.append(buffer, static_cast<size_t>(bytes));
            continue;
        }
        if (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }
        if (bytes == 0 || (bytes == -1 && errno != EINTR)) {
            break;
        }
    }

    while (true) {
        const size_t header_end = read_buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            break;
        }
        const std::optional<size_t> content_length =
            ParseContentLength(std::string_view(read_buffer_).substr(0, header_end));
        if (!content_length.has_value()) {
            read_buffer_.erase(0, header_end + 4);
            continue;
        }
        const size_t body_start = header_end + 4;
        if (read_buffer_.size() < body_start + *content_length) {
            break;
        }

        const std::string body = read_buffer_.substr(body_start, *content_length);
        read_buffer_.erase(0, body_start + *content_length);
        std::string parse_error;
        const std::optional<JsonValue> message = JsonValue::Parse(body, &parse_error);
        if (message.has_value()) {
            HandleMessage(*message);
        }
    }
}

void LanguageServerClient::HandleMessage(const JsonValue& message) {
    const JsonValue* id = message.find("id");
    if (id == nullptr || !id->isNumber()) {
        HandleNotification(message);
        return;
    }

    const int request_id = id->intValue();
    const JsonValue* method = message.find("method");
    if (method != nullptr && method->isString()) {
        HandleServerRequest(request_id, message);
        return;
    }

    if (const JsonValue* error = message.find("error"); error != nullptr) {
        queued_events_.push_back({.kind = CompletionEventKind::Error,
                                  .request_id = request_id,
                                  .error_message = displayName() + " completion request failed."});
        return;
    }

    const JsonValue* result = message.find("result");
    if (result != nullptr) {
        HandleCompletionResponse(request_id, *result);
    }
}

void LanguageServerClient::HandleServerRequest(int request_id, const JsonValue& message) {
    JsonValue result;
    const JsonValue* method = message.find("method");
    if (method != nullptr && method->isString() && method->stringValue() == "workspace/configuration") {
        JsonValue::Array settings;
        const JsonValue* params = message.find("params");
        const JsonValue* items = params == nullptr ? nullptr : params->find("items");
        if (items != nullptr && items->isArray()) {
            settings.resize(items->arrayValue().size());
        }
        result = JsonValue(std::move(settings));
    }

    std::string ignored;
    SendResponse(request_id, std::move(result), &ignored);
}

void LanguageServerClient::HandleNotification(const JsonValue& message) {
    const JsonValue* method = message.find("method");
    if (method == nullptr || !method->isString()) {
        return;
    }
    if (method->stringValue() == "textDocument/publishDiagnostics") {
        const JsonValue* params = message.find("params");
        if (params != nullptr) {
            HandlePublishDiagnostics(*params);
        }
    }
}

void LanguageServerClient::HandlePublishDiagnostics(const JsonValue& params) {
    std::vector<Diagnostic> diagnostics = ParseDiagnostics(params);
    for (Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.source.empty()) {
            diagnostic.source = displayName();
        }
    }
    queued_events_.push_back({
        .kind = CompletionEventKind::Diagnostics,
        .diagnostics = std::move(diagnostics),
    });
}

void LanguageServerClient::HandleCompletionResponse(int request_id, const JsonValue& result) {
    queued_events_.push_back({
        .kind = CompletionEventKind::Completed,
        .request_id = request_id,
        .items = ParseCompletionItems(result),
    });
}

void LanguageServerClient::CheckProcessExit() {
    if (child_pid_ <= 0) {
        return;
    }

    int status = 0;
    const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
    if (result == 0 || result == -1) {
        return;
    }

    if (input_fd_ >= 0) {
        ::close(input_fd_);
        input_fd_ = -1;
    }
    if (output_fd_ >= 0) {
        ::close(output_fd_);
        output_fd_ = -1;
    }
    const std::string server_name = displayName();
    child_pid_ = -1;
    active_kind_.reset();
    display_name_.clear();
    current_uri_.clear();
    read_buffer_.clear();
    queued_events_.push_back({.kind = CompletionEventKind::Error,
                              .request_id = 0,
                              .error_message = server_name + " exited before completing the request."});
}

bool LanguageServerClient::SendDidOpen(const Buffer& buffer, const std::string& uri, std::string* error) {
    JsonValue::Object text_document;
    text_document["uri"] = JsonValue(uri);
    text_document["languageId"] = JsonValue(LanguageIdForBuffer(buffer));
    text_document["version"] = JsonValue(document_version_);
    text_document["text"] = JsonValue(buffer.text());

    JsonValue::Object params;
    params["textDocument"] = JsonValue(std::move(text_document));
    return SendNotification("textDocument/didOpen", JsonValue(std::move(params)), error);
}

bool LanguageServerClient::SendDidChange(const Buffer& buffer, const std::string& uri, std::string* error) {
    JsonValue::Object text_document;
    text_document["uri"] = JsonValue(uri);
    text_document["version"] = JsonValue(document_version_);

    JsonValue::Object change;
    change["text"] = JsonValue(buffer.text());

    JsonValue::Object params;
    params["textDocument"] = JsonValue(std::move(text_document));
    params["contentChanges"] = JsonValue(JsonValue::Array{JsonValue(std::move(change))});
    return SendNotification("textDocument/didChange", JsonValue(std::move(params)), error);
}

void LanguageServerClient::SendDidClose() {
    if (current_uri_.empty() || !IsStarted()) {
        return;
    }

    JsonValue::Object params;
    params["textDocument"] = JsonValue(TextDocumentIdentifier(current_uri_));
    std::string ignored;
    SendNotification("textDocument/didClose", JsonValue(std::move(params)), &ignored);
}

std::filesystem::path ResolveClangdProjectRoot(const Buffer& buffer) {
    std::filesystem::path path = AbsolutePathForBuffer(buffer);
    if (!std::filesystem::is_directory(path)) {
        path = path.parent_path();
    }

    std::filesystem::path current = path;
    while (!current.empty()) {
        if (std::filesystem::exists(current / "compile_commands.json") ||
            std::filesystem::exists(current / "build" / "compile_commands.json") ||
            std::filesystem::exists(current / ".git")) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return path.empty() ? std::filesystem::current_path() : path;
}

std::filesystem::path ResolveLanguageServerProjectRoot(const Buffer& buffer) {
    const LanguageId language_id = buffer.languageId();
    if (language_id == LanguageId::Python || language_id == LanguageId::JavaScript ||
        language_id == LanguageId::TypeScript || language_id == LanguageId::Go) {
        std::filesystem::path path = AbsolutePathForBuffer(buffer);
        if (!std::filesystem::is_directory(path)) {
            path = path.parent_path();
        }

        std::filesystem::path current = path;
        while (!current.empty()) {
            const bool python_root =
                language_id == LanguageId::Python &&
                (std::filesystem::exists(current / "pyproject.toml") ||
                 std::filesystem::exists(current / "setup.cfg") ||
                 std::filesystem::exists(current / "setup.py") ||
                 std::filesystem::exists(current / "requirements.txt") ||
                 std::filesystem::exists(current / "Pipfile") ||
                 std::filesystem::exists(current / "poetry.lock"));
            const bool typescript_root =
                (language_id == LanguageId::JavaScript || language_id == LanguageId::TypeScript) &&
                (std::filesystem::exists(current / "tsconfig.json") ||
                 std::filesystem::exists(current / "jsconfig.json") ||
                 std::filesystem::exists(current / "package.json"));
            const bool go_root = language_id == LanguageId::Go &&
                                 (std::filesystem::exists(current / "go.work") ||
                                  std::filesystem::exists(current / "go.mod"));
            if (python_root || typescript_root || go_root || std::filesystem::exists(current / ".git")) {
                return current;
            }
            const std::filesystem::path parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }
        return path.empty() ? std::filesystem::current_path() : path;
    }

    return ResolveClangdProjectRoot(buffer);
}

std::optional<LanguageServerKind> LanguageServerKindForLanguage(LanguageId language_id) {
    if (IsCppCompletionLanguage(language_id)) {
        return LanguageServerKind::Clangd;
    }
    if (language_id == LanguageId::Python) {
        return LanguageServerKind::Python;
    }
    if (language_id == LanguageId::JavaScript || language_id == LanguageId::TypeScript) {
        return LanguageServerKind::TypeScript;
    }
    if (language_id == LanguageId::Go) {
        return LanguageServerKind::Go;
    }
    return std::nullopt;
}

std::string FileUriFromPath(const std::filesystem::path& path) {
    const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
    std::ostringstream uri;
    uri << "file://";
    const std::string text = absolute.generic_string();
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char ch : text) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                                ch == '.' || ch == '~' || ch == '/';
        if (unreserved) {
            uri << static_cast<char>(ch);
        } else {
            uri << '%' << kHex[ch >> 4] << kHex[ch & 0x0F];
        }
    }
    return uri.str();
}

std::vector<CompletionItem> ParseCompletionItemsForTest(const JsonValue& result) {
    return ParseCompletionItems(result);
}

std::vector<Diagnostic> ParseDiagnosticsForTest(const JsonValue& params) {
    return ParseDiagnostics(params);
}

}  // namespace flowstate
