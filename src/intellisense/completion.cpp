#include "intellisense/completion.h"

#include <algorithm>
#include <cctype>

namespace flowstate {

namespace {

bool IsIdentifierCharacter(char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    return std::isalnum(value) != 0 || ch == '_';
}

bool IsValidRange(const Buffer& buffer, const Cursor& start, const Cursor& end) {
    if (start.row >= buffer.lineCount() || end.row >= buffer.lineCount()) {
        return false;
    }
    if (start.row > end.row || (start.row == end.row && start.col > end.col)) {
        return false;
    }
    return start.col <= buffer.line(start.row).size() && end.col <= buffer.line(end.row).size();
}

std::string InsertTextForItem(const CompletionItem& item) {
    if (item.text_edit.has_value() && !item.text_edit->new_text.empty()) {
        return item.text_edit->new_text;
    }
    if (!item.insert_text.empty()) {
        return item.insert_text;
    }
    return item.label;
}

}  // namespace

bool IsCppCompletionLanguage(LanguageId language_id) {
    return language_id == LanguageId::C || language_id == LanguageId::CHeader || language_id == LanguageId::Cpp;
}

bool IsCompletionLanguage(LanguageId language_id) {
    return IsCppCompletionLanguage(language_id) || language_id == LanguageId::Python ||
           language_id == LanguageId::JavaScript || language_id == LanguageId::TypeScript;
}

Cursor CompletionPrefixStart(const Buffer& buffer, Cursor cursor) {
    if (buffer.lineCount() == 0) {
        return {};
    }
    cursor.row = std::min(cursor.row, buffer.lineCount() - 1);
    cursor.col = std::min(cursor.col, buffer.line(cursor.row).size());

    const std::string& line = buffer.line(cursor.row);
    size_t start_col = cursor.col;
    while (start_col > 0 && IsIdentifierCharacter(line[start_col - 1])) {
        --start_col;
    }
    return Cursor{cursor.row, start_col};
}

bool IsCompletionAutoTrigger(const Buffer& buffer, Cursor cursor) {
    const LanguageId language_id = buffer.languageId();
    if (!IsCompletionLanguage(language_id) || cursor.row >= buffer.lineCount()) {
        return false;
    }

    const std::string& line = buffer.line(cursor.row);
    if (cursor.col == 0 || cursor.col > line.size()) {
        return false;
    }

    const char previous = line[cursor.col - 1];
    if (IsIdentifierCharacter(previous)) {
        return true;
    }
    if (previous == '.') {
        return true;
    }
    if (!IsCppCompletionLanguage(language_id)) {
        return false;
    }
    if (previous == '>' && cursor.col >= 2 && line[cursor.col - 2] == '-') {
        return true;
    }
    return previous == ':' && cursor.col >= 2 && line[cursor.col - 2] == ':';
}

bool ApplyCompletionItem(Buffer& buffer,
                         Cursor& cursor,
                         const CompletionItem& item,
                         Cursor fallback_start,
                         Cursor fallback_end) {
    if (buffer.readOnly()) {
        return false;
    }

    const std::string replacement = InsertTextForItem(item);
    if (replacement.empty()) {
        return false;
    }

    Cursor start = fallback_start;
    Cursor end = fallback_end;
    if (item.text_edit.has_value() && IsValidRange(buffer, item.text_edit->start, item.text_edit->end)) {
        start = item.text_edit->start;
        end = item.text_edit->end;
    }
    if (!IsValidRange(buffer, start, end)) {
        return false;
    }

    buffer.replaceRange(cursor, start, end, replacement);
    return true;
}

}  // namespace flowstate
