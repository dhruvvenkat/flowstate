#include "screen.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>

#include "git_status.h"
#include "selection.h"
#include "syntax/registry.h"
#include "syntax/theme.h"

namespace flowstate {

namespace {

constexpr std::string_view kGutterSeparator = "\xE2\x94\x82";
constexpr std::string_view kGitDashedBar = "\xE2\x94\x86";
constexpr std::string_view kGitDeletedTriangle = "\xE2\x96\xB8";
constexpr std::string_view kGitAddedColor = "\x1b[38;5;71m";
constexpr std::string_view kGitModifiedColor = "\x1b[38;5;39m";
constexpr std::string_view kGitDeletedColor = "\x1b[38;5;196m";
constexpr std::string_view kInlineAiBorderColor = "\x1b[38;5;245m";
constexpr std::string_view kDiagnosticUnderline = "\x1b[4m\x1b[58;5;196m";
constexpr std::string_view kResetUnderline = "\x1b[24m\x1b[59m";
constexpr std::string_view kDiagnosticBackground = "\x1b[48;5;52m";
constexpr std::string_view kSelectionBackground = "\x1b[48;5;238m";
constexpr auto kGitStatusRefreshInterval = std::chrono::milliseconds(750);
constexpr size_t kInlineAiMaxBodyRows = 12;
constexpr size_t kInlineAiInputRows = 1;

enum class AiDiffPhase {
    Outside,
    AwaitNewFileHeader,
    AwaitHunkHeader,
    InHunk,
};

enum class AiDiffLineKind {
    Other,
    FileHeaderOld,
    FileHeaderNew,
    HunkHeader,
    Context,
    Add,
    Remove,
    Meta,
};

struct AiDiffRenderState {
    AiDiffPhase phase = AiDiffPhase::Outside;
    SyntaxLineState old_syntax_state;
    SyntaxLineState new_syntax_state;
};

struct FileRenderState {
    SyntaxLineState syntax_state;
    std::vector<char> delimiter_stack;
};

struct InlineAiBodyLine {
    std::string text;
    bool user_message = false;
    bool code = false;
    LanguageId code_language = LanguageId::PlainText;
    SyntaxLineState code_state;
    size_t code_col_offset = 0;
};

std::string RenderHighlightedCode(std::string_view code,
                                  size_t col_offset,
                                  size_t cols,
                                  const ISyntaxHighlighter& highlighter,
                                  SyntaxLineState line_state,
                                  SyntaxLineState* next_line_state);

std::string EscapeLine(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (char ch : text) {
        if (ch == '\t') {
            output.append("    ");
        } else if (static_cast<unsigned char>(ch) < 32) {
            output.push_back('?');
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

std::string DecoratePatchLine(const std::string& line) {
    if (!line.empty() && line[0] == '+') {
        return "\x1b[32m" + line + std::string(ResetColorCode());
    }
    if (!line.empty() && line[0] == '-') {
        return "\x1b[31m" + line + std::string(ResetColorCode());
    }
    if (line.rfind("@@", 0) == 0) {
        return "\x1b[36m" + line + std::string(ResetColorCode());
    }
    return line;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool IsUserAiMessageLine(std::string_view line) {
    return StartsWith(line, "You: ");
}

std::string_view TrimAsciiWhitespace(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string LowerAscii(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::string CodeFenceLanguageToken(std::string_view info) {
    info = TrimAsciiWhitespace(info);
    size_t end = 0;
    while (end < info.size() && std::isspace(static_cast<unsigned char>(info[end])) == 0) {
        ++end;
    }
    return LowerAscii(info.substr(0, end));
}

LanguageId LanguageIdForCodeFenceInfo(std::string_view info) {
    const std::string language = CodeFenceLanguageToken(info);
    if (language == "c") {
        return LanguageId::C;
    }
    if (language == "h" || language == "cc" || language == "cpp" || language == "cxx" ||
        language == "c++" || language == "hpp") {
        return LanguageId::Cpp;
    }
    if (language == "rs" || language == "rust") {
        return LanguageId::Rust;
    }
    if (language == "py" || language == "python") {
        return LanguageId::Python;
    }
    if (language == "js" || language == "jsx" || language == "javascript") {
        return LanguageId::JavaScript;
    }
    if (language == "ts" || language == "tsx" || language == "typescript") {
        return LanguageId::TypeScript;
    }
    if (language == "java") {
        return LanguageId::Java;
    }
    if (language == "go" || language == "golang") {
        return LanguageId::Go;
    }
    if (language == "md" || language == "markdown") {
        return LanguageId::Markdown;
    }
    return LanguageId::PlainText;
}

bool ParseCodeFence(std::string_view line, std::string_view* info) {
    line = TrimAsciiWhitespace(line);
    if (!StartsWith(line, "```")) {
        return false;
    }
    if (info != nullptr) {
        *info = line.substr(3);
    }
    return true;
}

std::string RenderVisibleText(std::string_view line, size_t col_offset, size_t cols) {
    if (cols == 0 || col_offset >= line.size()) {
        return {};
    }
    return std::string(line.substr(col_offset, cols));
}

size_t LineNumberDigits(const Buffer& buffer) {
    return std::max<size_t>(1, std::to_string(buffer.lineCount()).size());
}

size_t GutterWidth(const Buffer& buffer) {
    return LineNumberDigits(buffer) + 2;
}

bool ShowsLineNumbers(const EditorState& state) {
    return state.activeView() == ViewKind::File;
}

std::string GitMarkerForRow(size_t row, const GitLineStatus& git_status) {
    if (!git_status.available || row >= git_status.lines.size()) {
        return std::string(kGutterSeparator);
    }

    switch (git_status.lines[row].marker) {
        case GitLineMarker::Added:
            return std::string(kGitAddedColor) + std::string(kGitDashedBar) + std::string(ResetColorCode());
        case GitLineMarker::Modified:
            return std::string(kGitModifiedColor) + std::string(kGitDashedBar) + std::string(ResetColorCode());
        case GitLineMarker::Deleted:
            return std::string(kGitDeletedColor) + std::string(kGitDeletedTriangle) + std::string(ResetColorCode());
        case GitLineMarker::Clean:
            return std::string(kGutterSeparator);
    }
    return std::string(kGutterSeparator);
}

const std::vector<std::string>& ExpandedPreviousLinesForRow(const EditorState& state,
                                                           const GitLineStatus& git_status,
                                                           size_t row) {
    static const std::vector<std::string> kEmptyPreviousLines;
    if (!state.isGitChangePeekExpanded(row) || !git_status.available || row >= git_status.lines.size()) {
        return kEmptyPreviousLines;
    }
    return git_status.lines[row].previous_lines;
}

size_t ExpandedGitRowsBefore(const EditorState& state,
                             const GitLineStatus& git_status,
                             size_t start_row,
                             size_t end_row) {
    size_t count = 0;
    for (size_t row = start_row; row < end_row && row < git_status.lines.size(); ++row) {
        count += ExpandedPreviousLinesForRow(state, git_status, row).size();
    }
    return count;
}

struct GitStatusCacheEntry {
    std::filesystem::path file_path;
    size_t line_count = 0;
    std::chrono::steady_clock::time_point refreshed_at;
    GitLineStatus status;
};

const GitLineStatus& GitStatusForBuffer(const Buffer& buffer) {
    static GitStatusCacheEntry cache;

    const std::filesystem::path file_path = buffer.path().value_or(std::filesystem::path());
    const auto now = std::chrono::steady_clock::now();
    if (cache.file_path == file_path && cache.line_count == buffer.lineCount() &&
        now - cache.refreshed_at < kGitStatusRefreshInterval) {
        return cache.status;
    }

    cache.file_path = file_path;
    cache.line_count = buffer.lineCount();
    cache.refreshed_at = now;
    cache.status = LoadGitLineStatus(file_path, buffer.lineCount());
    return cache.status;
}

std::string RenderLineNumber(size_t row, const Buffer& buffer, const GitLineStatus& git_status) {
    std::ostringstream output;
    output << std::setw(static_cast<int>(LineNumberDigits(buffer))) << (row + 1)
           << GitMarkerForRow(row, git_status) << ' ';
    return output.str();
}

std::string RenderGitPreviousLine(const Buffer& buffer,
                                  std::string_view previous_line,
                                  size_t col_offset,
                                  size_t cols) {
    std::ostringstream output;
    output << std::string(LineNumberDigits(buffer), ' ') << std::string(kGitDeletedColor)
           << kGitDeletedTriangle << ResetColorCode() << ' ';
    output << std::string(kGitDeletedColor);
    if (col_offset == 0 && cols > 0) {
        output << "- ";
        if (cols > 2) {
            output << RenderVisibleText(previous_line, 0, cols - 2);
        }
    } else if (col_offset > 0) {
        output << RenderVisibleText(previous_line, col_offset, cols);
    }
    output << ResetColorCode();
    return output.str();
}

size_t InlineAiBodyWidth(size_t content_cols) {
    if (content_cols <= 4) {
        return 0;
    }
    return content_cols - 4;
}

size_t InlineAiInputTextWidth(size_t content_cols) {
    const size_t body_width = InlineAiBodyWidth(content_cols);
    return body_width > 2 ? body_width - 2 : 0;
}

size_t InlineAiInputVisibleStart(const InlineAiSession& session, size_t input_width) {
    if (input_width == 0 || session.input_cursor_col <= input_width) {
        return 0;
    }
    return session.input_cursor_col - input_width;
}

std::string FitInlineText(std::string text, size_t width, std::string_view fill = " ") {
    if (text.size() > width) {
        text.resize(width);
    }
    const size_t pad_count = width - text.size();
    for (size_t index = 0; index < pad_count; ++index) {
        text.append(fill.empty() ? " " : fill);
    }
    return text;
}

std::string FitInlineFooter(std::string left, std::string right, size_t width) {
    if (right.empty()) {
        return FitInlineText(std::move(left), width, "─");
    }
    if (width == 0) {
        return {};
    }

    constexpr size_t kRightBorderGap = 1;
    if (right.size() + kRightBorderGap >= width) {
        right.resize(width - kRightBorderGap);
        right.append("─");
        return right;
    }

    const size_t max_left = width - right.size() - kRightBorderGap;
    if (left.size() > max_left) {
        left.resize(max_left);
    }

    std::string text = std::move(left);
    const size_t fill_count = width - text.size() - right.size() - kRightBorderGap;
    for (size_t index = 0; index < fill_count; ++index) {
        text.append("─");
    }
    text += right;
    text.append("─");
    return text;
}

std::string InlineAiHeaderPrefix(size_t width) {
    if (width == 0) {
        return {};
    }
    if (width == 1) {
        return "─";
    }
    return "─ ";
}

bool RateLimitDurationBetween(const RateLimitWindowInfo& window, int64_t min_mins, int64_t max_mins) {
    return window.window_duration_mins.has_value() && *window.window_duration_mins >= min_mins &&
           *window.window_duration_mins <= max_mins;
}

const RateLimitWindowInfo* PickRateLimitWindow(const RateLimitSnapshotInfo& snapshot,
                                               bool want_weekly,
                                               bool prefer_primary) {
    const auto matches = [want_weekly](const RateLimitWindowInfo& window) {
        if (!window.available) {
            return false;
        }
        return want_weekly ? RateLimitDurationBetween(window, 9000, 11000)
                           : RateLimitDurationBetween(window, 240, 360);
    };

    if (matches(snapshot.primary)) {
        return &snapshot.primary;
    }
    if (matches(snapshot.secondary)) {
        return &snapshot.secondary;
    }
    if (prefer_primary && snapshot.primary.available) {
        return &snapshot.primary;
    }
    if (!prefer_primary && snapshot.secondary.available) {
        return &snapshot.secondary;
    }
    if (snapshot.primary.available) {
        return &snapshot.primary;
    }
    if (snapshot.secondary.available) {
        return &snapshot.secondary;
    }
    return nullptr;
}

std::string FormatRateLimitBar(double used_percent) {
    constexpr size_t kBarWidth = 8;
    const double clamped = std::clamp(used_percent, 0.0, 100.0);
    const size_t filled =
        std::min(kBarWidth, static_cast<size_t>((clamped * static_cast<double>(kBarWidth) + 50.0) / 100.0));
    return "[" + std::string(filled, '#') + std::string(kBarWidth - filled, '-') + "]";
}

std::string FormatRateLimitPercent(double used_percent) {
    const int percent = static_cast<int>(std::clamp(used_percent, 0.0, 999.0) + 0.5);
    return std::to_string(percent) + "%";
}

std::string FormatInlineRateLimits(const std::optional<RateLimitSnapshotInfo>& rate_limits) {
    if (!rate_limits.has_value() || !rate_limits->available) {
        return {};
    }

    const RateLimitWindowInfo* five_hour = PickRateLimitWindow(*rate_limits, false, true);
    const RateLimitWindowInfo* weekly = PickRateLimitWindow(*rate_limits, true, false);
    if (five_hour == nullptr && weekly == nullptr) {
        return {};
    }

    std::string text;
    if (five_hour != nullptr) {
        text += " 5h " + FormatRateLimitBar(five_hour->used_percent) + " " +
                FormatRateLimitPercent(five_hour->used_percent);
    }
    if (weekly != nullptr && weekly != five_hour) {
        if (!text.empty()) {
            text += "  ";
        } else {
            text += " ";
        }
        text += "wk " + FormatRateLimitBar(weekly->used_percent) + " " +
                FormatRateLimitPercent(weekly->used_percent);
    }
    return text;
}

std::vector<std::string> SplitPlainLines(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string line(end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

std::vector<std::string> WrapPlainLine(std::string_view line, size_t width) {
    if (width == 0) {
        return {""};
    }
    if (line.empty()) {
        return {""};
    }

    std::vector<std::string> wrapped;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && line[start] == ' ') {
            ++start;
        }
        if (start >= line.size()) {
            break;
        }

        const size_t remaining = line.size() - start;
        if (remaining <= width) {
            wrapped.emplace_back(line.substr(start));
            break;
        }

        size_t end = start + width;
        size_t break_at = line.rfind(' ', end);
        if (break_at == std::string_view::npos || break_at <= start) {
            break_at = end;
        }
        wrapped.emplace_back(line.substr(start, break_at - start));
        start = break_at;
    }

    if (wrapped.empty()) {
        wrapped.emplace_back();
    }
    return wrapped;
}

void AppendInlineCodeBodyLine(std::vector<InlineAiBodyLine>* body,
                              std::string_view line,
                              size_t width,
                              LanguageId language,
                              SyntaxLineState line_state) {
    if (width == 0 || line.empty()) {
        body->push_back({.text = std::string(line),
                         .code = true,
                         .code_language = language,
                         .code_state = line_state,
                         .code_col_offset = 0});
        return;
    }

    for (size_t start = 0; start < line.size(); start += width) {
        body->push_back({.text = std::string(line),
                         .code = true,
                         .code_language = language,
                         .code_state = line_state,
                         .code_col_offset = start});
    }
}

std::vector<InlineAiBodyLine> WrappedInlineAiBody(const InlineAiSession& session, size_t content_cols) {
    const size_t body_width = InlineAiBodyWidth(content_cols);
    std::vector<InlineAiBodyLine> body;
    bool in_code_block = false;
    LanguageId code_language = LanguageId::PlainText;
    const ISyntaxHighlighter* code_highlighter = &HighlighterForLanguage(code_language);
    SyntaxLineState code_state = code_highlighter->InitialState();

    for (const std::string& line : SplitPlainLines(session.text)) {
        std::string_view fence_info;
        if (ParseCodeFence(line, &fence_info)) {
            if (in_code_block) {
                in_code_block = false;
                continue;
            }

            code_language = LanguageIdForCodeFenceInfo(fence_info);
            code_highlighter = &HighlighterForLanguage(code_language);
            code_state = code_highlighter->InitialState();
            in_code_block = true;
            continue;
        }

        if (in_code_block) {
            AppendInlineCodeBodyLine(&body, line, body_width, code_language, code_state);
            code_state = code_highlighter->HighlightLine(line, code_state, nullptr);
            continue;
        }

        const bool user_message = IsUserAiMessageLine(line);
        std::vector<std::string> wrapped = WrapPlainLine(line, body_width);
        for (std::string& wrapped_line : wrapped) {
            body.push_back({std::move(wrapped_line), user_message});
        }
    }
    if (body.empty()) {
        body.push_back({});
    }
    return body;
}

size_t InlineAiVisibleBodyRows(size_t body_rows) {
    return std::min(body_rows, kInlineAiMaxBodyRows);
}

size_t InlineAiBodyRowCountImpl(const EditorState& state, size_t content_cols) {
    if (state.activeView() != ViewKind::File || !state.inlineAiSession().has_value()) {
        return 0;
    }
    return WrappedInlineAiBody(*state.inlineAiSession(), content_cols).size();
}

size_t InlineAiVisibleBodyRowCountImpl(const EditorState& state, size_t content_cols) {
    return InlineAiVisibleBodyRows(InlineAiBodyRowCountImpl(state, content_cols));
}

size_t InlineAiRowCountImpl(const EditorState& state, size_t content_cols) {
    if (state.activeView() != ViewKind::File || !state.inlineAiSession().has_value()) {
        return 0;
    }
    return InlineAiVisibleBodyRowCountImpl(state, content_cols) + kInlineAiInputRows + 3;
}

size_t InlineAiRowsBetweenImpl(const EditorState& state,
                               size_t content_cols,
                               size_t start_row,
                               size_t end_row) {
    if (state.activeView() != ViewKind::File || !state.inlineAiSession().has_value() || start_row >= end_row) {
        return 0;
    }
    const size_t anchor_row = state.inlineAiSession()->anchor_row;
    if (anchor_row < start_row || anchor_row >= end_row) {
        return 0;
    }
    return InlineAiRowCountImpl(state, content_cols);
}

std::string InlineAiGutter(const Buffer& buffer) {
    return std::string(LineNumberDigits(buffer), ' ') + std::string(kGutterSeparator) + ' ';
}

size_t InlineCodeSliceWidth(std::string_view line, size_t col_offset, size_t width) {
    if (width == 0 || col_offset >= line.size()) {
        return 0;
    }
    return std::min(width, line.size() - col_offset);
}

std::string PadRenderedInlineText(std::string rendered, size_t visible_width, size_t target_width) {
    if (visible_width < target_width) {
        rendered.append(target_width - visible_width, ' ');
    }
    return rendered;
}

std::string RenderInlineAiBodyLine(const InlineAiBodyLine& line, size_t width) {
    if (line.code) {
        const ISyntaxHighlighter& highlighter = HighlighterForLanguage(line.code_language);
        std::string rendered =
            RenderHighlightedCode(line.text, line.code_col_offset, width, highlighter, line.code_state, nullptr);
        return PadRenderedInlineText(std::move(rendered),
                                     InlineCodeSliceWidth(line.text, line.code_col_offset, width),
                                     width);
    }

    std::string rendered = FitInlineText(line.text, width);
    if (line.user_message) {
        rendered = std::string("\x1b[1m") + rendered + "\x1b[22m";
    }
    return rendered;
}

std::vector<std::string> RenderInlineAiRows(const InlineAiSession& session,
                                            const std::optional<RateLimitSnapshotInfo>& rate_limits,
                                            size_t content_cols) {
    const std::vector<InlineAiBodyLine> body = WrappedInlineAiBody(session, content_cols);
    const size_t visible_body_rows = InlineAiVisibleBodyRows(body.size());
    const size_t max_scroll_row = body.size() > visible_body_rows ? body.size() - visible_body_rows : 0;
    const size_t scroll_row = std::min(session.scroll_row, max_scroll_row);
    std::vector<std::string> rows;
    rows.reserve(visible_body_rows + kInlineAiInputRows + 3);

    if (content_cols <= 2) {
        rows.push_back(std::string(kInlineAiBorderColor) + "┌┐" + std::string(ResetColorCode()));
        for (size_t index = 0; index < visible_body_rows + kInlineAiInputRows + 1; ++index) {
            rows.push_back(std::string(kInlineAiBorderColor) + "││" + std::string(ResetColorCode()));
        }
        rows.push_back(std::string(kInlineAiBorderColor) + "└┘" + std::string(ResetColorCode()));
        return rows;
    }

    const size_t inner_width = content_cols - 2;
    const size_t prefix_width = std::min<size_t>(2, inner_width);
    const std::string header_prefix = InlineAiHeaderPrefix(prefix_width);
    const size_t title_space = inner_width - prefix_width;
    const size_t title_width = std::min(session.title.size(), title_space);
    const std::string title = session.title.substr(0, title_width);
    const size_t header_used = prefix_width + title.size();
    const size_t meta_width = inner_width > header_used ? inner_width - header_used : 0;
    const std::string meta =
        FitInlineText(" | " + session.state_label + " | " + session.provider_name, meta_width, "─");
    rows.push_back(std::string(kInlineAiBorderColor) + "┌" + header_prefix +
                   std::string(DefaultForegroundCode()) + "\x1b[1m" + title + "\x1b[22m" +
                   std::string(kInlineAiBorderColor) + meta + "┐" + std::string(ResetColorCode()));

    for (size_t index = 0; index < visible_body_rows; ++index) {
        const InlineAiBodyLine& body_line = body[scroll_row + index];
        rows.push_back(std::string(kInlineAiBorderColor) + "│ " + std::string(ResetColorCode()) +
                       RenderInlineAiBodyLine(body_line, InlineAiBodyWidth(content_cols)) +
                       std::string(kInlineAiBorderColor) + " │" + std::string(ResetColorCode()));
    }

    rows.push_back(std::string(kInlineAiBorderColor) + "├" + FitInlineText("", inner_width, "─") + "┤" +
                   std::string(ResetColorCode()));

    const size_t input_width = InlineAiInputTextWidth(content_cols);
    const size_t input_start = InlineAiInputVisibleStart(session, input_width);
    const std::string visible_input =
        input_width == 0 ? "" : session.input_text.substr(input_start, input_width);
    rows.push_back(std::string(kInlineAiBorderColor) + "│ " + std::string(ResetColorCode()) +
                   FitInlineText("> " + visible_input, InlineAiBodyWidth(content_cols)) +
                   std::string(kInlineAiBorderColor) + " │" + std::string(ResetColorCode()));

    std::string footer_text;
    if (session.waiting) {
        footer_text = " Esc close | request running ";
    } else if (body.size() > visible_body_rows) {
        footer_text = " Up/Down " + std::to_string(scroll_row + 1) + "-" +
                      std::to_string(scroll_row + visible_body_rows) + "/" +
                      std::to_string(body.size()) + " | Esc close ";
    } else {
        footer_text = " Esc close ";
    }
    rows.push_back(std::string(kInlineAiBorderColor) + "└" +
                   FitInlineFooter(footer_text, FormatInlineRateLimits(rate_limits), inner_width) + "┘" +
                   std::string(ResetColorCode()));
    return rows;
}

AiDiffLineKind ClassifyAiDiffLine(std::string_view line, AiDiffPhase phase) {
    if (StartsWith(line, "@@")) {
        return AiDiffLineKind::HunkHeader;
    }
    if (StartsWith(line, "--- ")) {
        return AiDiffLineKind::FileHeaderOld;
    }
    if (StartsWith(line, "+++ ")) {
        return AiDiffLineKind::FileHeaderNew;
    }
    if (phase != AiDiffPhase::InHunk || line.empty()) {
        return AiDiffLineKind::Other;
    }

    switch (line.front()) {
        case ' ':
            return AiDiffLineKind::Context;
        case '+':
            return AiDiffLineKind::Add;
        case '-':
            return AiDiffLineKind::Remove;
        case '\\':
            return AiDiffLineKind::Meta;
        default:
            return AiDiffLineKind::Other;
    }
}

SyntaxTokenKind TokenKindAt(const std::vector<SyntaxSpan>& spans, size_t index) {
    for (auto span = spans.rbegin(); span != spans.rend(); ++span) {
        if (index >= span->start && index < span->end) {
            return span->kind;
        }
    }
    return SyntaxTokenKind::Default;
}

std::string_view BraceColorCodeForDepth(int depth) {
    switch (depth % 3) {
        case 0:
            return "\x1b[38;5;211m";
        case 1:
            return "\x1b[38;5;75m";
        case 2:
            return "\x1b[38;5;78m";
    }
    return "\x1b[38;5;211m";
}

bool IsOpeningDelimiter(char ch) {
    return ch == '{' || ch == '[' || ch == '(';
}

bool IsClosingDelimiter(char ch) {
    return ch == '}' || ch == ']' || ch == ')';
}

char MatchingDelimiter(char ch) {
    switch (ch) {
        case '{':
            return '}';
        case '}':
            return '{';
        case '[':
            return ']';
        case ']':
            return '[';
        case '(':
            return ')';
        case ')':
            return '(';
    }
    return '\0';
}

bool IsRainbowBrace(char ch, SyntaxTokenKind token_kind) {
    return token_kind == SyntaxTokenKind::Default && (IsOpeningDelimiter(ch) || IsClosingDelimiter(ch));
}

bool DelimitersMatch(char opening, char closing) {
    return IsOpeningDelimiter(opening) && MatchingDelimiter(opening) == closing;
}

bool IsBracePosition(const Cursor& cursor, size_t row, size_t col) {
    return cursor.row == row && cursor.col == col;
}

std::vector<SyntaxLineState> BuildLineStates(const Buffer& buffer, const ISyntaxHighlighter& highlighter) {
    std::vector<SyntaxLineState> states(buffer.lineCount() + 1, highlighter.InitialState());
    for (size_t row = 0; row < buffer.lineCount(); ++row) {
        states[row + 1] = highlighter.HighlightLine(buffer.line(row), states[row], nullptr);
    }
    return states;
}

std::optional<Cursor> FindMatchingBrace(const Buffer& buffer,
                                        const ISyntaxHighlighter& highlighter,
                                        const Cursor& cursor) {
    if (cursor.row >= buffer.lineCount()) {
        return std::nullopt;
    }

    const std::string& cursor_line = buffer.line(cursor.row);
    if (cursor.col >= cursor_line.size()) {
        return std::nullopt;
    }

    const std::vector<SyntaxLineState> line_states = BuildLineStates(buffer, highlighter);
    std::vector<SyntaxSpan> spans;
    highlighter.HighlightLine(cursor_line, line_states[cursor.row], &spans);
    const SyntaxTokenKind cursor_token_kind = TokenKindAt(spans, cursor.col);
    if (!IsRainbowBrace(cursor_line[cursor.col], cursor_token_kind)) {
        return std::nullopt;
    }

    const char cursor_delimiter = cursor_line[cursor.col];
    if (IsOpeningDelimiter(cursor_delimiter)) {
        const char matching_delimiter = MatchingDelimiter(cursor_delimiter);
        int depth = 0;
        for (size_t row = cursor.row; row < buffer.lineCount(); ++row) {
            spans.clear();
            const std::string& line = buffer.line(row);
            highlighter.HighlightLine(line, line_states[row], &spans);
            const size_t start = row == cursor.row ? cursor.col : 0;
            for (size_t col = start; col < line.size(); ++col) {
                const SyntaxTokenKind token_kind = TokenKindAt(spans, col);
                if (!IsRainbowBrace(line[col], token_kind)) {
                    continue;
                }
                if (line[col] == cursor_delimiter) {
                    ++depth;
                } else if (line[col] == matching_delimiter) {
                    --depth;
                    if (depth == 0) {
                        return Cursor{row, col};
                    }
                }
            }
        }
        return std::nullopt;
    }

    const char matching_delimiter = MatchingDelimiter(cursor_delimiter);
    int depth = 0;
    for (size_t row = cursor.row + 1; row-- > 0;) {
        spans.clear();
        const std::string& line = buffer.line(row);
        highlighter.HighlightLine(line, line_states[row], &spans);
        const size_t start = row == cursor.row ? cursor.col + 1 : line.size();
        for (size_t col = start; col-- > 0;) {
            const SyntaxTokenKind token_kind = TokenKindAt(spans, col);
            if (!IsRainbowBrace(line[col], token_kind)) {
                continue;
            }
            if (line[col] == cursor_delimiter) {
                ++depth;
            } else if (line[col] == matching_delimiter) {
                --depth;
                if (depth == 0) {
                    return Cursor{row, col};
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<char> AdvanceDelimiterStack(std::string_view line,
                                        const std::vector<SyntaxSpan>& highlights,
                                        std::vector<char> delimiter_stack) {
    for (size_t index = 0; index < line.size(); ++index) {
        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        if (!IsRainbowBrace(line[index], token_kind)) {
            continue;
        }

        if (IsOpeningDelimiter(line[index])) {
            delimiter_stack.push_back(line[index]);
        } else if (!delimiter_stack.empty()) {
            delimiter_stack.pop_back();
        }
    }
    return delimiter_stack;
}

std::string RenderHighlightedCode(std::string_view code,
                                  size_t col_offset,
                                  size_t cols,
                                  const ISyntaxHighlighter& highlighter,
                                  SyntaxLineState line_state,
                                  SyntaxLineState* next_line_state) {
    std::string rendered;
    rendered.reserve(cols + 16);

    std::vector<SyntaxSpan> highlights;
    const SyntaxLineState updated_state = highlighter.HighlightLine(code, line_state, &highlights);
    if (next_line_state != nullptr) {
        *next_line_state = updated_state;
    }

    if (col_offset >= code.size()) {
        return rendered;
    }

    const size_t end = std::min(code.size(), col_offset + cols);
    SyntaxTokenKind active_token_kind = SyntaxTokenKind::Default;
    for (size_t index = col_offset; index < end; ++index) {
        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        if (token_kind != active_token_kind) {
            rendered += ColorCodeForToken(token_kind);
            active_token_kind = token_kind;
        }

        const char ch = code[index];
        if (ch == '\t') {
            rendered.append("    ");
        } else if (static_cast<unsigned char>(ch) < 32) {
            rendered.push_back('?');
        } else {
            rendered.push_back(ch);
        }
    }

    if (active_token_kind != SyntaxTokenKind::Default) {
        rendered += std::string(ResetColorCode());
    }
    return rendered;
}

std::string DiffPrefixColor(AiDiffLineKind kind) {
    switch (kind) {
        case AiDiffLineKind::Add:
            return "\x1b[32m";
        case AiDiffLineKind::Remove:
            return "\x1b[31m";
        default:
            return {};
    }
}

AiDiffRenderState AdvanceAiDiffRenderState(std::string_view line,
                                           const ISyntaxHighlighter& highlighter,
                                           AiDiffRenderState state) {
    const AiDiffLineKind kind = ClassifyAiDiffLine(line, state.phase);

    switch (kind) {
        case AiDiffLineKind::FileHeaderOld:
            state.phase = AiDiffPhase::AwaitNewFileHeader;
            state.old_syntax_state = highlighter.InitialState();
            state.new_syntax_state = highlighter.InitialState();
            return state;
        case AiDiffLineKind::FileHeaderNew:
            state.phase = AiDiffPhase::AwaitHunkHeader;
            state.old_syntax_state = highlighter.InitialState();
            state.new_syntax_state = highlighter.InitialState();
            return state;
        case AiDiffLineKind::HunkHeader:
            state.phase = AiDiffPhase::InHunk;
            state.old_syntax_state = highlighter.InitialState();
            state.new_syntax_state = highlighter.InitialState();
            return state;
        case AiDiffLineKind::Context: {
            SyntaxLineState next_state = state.new_syntax_state;
            next_state = highlighter.HighlightLine(line.substr(1), next_state, nullptr);
            state.old_syntax_state = next_state;
            state.new_syntax_state = next_state;
            return state;
        }
        case AiDiffLineKind::Add:
            state.new_syntax_state = highlighter.HighlightLine(line.substr(1), state.new_syntax_state, nullptr);
            return state;
        case AiDiffLineKind::Remove:
            state.old_syntax_state = highlighter.HighlightLine(line.substr(1), state.old_syntax_state, nullptr);
            return state;
        case AiDiffLineKind::Meta:
            return state;
        case AiDiffLineKind::Other:
            if (state.phase == AiDiffPhase::InHunk && !line.empty()) {
                state.phase = AiDiffPhase::Outside;
                state.old_syntax_state = highlighter.InitialState();
                state.new_syntax_state = highlighter.InitialState();
            }
            return state;
    }

    return state;
}

AiDiffRenderState StateBeforeVisibleAiRow(const Buffer& buffer,
                                          const ISyntaxHighlighter& highlighter,
                                          size_t row) {
    AiDiffRenderState state;
    state.old_syntax_state = highlighter.InitialState();
    state.new_syntax_state = highlighter.InitialState();
    for (size_t index = 0; index < row && index < buffer.lineCount(); ++index) {
        state = AdvanceAiDiffRenderState(buffer.line(index), highlighter, state);
    }
    return state;
}

std::string RenderAiDiffCodeLine(std::string_view line,
                                 size_t col_offset,
                                 size_t cols,
                                 AiDiffLineKind kind,
                                 const ISyntaxHighlighter& highlighter,
                                 SyntaxLineState line_state,
                                 SyntaxLineState* next_line_state);

std::string RenderPatchPreviewLine(std::string_view line,
                                   size_t col_offset,
                                   size_t cols,
                                   const ISyntaxHighlighter& highlighter,
                                   const AiDiffRenderState& state_before,
                                   AiDiffRenderState* state_after) {
    const AiDiffLineKind kind = ClassifyAiDiffLine(line, state_before.phase);
    std::string rendered;

    switch (kind) {
        case AiDiffLineKind::HunkHeader:
            rendered = DecoratePatchLine(RenderVisibleText(line, col_offset, cols));
            break;
        case AiDiffLineKind::Context: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderHighlightedCode(line.substr(1), col_offset > 0 ? col_offset - 1 : 0, cols, highlighter,
                                             state_before.new_syntax_state, &next_state);
            if (col_offset == 0) {
                rendered = " " + rendered;
            }
            break;
        }
        case AiDiffLineKind::Add: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.new_syntax_state,
                                            &next_state);
            break;
        }
        case AiDiffLineKind::Remove:
        case AiDiffLineKind::Meta:
        case AiDiffLineKind::Other:
        case AiDiffLineKind::FileHeaderOld:
        case AiDiffLineKind::FileHeaderNew:
            rendered = DecoratePatchLine(RenderVisibleText(line, col_offset, cols));
            break;
    }

    if (state_after != nullptr) {
        *state_after = AdvanceAiDiffRenderState(line, highlighter, state_before);
    }
    return rendered;
}

std::string RenderAiDiffCodeLine(std::string_view line,
                                 size_t col_offset,
                                 size_t cols,
                                 AiDiffLineKind kind,
                                 const ISyntaxHighlighter& highlighter,
                                 SyntaxLineState line_state,
                                 SyntaxLineState* next_line_state) {
    if (line.empty() || cols == 0) {
        if (next_line_state != nullptr) {
            *next_line_state = line_state;
        }
        return {};
    }

    const bool show_prefix = col_offset == 0;
    const size_t code_col_offset = col_offset == 0 ? 0 : col_offset - 1;
    const size_t code_cols = cols > 0 ? cols - (show_prefix ? 1 : 0) : 0;

    std::string rendered;
    rendered.reserve(cols + 16);
    if (show_prefix) {
        const std::string prefix_color = DiffPrefixColor(kind);
        if (!prefix_color.empty()) {
            rendered += prefix_color;
        }
        rendered.push_back(line.front());
        if (!prefix_color.empty()) {
            rendered += std::string(ResetColorCode());
        }
    }

    rendered += RenderHighlightedCode(line.substr(1), code_col_offset, code_cols, highlighter, line_state, next_line_state);
    return rendered;
}

std::string RenderAiScratchLine(std::string_view line,
                                size_t col_offset,
                                size_t cols,
                                const ISyntaxHighlighter& highlighter,
                                const AiDiffRenderState& state_before,
                                AiDiffRenderState* state_after) {
    const AiDiffLineKind kind = ClassifyAiDiffLine(line, state_before.phase);
    std::string rendered;

    switch (kind) {
        case AiDiffLineKind::HunkHeader:
        case AiDiffLineKind::FileHeaderOld:
        case AiDiffLineKind::FileHeaderNew:
            rendered = DecoratePatchLine(RenderVisibleText(line, col_offset, cols));
            break;
        case AiDiffLineKind::Context: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.new_syntax_state, &next_state);
            break;
        }
        case AiDiffLineKind::Add: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.new_syntax_state, &next_state);
            break;
        }
        case AiDiffLineKind::Remove: {
            SyntaxLineState next_state = state_before.old_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.old_syntax_state, &next_state);
            break;
        }
        case AiDiffLineKind::Meta:
        case AiDiffLineKind::Other:
            rendered = RenderVisibleText(line, col_offset, cols);
            break;
    }

    if (state_after != nullptr) {
        *state_after = AdvanceAiDiffRenderState(line, highlighter, state_before);
    }
    if (IsUserAiMessageLine(line)) {
        rendered = std::string("\x1b[1m") + rendered + "\x1b[22m";
    }
    return rendered;
}

FileRenderState StateBeforeVisibleRow(const Buffer& buffer,
                                      const ISyntaxHighlighter& highlighter,
                                      size_t row) {
    FileRenderState state{
        .syntax_state = highlighter.InitialState(),
    };
    std::vector<SyntaxSpan> scratch;
    for (size_t index = 0; index < row && index < buffer.lineCount(); ++index) {
        state.syntax_state = highlighter.HighlightLine(buffer.line(index), state.syntax_state, &scratch);
        state.delimiter_stack = AdvanceDelimiterStack(buffer.line(index), scratch, std::move(state.delimiter_stack));
    }
    return state;
}

std::string RenderFileLine(const EditorState& state,
                           const ISyntaxHighlighter& highlighter,
                           const std::string& line,
                           size_t row,
                           size_t col_offset,
                           size_t cols,
                           SyntaxLineState line_state,
                           std::vector<char> delimiter_stack,
                           const std::optional<Cursor>& active_brace,
                           const std::optional<Cursor>& matching_brace,
                           SyntaxLineState* next_line_state,
                           std::vector<char>* next_delimiter_stack) {
    std::string rendered;
    rendered.reserve(cols + 16);

    std::vector<SyntaxSpan> highlights;
    const SyntaxLineState updated_state = highlighter.HighlightLine(line, line_state, &highlights);
    if (next_line_state != nullptr) {
        *next_line_state = updated_state;
    }

    if (col_offset >= line.size()) {
        if (next_delimiter_stack != nullptr) {
            *next_delimiter_stack = AdvanceDelimiterStack(line, highlights, std::move(delimiter_stack));
        }
        return rendered;
    }

    const size_t end = std::min(line.size(), col_offset + cols);
    bool selection_background = false;
    std::string_view active_color_code = ResetColorCode();
    bool active_bold = false;
    bool active_diagnostic_underline = false;
    for (size_t index = 0; index < line.size(); ++index) {
        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        std::string_view desired_color_code = ColorCodeForToken(token_kind);
        if (IsRainbowBrace(line[index], token_kind)) {
            if (IsClosingDelimiter(line[index])) {
                const int color_depth =
                    delimiter_stack.empty() ? 0 : static_cast<int>(delimiter_stack.size()) - 1;
                desired_color_code = BraceColorCodeForDepth(color_depth);
                if (!delimiter_stack.empty()) {
                    delimiter_stack.pop_back();
                }
            } else {
                desired_color_code = BraceColorCodeForDepth(static_cast<int>(delimiter_stack.size()));
                delimiter_stack.push_back(line[index]);
            }
        }

        if (index < col_offset || index >= end) {
            continue;
        }

        const bool selected = IsPositionSelected(state.selection(), row, index);
        if (selected && !selection_background) {
            if (!active_diagnostic_underline) {
                rendered += kSelectionBackground;
            }
            selection_background = true;
        } else if (!selected && selection_background) {
            if (!active_diagnostic_underline) {
                rendered += ResetBackgroundCode();
            }
            selection_background = false;
        }

        const bool desired_bold =
            (active_brace.has_value() && IsBracePosition(*active_brace, row, index)) ||
            (matching_brace.has_value() && IsBracePosition(*matching_brace, row, index));
        const bool diagnostic_underline = HasErrorDiagnosticAt(state.diagnostics(), row, index);
        if (desired_color_code != active_color_code) {
            rendered += desired_color_code;
            active_color_code = desired_color_code;
        }
        if (desired_bold && !active_bold) {
            rendered += "\x1b[1m";
            active_bold = true;
        } else if (!desired_bold && active_bold) {
            rendered += "\x1b[22m";
            active_bold = false;
        }
        if (diagnostic_underline && !active_diagnostic_underline) {
            rendered += kDiagnosticBackground;
            rendered += kDiagnosticUnderline;
            active_diagnostic_underline = true;
        } else if (!diagnostic_underline && active_diagnostic_underline) {
            rendered += kResetUnderline;
            rendered += selection_background ? kSelectionBackground : ResetBackgroundCode();
            active_diagnostic_underline = false;
        }

        const char ch = line[index];
        if (ch == '\t') {
            rendered.append("    ");
        } else if (static_cast<unsigned char>(ch) < 32) {
            rendered.push_back('?');
        } else {
            rendered.push_back(ch);
        }
    }
    if (selection_background) {
        rendered += ResetBackgroundCode();
    }
    if (active_bold) {
        rendered += "\x1b[22m";
    }
    if (active_diagnostic_underline) {
        rendered += kResetUnderline;
        rendered += ResetBackgroundCode();
    }
    if (active_color_code != ResetColorCode()) {
        rendered += std::string(ResetColorCode());
    }
    if (next_delimiter_stack != nullptr) {
        *next_delimiter_stack = std::move(delimiter_stack);
    }
    return rendered;
}

std::string ActiveViewLabel(ViewKind view) {
    switch (view) {
        case ViewKind::File:
            return "FILE";
        case ViewKind::AiScratch:
            return "AI";
        case ViewKind::PatchPreview:
            return "PATCH";
        case ViewKind::BuildOutput:
            return "BUILD";
    }
    return "FILE";
}

std::string CompletionItemText(const CompletionItem& item) {
    std::string text = item.label;
    if (!item.detail.empty()) {
        text += "  " + item.detail;
    }
    for (char& ch : text) {
        if (static_cast<unsigned char>(ch) < 32) {
            ch = ' ';
        }
    }
    return text;
}

void RenderCompletionPopup(std::ostringstream& output,
                           const EditorState& state,
                           const Viewport& viewport,
                           int content_rows,
                           int cols,
                           size_t gutter_width) {
    const CompletionSession& session = state.completionSession();
    if (!session.active || state.activeView() != ViewKind::File || content_rows <= 0 || cols <= 0) {
        return;
    }

    std::vector<std::string> rows;
    if (session.waiting || session.items.empty()) {
        rows.push_back(session.message.empty() ? "Completing..." : session.message);
    } else {
        const size_t visible_count = std::min<size_t>(8, session.items.size());
        const size_t first =
            session.selected >= visible_count ? session.selected - visible_count + 1 : size_t{0};
        for (size_t index = first; index < session.items.size() && rows.size() < visible_count; ++index) {
            rows.push_back(CompletionItemText(session.items[index]));
        }
    }

    if (rows.empty()) {
        return;
    }

    size_t width = 1;
    for (const std::string& row : rows) {
        width = std::max(width, row.size());
    }
    width = std::min<size_t>(std::max<size_t>(width, 16), 60);
    width = std::min(width, static_cast<size_t>(cols));

    size_t visual_cursor_row =
        state.fileCursor().row >= viewport.row_offset ? state.fileCursor().row - viewport.row_offset : 0;
    const size_t content_cols =
        static_cast<size_t>(cols) > gutter_width ? static_cast<size_t>(cols) - gutter_width : 1;
    visual_cursor_row += InlineAiRowsBetweenImpl(state, content_cols, viewport.row_offset, state.fileCursor().row);
    visual_cursor_row = std::min<size_t>(visual_cursor_row, static_cast<size_t>(content_rows - 1));
    size_t popup_row = visual_cursor_row + 2;
    if (popup_row + rows.size() - 1 > static_cast<size_t>(content_rows)) {
        popup_row = visual_cursor_row > rows.size() ? visual_cursor_row - rows.size() + 1 : 1;
    }

    size_t popup_col = gutter_width + 1;
    if (state.fileCursor().col >= viewport.col_offset) {
        popup_col = gutter_width + state.fileCursor().col - viewport.col_offset + 1;
    }
    if (popup_col == 0 || popup_col > static_cast<size_t>(cols)) {
        popup_col = 1;
    }
    if (popup_col + width - 1 > static_cast<size_t>(cols)) {
        popup_col = static_cast<size_t>(cols) - width + 1;
    }

    const size_t selected_visible =
        session.selected >= rows.size() ? rows.size() - 1 : session.selected;
    for (size_t index = 0; index < rows.size(); ++index) {
        std::string visible = rows[index];
        if (visible.size() > width) {
            visible.resize(width);
        }
        if (visible.size() < width) {
            visible += std::string(width - visible.size(), ' ');
        }
        output << "\x1b[" << (popup_row + index) << ";" << popup_col << "H";
        if (!session.waiting && !session.items.empty() && index == selected_visible) {
            output << "\x1b[7m" << visible << "\x1b[27m";
        } else {
            output << "\x1b[48;5;236m" << visible << ResetBackgroundCode();
        }
    }
}

void RenderDiagnosticBubble(std::ostringstream& output,
                            const EditorState& state,
                            const Viewport& viewport,
                            int content_rows,
                            int cols,
                            size_t gutter_width) {
    if (state.activeView() != ViewKind::File || content_rows <= 0 || cols <= 0 ||
        state.completionSession().active) {
        return;
    }

    const Cursor& cursor = state.fileCursor();
    const Diagnostic* diagnostic = ErrorDiagnosticAt(state.diagnostics(), cursor.row, cursor.col);
    if (diagnostic == nullptr || diagnostic->message.empty()) {
        diagnostic = cursor.col == 0 ? nullptr : ErrorDiagnosticAt(state.diagnostics(), cursor.row, cursor.col - 1);
    }
    if (diagnostic == nullptr || diagnostic->message.empty()) {
        return;
    }

    const std::string source = diagnostic->source.empty() ? "LSP" : diagnostic->source;
    std::string text = source + ": " + diagnostic->message;
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t' || static_cast<unsigned char>(ch) < 32) {
            ch = ' ';
        }
    }

    const size_t width = std::min<size_t>(std::max<size_t>(text.size(), 18), std::min<size_t>(72, cols));
    if (text.size() > width) {
        text.resize(width);
    }
    if (text.size() < width) {
        text += std::string(width - text.size(), ' ');
    }

    size_t visual_cursor_row =
        cursor.row >= viewport.row_offset ? cursor.row - viewport.row_offset : 0;
    const size_t content_cols =
        static_cast<size_t>(cols) > gutter_width ? static_cast<size_t>(cols) - gutter_width : 1;
    visual_cursor_row += InlineAiRowsBetweenImpl(state, content_cols, viewport.row_offset, cursor.row);
    visual_cursor_row = std::min<size_t>(visual_cursor_row, static_cast<size_t>(content_rows - 1));
    size_t bubble_row = visual_cursor_row + 2;
    if (bubble_row > static_cast<size_t>(content_rows)) {
        bubble_row = visual_cursor_row > 0 ? visual_cursor_row : 1;
    }

    size_t bubble_col = gutter_width + 1;
    if (cursor.col >= viewport.col_offset) {
        bubble_col = gutter_width + cursor.col - viewport.col_offset + 2;
    }
    if (bubble_col == 0 || bubble_col > static_cast<size_t>(cols)) {
        bubble_col = 1;
    }
    if (bubble_col + width - 1 > static_cast<size_t>(cols)) {
        bubble_col = static_cast<size_t>(cols) - width + 1;
    }

    output << "\x1b[" << bubble_row << ";" << bubble_col << "H"
           << "\x1b[48;5;52m" << DefaultForegroundCode() << text << ResetColorCode()
           << ResetBackgroundCode();
}

}  // namespace

size_t Screen::ContentColumns(const EditorState& state, int total_cols) const {
    const size_t gutter_width = ShowsLineNumbers(state) ? GutterWidth(state.activeBuffer()) : 0;
    if (total_cols <= 0) {
        return 1;
    }
    return std::max<size_t>(1, static_cast<size_t>(total_cols) > gutter_width
                                   ? static_cast<size_t>(total_cols) - gutter_width
                                   : 1);
}

size_t Screen::InlineAiBodyRowCount(const EditorState& state, size_t content_cols) const {
    return InlineAiBodyRowCountImpl(state, content_cols);
}

size_t Screen::InlineAiVisibleBodyRowCount(const EditorState& state, size_t content_cols) const {
    return InlineAiVisibleBodyRowCountImpl(state, content_cols);
}

size_t Screen::InlineAiRowCount(const EditorState& state, size_t content_cols) const {
    return InlineAiRowCountImpl(state, content_cols);
}

size_t Screen::InlineAiRowsBetween(const EditorState& state,
                                   size_t content_cols,
                                   size_t start_row,
                                   size_t end_row) const {
    return InlineAiRowsBetweenImpl(state, content_cols, start_row, end_row);
}

std::string Screen::Render(const EditorState& state,
                           const RenderOptions& options,
                           int rows,
                           int cols) const {
    const Buffer& buffer = state.activeBuffer();
    const Viewport& viewport = state.activeViewport();
    const int content_rows = std::max(1, rows - 2);
    const size_t gutter_width = ShowsLineNumbers(state) ? GutterWidth(buffer) : 0;
    const size_t content_cols = ContentColumns(state, cols);
    std::ostringstream output;
    output << "\x1b[?25l";
    output << DefaultBackgroundCode() << DefaultForegroundCode();
    output << "\x1b[H\x1b[J";

    const ISyntaxHighlighter& highlighter = HighlighterForLanguage(state.fileBuffer().languageId());
    const GitLineStatus* git_status = ShowsLineNumbers(state) ? &GitStatusForBuffer(buffer) : nullptr;
    const std::optional<Cursor> matching_brace =
        state.activeView() == ViewKind::File ? FindMatchingBrace(state.fileBuffer(), highlighter, state.fileCursor())
                                             : std::nullopt;
    FileRenderState file_render_state{
        .syntax_state = highlighter.InitialState(),
    };
    AiDiffRenderState ai_diff_state;
    if (state.activeView() == ViewKind::File) {
        file_render_state = StateBeforeVisibleRow(buffer, highlighter, viewport.row_offset);
    } else if (state.activeView() == ViewKind::AiScratch || state.activeView() == ViewKind::PatchPreview) {
        ai_diff_state = StateBeforeVisibleAiRow(buffer, highlighter, viewport.row_offset);
    }

    auto end_screen_row = [&](int screen_row) {
        output << "\x1b[K";
        if (screen_row < content_rows - 1) {
            output << "\r\n";
        }
    };

    int screen_row = 0;
    size_t file_row = viewport.row_offset;
    while (screen_row < content_rows) {
        if (file_row >= buffer.lineCount()) {
            if (ShowsLineNumbers(state)) {
                output << std::string(LineNumberDigits(buffer), ' ') << kGutterSeparator << ' ';
            }
            output << "~";
        } else {
            if (ShowsLineNumbers(state)) {
                output << RenderLineNumber(file_row, buffer, *git_status);
            }
            const std::string line = EscapeLine(buffer.line(file_row));
            if (state.activeView() == ViewKind::File) {
                SyntaxLineState next_line_state = file_render_state.syntax_state;
                std::vector<char> next_delimiter_stack = file_render_state.delimiter_stack;
                output << RenderFileLine(state,
                                         highlighter,
                                         buffer.line(file_row),
                                         file_row,
                                         viewport.col_offset,
                                         content_cols,
                                         file_render_state.syntax_state,
                                         file_render_state.delimiter_stack,
                                         matching_brace.has_value() ? std::optional<Cursor>(state.fileCursor())
                                                                    : std::nullopt,
                                         matching_brace,
                                         &next_line_state,
                                         &next_delimiter_stack);
                file_render_state.syntax_state = next_line_state;
                file_render_state.delimiter_stack = std::move(next_delimiter_stack);
            } else if (state.activeView() == ViewKind::AiScratch) {
                AiDiffRenderState next_ai_diff_state = ai_diff_state;
                output << RenderAiScratchLine(buffer.line(file_row),
                                              viewport.col_offset,
                                              content_cols,
                                              highlighter,
                                              ai_diff_state,
                                              &next_ai_diff_state);
                ai_diff_state = next_ai_diff_state;
            } else if (state.activeView() == ViewKind::PatchPreview) {
                AiDiffRenderState next_ai_diff_state = ai_diff_state;
                output << RenderPatchPreviewLine(buffer.line(file_row),
                                                viewport.col_offset,
                                                content_cols,
                                                highlighter,
                                                ai_diff_state,
                                                &next_ai_diff_state);
                ai_diff_state = next_ai_diff_state;
            } else if (viewport.col_offset < line.size()) {
                std::string visible = line.substr(viewport.col_offset, content_cols);
                if (options.file_picker_mode && state.activeView() == ViewKind::BuildOutput &&
                    file_row == options.file_picker_selected + 1) {
                    output << "\x1b[7m" << visible << "\x1b[27m";
                } else {
                    output << visible;
                }
            }
        }
        end_screen_row(screen_row);
        ++screen_row;

        if (state.activeView() == ViewKind::File && file_row < buffer.lineCount()) {
            const std::vector<std::string>& previous_lines =
                ExpandedPreviousLinesForRow(state, *git_status, file_row);
            for (const std::string& previous_line : previous_lines) {
                if (screen_row >= content_rows) {
                    break;
                }
                output << RenderGitPreviousLine(buffer, previous_line, viewport.col_offset, content_cols);
                end_screen_row(screen_row);
                ++screen_row;
            }
        }
        if (state.activeView() == ViewKind::File && state.inlineAiSession().has_value() &&
            state.inlineAiSession()->anchor_row == file_row) {
            const std::string gutter = InlineAiGutter(buffer);
            for (const std::string& inline_row :
                 RenderInlineAiRows(*state.inlineAiSession(), state.aiRateLimits(), content_cols)) {
                if (screen_row >= content_rows) {
                    break;
                }
                output << gutter << inline_row;
                end_screen_row(screen_row);
                ++screen_row;
            }
        }
        ++file_row;
    }

    RenderCompletionPopup(output, state, viewport, content_rows, cols, gutter_width);
    RenderDiagnosticBubble(output, state, viewport, content_rows, cols, gutter_width);

    const Cursor& file_cursor = state.fileCursor();
    const std::string modified = state.fileBuffer().dirty() ? "modified" : "saved";
    std::string status = state.activeBuffer().name() + " | " + state.fileBuffer().guessLanguage() + " | " +
                         modified + " | AI " + state.aiProviderName() + " | " +
                         ActiveViewLabel(state.activeView()) + " | Ln " +
                         std::to_string(file_cursor.row + 1) + ", Col " + std::to_string(file_cursor.col + 1);
    if (!state.aiRequestState().empty()) {
        status += " | " + state.aiRequestState();
    }
    if (status.size() > static_cast<size_t>(cols)) {
        status.resize(static_cast<size_t>(cols));
    }

    const int status_row = std::max(1, rows - 1);
    const int message_row = std::max(1, rows);
    output << "\x1b[" << status_row << ";1H\x1b[7m" << status;
    if (status.size() < static_cast<size_t>(cols)) {
        output << std::string(static_cast<size_t>(cols) - status.size(), ' ');
    }
    output << "\x1b[27m" << ResetColorCode() << ResetBackgroundCode();

    output << "\x1b[" << message_row << ";1H";
    if (options.command_mode) {
        std::string command_line = ":" + options.command_input;
        if (command_line.size() > static_cast<size_t>(cols)) {
            command_line = command_line.substr(command_line.size() - static_cast<size_t>(cols));
        }
        output << command_line;
    } else if (options.file_picker_mode) {
        std::string picker_line = "Open file: " + options.file_picker_query;
        if (picker_line.size() > static_cast<size_t>(cols)) {
            picker_line = picker_line.substr(picker_line.size() - static_cast<size_t>(cols));
        }
        output << picker_line;
    } else {
        const std::string message = state.statusText();
        if (!message.empty()) {
            output << message.substr(0, static_cast<size_t>(cols));
        }
    }
    output << "\x1b[K";

    size_t cursor_row = 1;
    size_t cursor_col = 1;
    if (options.command_mode) {
        cursor_row = static_cast<size_t>(rows);
        cursor_col = std::min(static_cast<size_t>(cols), options.command_input.size() + 2);
    } else if (options.file_picker_mode) {
        cursor_row = static_cast<size_t>(rows);
        cursor_col = std::min(static_cast<size_t>(cols), std::string("Open file: ").size() + options.file_picker_query.size() + 1);
    } else if (state.activeView() == ViewKind::File && state.inlineAiSession().has_value() &&
               state.inlineAiSession()->focused && git_status != nullptr) {
        const InlineAiSession& session = *state.inlineAiSession();
        const size_t visible_rows = InlineAiVisibleBodyRowCountImpl(state, content_cols);

        size_t visual_cursor_row =
            session.anchor_row >= viewport.row_offset ? session.anchor_row - viewport.row_offset : 0;
        if (session.anchor_row >= viewport.row_offset) {
            visual_cursor_row += ExpandedGitRowsBefore(state, *git_status, viewport.row_offset, session.anchor_row);
            visual_cursor_row += ExpandedPreviousLinesForRow(state, *git_status, session.anchor_row).size();
        }
        visual_cursor_row += visible_rows + 3;

        cursor_row = std::min(static_cast<size_t>(content_rows), visual_cursor_row + 1);
        const size_t input_width = InlineAiInputTextWidth(content_cols);
        const size_t input_start = InlineAiInputVisibleStart(session, input_width);
        const size_t input_offset =
            session.input_cursor_col >= input_start ? session.input_cursor_col - input_start : 0;
        cursor_col = std::min(static_cast<size_t>(cols),
                              gutter_width + (content_cols <= 4 ? 1 : 5 + input_offset));
    } else {
        size_t visual_cursor_row =
            state.activeViewport().cursor.row >= viewport.row_offset
                ? state.activeViewport().cursor.row - viewport.row_offset
                : 0;
        if (state.activeView() == ViewKind::File && git_status != nullptr &&
            state.activeViewport().cursor.row >= viewport.row_offset) {
            visual_cursor_row +=
                ExpandedGitRowsBefore(state, *git_status, viewport.row_offset, state.activeViewport().cursor.row);
            visual_cursor_row +=
                InlineAiRowsBetweenImpl(state, content_cols, viewport.row_offset, state.activeViewport().cursor.row);
        }
        cursor_row = std::min(static_cast<size_t>(content_rows), visual_cursor_row + 1);
        cursor_col =
            std::min(static_cast<size_t>(cols), gutter_width + state.activeViewport().cursor.col - viewport.col_offset + 1);
    }

    output << "\x1b[" << cursor_row << ";" << cursor_col << "H";
    output << "\x1b[?25h";
    return output.str();
}

}  // namespace flowstate
