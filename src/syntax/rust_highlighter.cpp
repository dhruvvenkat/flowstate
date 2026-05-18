#include "syntax/rust_highlighter.h"

#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace flowstate {

namespace {

enum class RustScanMode : uint64_t {
    None = 0,
    BlockComment = 1,
    QuotedString = 2,
    RawString = 3,
};

constexpr uint64_t kModeMask = 0x7;

constexpr std::array<std::string_view, 47> kKeywords = {
    "as",       "async",   "await",  "break",  "const",  "continue", "crate",   "dyn",
    "else",     "enum",    "extern", "false",  "fn",     "for",      "if",      "impl",
    "in",       "let",     "loop",   "match",  "mod",    "move",     "mut",     "pub",
    "ref",      "return",  "self",   "Self",   "static", "struct",   "super",   "trait",
    "true",     "type",    "unsafe", "use",    "where",  "while",    "yield",   "union",
    "macro",    "macro_rules", "abstract", "become", "box", "do", "final",
};

constexpr std::array<std::string_view, 18> kTypeKeywords = {
    "bool", "char",  "str",    "i8",     "i16",   "i32",   "i64",   "i128", "isize",
    "u8",   "u16",   "u32",    "u64",    "u128",  "usize", "f32",   "f64",  "Self",
};

constexpr std::array<std::string_view, 6> kTypeDeclarators = {
    "enum",
    "impl",
    "struct",
    "trait",
    "type",
    "union",
};

constexpr std::array<std::string_view, 1> kFunctionDeclarators = {
    "fn",
};

template <size_t N>
bool ContainsWord(const std::array<std::string_view, N>& words, std::string_view token) {
    for (std::string_view word : words) {
        if (word == token) {
            return true;
        }
    }
    return false;
}

SyntaxLineState EncodeState(RustScanMode mode, uint64_t data = 0) {
    return {.value = (data << 3) | static_cast<uint64_t>(mode)};
}

RustScanMode DecodeMode(SyntaxLineState state) {
    return static_cast<RustScanMode>(state.value & kModeMask);
}

uint64_t DecodeData(SyntaxLineState state) {
    return state.value >> 3;
}

bool IsIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

bool IsIdentifierBody(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

size_t SkipWhitespace(std::string_view line, size_t index) {
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
        ++index;
    }
    return index;
}

size_t ScanIdentifierBody(std::string_view line, size_t index) {
    while (index < line.size() && IsIdentifierBody(line[index])) {
        ++index;
    }
    return index;
}

struct RustIdentifier {
    size_t start = 0;
    size_t end = 0;
    std::string_view logical;
};

std::optional<RustIdentifier> ScanRustIdentifier(std::string_view line, size_t index) {
    if (index >= line.size()) {
        return std::nullopt;
    }

    if (line.compare(index, 2, "r#") == 0 && index + 2 < line.size() && IsIdentifierStart(line[index + 2])) {
        const size_t end = ScanIdentifierBody(line, index + 2);
        return RustIdentifier{.start = index, .end = end, .logical = line.substr(index + 2, end - index - 2)};
    }

    if (!IsIdentifierStart(line[index])) {
        return std::nullopt;
    }

    const size_t end = ScanIdentifierBody(line, index);
    return RustIdentifier{.start = index, .end = end, .logical = line.substr(index, end - index)};
}

bool LooksLikeTypeName(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    return std::isupper(static_cast<unsigned char>(token.front())) != 0;
}

void AppendSpan(std::vector<SyntaxSpan>* spans, size_t start, size_t end, SyntaxTokenKind kind) {
    if (spans == nullptr || start >= end) {
        return;
    }
    spans->push_back({.start = start, .end = end, .kind = kind});
}

std::optional<size_t> FindEscapedQuoteEnd(std::string_view line, size_t quote_index, char quote) {
    size_t index = quote_index + 1;
    while (index < line.size()) {
        if (line[index] == '\\' && index + 1 < line.size()) {
            index += 2;
            continue;
        }
        if (line[index] == quote) {
            return index + 1;
        }
        ++index;
    }
    return std::nullopt;
}

bool MatchRawStringStart(std::string_view line, size_t index, size_t* prefix_length, size_t* hash_count) {
    if (prefix_length == nullptr || hash_count == nullptr || index >= line.size()) {
        return false;
    }

    size_t cursor = index;
    if (line[cursor] == 'b') {
        ++cursor;
    }
    if (cursor >= line.size() || line[cursor] != 'r') {
        return false;
    }
    ++cursor;

    size_t hashes = 0;
    while (cursor < line.size() && line[cursor] == '#') {
        ++hashes;
        ++cursor;
    }

    if (cursor >= line.size() || line[cursor] != '"') {
        return false;
    }

    *prefix_length = cursor - index + 1;
    *hash_count = hashes;
    return true;
}

std::optional<size_t> FindRawStringEnd(std::string_view line, size_t search_index, size_t hash_count) {
    size_t cursor = search_index;
    while (true) {
        cursor = line.find('"', cursor);
        if (cursor == std::string_view::npos) {
            return std::nullopt;
        }

        if (cursor + hash_count >= line.size()) {
            return std::nullopt;
        }

        bool matched = true;
        for (size_t index = 0; index < hash_count; ++index) {
            if (line[cursor + 1 + index] != '#') {
                matched = false;
                break;
            }
        }
        if (matched) {
            return cursor + 1 + hash_count;
        }
        ++cursor;
    }
}

std::optional<size_t> ScanRustCharLiteral(std::string_view line, size_t index) {
    size_t quote_index = index;
    if (line[index] == 'b') {
        if (index + 1 >= line.size() || line[index + 1] != '\'') {
            return std::nullopt;
        }
        quote_index = index + 1;
    } else if (line[index] != '\'') {
        return std::nullopt;
    }

    size_t cursor = quote_index + 1;
    if (cursor >= line.size()) {
        return std::nullopt;
    }

    if (line[cursor] == '\\') {
        ++cursor;
        if (cursor >= line.size()) {
            return std::nullopt;
        }
        if (line[cursor] == 'u') {
            ++cursor;
            if (cursor >= line.size() || line[cursor] != '{') {
                return std::nullopt;
            }
            ++cursor;
            while (cursor < line.size() && line[cursor] != '}') {
                ++cursor;
            }
            if (cursor >= line.size()) {
                return std::nullopt;
            }
            ++cursor;
        } else if (line[cursor] == 'x') {
            ++cursor;
            if (cursor + 1 >= line.size() || !std::isxdigit(static_cast<unsigned char>(line[cursor])) ||
                !std::isxdigit(static_cast<unsigned char>(line[cursor + 1]))) {
                return std::nullopt;
            }
            cursor += 2;
        } else {
            ++cursor;
        }
    } else {
        if (line[cursor] == '\'') {
            return std::nullopt;
        }
        ++cursor;
    }

    if (cursor < line.size() && line[cursor] == '\'') {
        return cursor + 1;
    }
    return std::nullopt;
}

bool IsNumberStart(std::string_view line, size_t index) {
    if (index >= line.size() || !std::isdigit(static_cast<unsigned char>(line[index]))) {
        return false;
    }
    if (index > 0) {
        const char previous = line[index - 1];
        if (IsIdentifierBody(previous) || previous == '.') {
            return false;
        }
    }
    return true;
}

size_t ScanDecimalDigits(std::string_view line, size_t index) {
    while (index < line.size() &&
           (std::isdigit(static_cast<unsigned char>(line[index])) || line[index] == '_')) {
        ++index;
    }
    return index;
}

size_t ScanBaseDigits(std::string_view line, size_t index, int base) {
    while (index < line.size()) {
        const char ch = line[index];
        if (ch == '_') {
            ++index;
            continue;
        }
        const bool valid =
            (base == 16 && std::isxdigit(static_cast<unsigned char>(ch))) ||
            (base == 10 && std::isdigit(static_cast<unsigned char>(ch))) ||
            (base == 8 && ch >= '0' && ch <= '7') ||
            (base == 2 && (ch == '0' || ch == '1'));
        if (!valid) {
            break;
        }
        ++index;
    }
    return index;
}

size_t ScanNumericSuffix(std::string_view line, size_t index) {
    while (index < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[index])) || line[index] == '_')) {
        ++index;
    }
    return index;
}

size_t ScanNumberLiteral(std::string_view line, size_t index) {
    if (index + 1 < line.size() && line[index] == '0') {
        const char prefix = line[index + 1];
        if (prefix == 'x' || prefix == 'X') {
            index = ScanBaseDigits(line, index + 2, 16);
            return ScanNumericSuffix(line, index);
        }
        if (prefix == 'o' || prefix == 'O') {
            index = ScanBaseDigits(line, index + 2, 8);
            return ScanNumericSuffix(line, index);
        }
        if (prefix == 'b' || prefix == 'B') {
            index = ScanBaseDigits(line, index + 2, 2);
            return ScanNumericSuffix(line, index);
        }
    }

    index = ScanDecimalDigits(line, index);
    if (index < line.size() && line[index] == '.' && index + 1 < line.size() &&
        std::isdigit(static_cast<unsigned char>(line[index + 1]))) {
        ++index;
        index = ScanDecimalDigits(line, index);
    }
    if (index < line.size() && (line[index] == 'e' || line[index] == 'E')) {
        ++index;
        if (index < line.size() && (line[index] == '+' || line[index] == '-')) {
            ++index;
        }
        index = ScanDecimalDigits(line, index);
    }
    return ScanNumericSuffix(line, index);
}

bool IsMacroCall(std::string_view line, size_t identifier_end) {
    const size_t next = SkipWhitespace(line, identifier_end);
    return next < line.size() && line[next] == '!';
}

bool IsFunctionCall(std::string_view line, size_t identifier_end) {
    const size_t next = SkipWhitespace(line, identifier_end);
    return next < line.size() && line[next] == '(';
}

size_t SkipBackwardWhitespace(std::string_view line, size_t index) {
    while (index > 0 && std::isspace(static_cast<unsigned char>(line[index - 1])) != 0) {
        --index;
    }
    return index;
}

bool IsAttributeIdentifier(std::string_view line, size_t identifier_start) {
    size_t cursor = SkipBackwardWhitespace(line, identifier_start);
    if (cursor == 0 || line[cursor - 1] != '[') {
        return false;
    }

    cursor = SkipBackwardWhitespace(line, cursor - 1);
    if (cursor > 0 && line[cursor - 1] == '!') {
        cursor = SkipBackwardWhitespace(line, cursor - 1);
    }
    return cursor > 0 && line[cursor - 1] == '#';
}

std::optional<size_t> ScanLifetime(std::string_view line, size_t index) {
    if (index + 1 >= line.size() || line[index] != '\'') {
        return std::nullopt;
    }
    if (line[index + 1] == '_') {
        return index + 2;
    }
    if (!IsIdentifierStart(line[index + 1])) {
        return std::nullopt;
    }

    const size_t end = ScanIdentifierBody(line, index + 1);
    if (end < line.size() && line[end] == '\'') {
        return std::nullopt;
    }
    return end;
}

bool SpanEndAt(const std::vector<SyntaxSpan>& spans, size_t index, size_t* end) {
    if (end == nullptr) {
        return false;
    }
    for (const SyntaxSpan& span : spans) {
        if (index >= span.start && index < span.end) {
            *end = span.end;
            return true;
        }
    }
    return false;
}

size_t ScanNestedBlockComment(std::string_view line, size_t index, uint64_t depth, uint64_t* remaining_depth) {
    while (index < line.size()) {
        if (index + 1 < line.size() && line[index] == '/' && line[index + 1] == '*') {
            ++depth;
            index += 2;
            continue;
        }
        if (index + 1 < line.size() && line[index] == '*' && line[index + 1] == '/') {
            --depth;
            index += 2;
            if (depth == 0) {
                if (remaining_depth != nullptr) {
                    *remaining_depth = 0;
                }
                return index;
            }
            continue;
        }
        ++index;
    }

    if (remaining_depth != nullptr) {
        *remaining_depth = depth;
    }
    return line.size();
}

SyntaxLineState AppendProtectedSpans(std::string_view line,
                                     SyntaxLineState in_state,
                                     std::vector<SyntaxSpan>* spans) {
    size_t index = 0;

    if (DecodeMode(in_state) == RustScanMode::BlockComment) {
        uint64_t remaining_depth = DecodeData(in_state);
        index = ScanNestedBlockComment(line, 0, remaining_depth, &remaining_depth);
        AppendSpan(spans, 0, index, SyntaxTokenKind::Comment);
        if (remaining_depth != 0) {
            return EncodeState(RustScanMode::BlockComment, remaining_depth);
        }
    } else if (DecodeMode(in_state) == RustScanMode::QuotedString) {
        const std::optional<size_t> end = FindEscapedQuoteEnd(line, static_cast<size_t>(-1), '"');
        const size_t span_end = end.has_value() ? *end : line.size();
        AppendSpan(spans, 0, span_end, SyntaxTokenKind::String);
        if (!end.has_value()) {
            return EncodeState(RustScanMode::QuotedString);
        }
        index = span_end;
    } else if (DecodeMode(in_state) == RustScanMode::RawString) {
        const std::optional<size_t> end = FindRawStringEnd(line, 0, DecodeData(in_state));
        const size_t span_end = end.has_value() ? *end : line.size();
        AppendSpan(spans, 0, span_end, SyntaxTokenKind::String);
        if (!end.has_value()) {
            return EncodeState(RustScanMode::RawString, DecodeData(in_state));
        }
        index = span_end;
    }

    while (index < line.size()) {
        if (index + 1 < line.size() && line[index] == '/' && line[index + 1] == '/') {
            AppendSpan(spans, index, line.size(), SyntaxTokenKind::Comment);
            return EncodeState(RustScanMode::None);
        }

        if (index + 1 < line.size() && line[index] == '/' && line[index + 1] == '*') {
            uint64_t remaining_depth = 1;
            const size_t span_end = ScanNestedBlockComment(line, index + 2, remaining_depth, &remaining_depth);
            AppendSpan(spans, index, span_end, SyntaxTokenKind::Comment);
            if (remaining_depth != 0) {
                return EncodeState(RustScanMode::BlockComment, remaining_depth);
            }
            index = span_end;
            continue;
        }

        size_t raw_prefix_length = 0;
        size_t raw_hash_count = 0;
        if (MatchRawStringStart(line, index, &raw_prefix_length, &raw_hash_count)) {
            const std::optional<size_t> end = FindRawStringEnd(line, index + raw_prefix_length, raw_hash_count);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(RustScanMode::RawString, raw_hash_count);
            }
            index = span_end;
            continue;
        }

        if (line[index] == '"' ||
            (line[index] == 'b' && index + 1 < line.size() && line[index + 1] == '"')) {
            const size_t quote_index = line[index] == '"' ? index : index + 1;
            const std::optional<size_t> end = FindEscapedQuoteEnd(line, quote_index, '"');
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(RustScanMode::QuotedString);
            }
            index = span_end;
            continue;
        }

        if (line[index] == '\'' || (line[index] == 'b' && index + 1 < line.size() && line[index + 1] == '\'')) {
            const std::optional<size_t> end = ScanRustCharLiteral(line, index);
            if (end.has_value()) {
                AppendSpan(spans, index, *end, SyntaxTokenKind::String);
                index = *end;
                continue;
            }
        }

        ++index;
    }

    return EncodeState(RustScanMode::None);
}

void AppendLexicalSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    if (spans == nullptr) {
        return;
    }

    size_t index = 0;
    bool expect_type_name = false;
    bool expect_function_name = false;
    while (index < line.size()) {
        size_t protected_end = 0;
        if (SpanEndAt(*spans, index, &protected_end)) {
            index = protected_end;
            expect_type_name = false;
            expect_function_name = false;
            continue;
        }

        const std::optional<size_t> lifetime_end = ScanLifetime(line, index);
        if (lifetime_end.has_value()) {
            AppendSpan(spans, index, *lifetime_end, SyntaxTokenKind::Type);
            index = *lifetime_end;
            continue;
        }

        if (IsNumberStart(line, index)) {
            const size_t end = ScanNumberLiteral(line, index);
            AppendSpan(spans, index, end, SyntaxTokenKind::Number);
            index = end;
            continue;
        }

        const std::optional<RustIdentifier> identifier = ScanRustIdentifier(line, index);
        if (identifier.has_value()) {
            const std::string_view token = identifier->logical;
            if (IsAttributeIdentifier(line, identifier->start)) {
                AppendSpan(spans, identifier->start, identifier->end, SyntaxTokenKind::Macro);
                expect_function_name = false;
                expect_type_name = false;
            } else if (expect_function_name) {
                AppendSpan(spans, identifier->start, identifier->end, SyntaxTokenKind::Function);
                expect_function_name = false;
                expect_type_name = false;
            } else if (expect_type_name || ContainsWord(kTypeKeywords, token) || LooksLikeTypeName(token)) {
                AppendSpan(spans, identifier->start, identifier->end, SyntaxTokenKind::Type);
                expect_type_name = false;
            } else if (ContainsWord(kKeywords, token)) {
                AppendSpan(spans, identifier->start, identifier->end, SyntaxTokenKind::Keyword);
                expect_function_name = ContainsWord(kFunctionDeclarators, token);
                expect_type_name = ContainsWord(kTypeDeclarators, token);
            } else if (IsMacroCall(line, identifier->end)) {
                AppendSpan(spans, identifier->start, identifier->end, SyntaxTokenKind::Macro);
            } else if (IsFunctionCall(line, identifier->end)) {
                AppendSpan(spans, identifier->start, identifier->end, SyntaxTokenKind::Function);
            } else {
                expect_type_name = false;
            }

            index = identifier->end;
            continue;
        }

        ++index;
    }
}

}  // namespace

LanguageId RustHighlighter::language() const { return LanguageId::Rust; }

SyntaxLineState RustHighlighter::HighlightLine(std::string_view line,
                                               SyntaxLineState in_state,
                                               std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
    }

    const SyntaxLineState next_state = AppendProtectedSpans(line, in_state, spans);
    if (spans != nullptr) {
        AppendLexicalSpans(line, spans);
    }
    return next_state;
}

}  // namespace flowstate
