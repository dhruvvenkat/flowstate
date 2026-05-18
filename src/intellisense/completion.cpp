#include "intellisense/completion.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace flowstate {

namespace {

bool IsIdentifierCharacter(char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    return std::isalnum(value) != 0 || ch == '_';
}

std::string Lowercase(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string MatchTextForItem(const CompletionItem& item) {
    if (!item.filter_text.empty()) {
        return item.filter_text;
    }
    if (!item.insert_text.empty()) {
        return item.insert_text;
    }
    return item.label;
}

int CompletionMatchScore(const CompletionItem& item,
                         std::string_view prefix,
                         std::string_view lowered_prefix) {
    if (prefix.empty()) {
        return 0;
    }

    const std::string match_text = MatchTextForItem(item);
    const std::string lowered_match_text = Lowercase(match_text);
    const std::string lowered_label = Lowercase(item.label);
    const std::string lowered_insert_text = Lowercase(item.insert_text);

    if (match_text == prefix || item.label == prefix || item.insert_text == prefix) {
        return 0;
    }
    if (StartsWith(match_text, prefix) || StartsWith(item.label, prefix) ||
        (!item.insert_text.empty() && StartsWith(item.insert_text, prefix))) {
        return 1;
    }
    if (StartsWith(lowered_match_text, lowered_prefix) || StartsWith(lowered_label, lowered_prefix) ||
        (!lowered_insert_text.empty() && StartsWith(lowered_insert_text, lowered_prefix))) {
        return 2;
    }
    return std::numeric_limits<int>::max();
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
           language_id == LanguageId::JavaScript || language_id == LanguageId::TypeScript ||
           language_id == LanguageId::Go || language_id == LanguageId::Rust;
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

std::string CompletionPrefixText(const Buffer& buffer, Cursor start, Cursor end) {
    if (start.row != end.row || start.row >= buffer.lineCount()) {
        return {};
    }

    const std::string& line = buffer.line(start.row);
    start.col = std::min(start.col, line.size());
    end.col = std::min(end.col, line.size());
    if (start.col > end.col) {
        return {};
    }
    return line.substr(start.col, end.col - start.col);
}

std::vector<CompletionItem> FilterAndRankCompletionItems(std::vector<CompletionItem> items,
                                                         std::string_view prefix) {
    if (prefix.empty() || items.size() <= 1) {
        return items;
    }

    const std::string lowered_prefix = Lowercase(prefix);
    struct RankedItem {
        CompletionItem item;
        int score = 0;
        size_t original_index = 0;
    };

    std::vector<RankedItem> ranked;
    ranked.reserve(items.size());
    for (size_t index = 0; index < items.size(); ++index) {
        const int score = CompletionMatchScore(items[index], prefix, lowered_prefix);
        if (score == std::numeric_limits<int>::max()) {
            continue;
        }
        ranked.push_back({.item = std::move(items[index]), .score = score, .original_index = index});
    }

    if (ranked.empty()) {
        return items;
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedItem& left, const RankedItem& right) {
        if (left.score != right.score) {
            return left.score < right.score;
        }
        if (left.item.sort_text != right.item.sort_text) {
            if (left.item.sort_text.empty()) {
                return false;
            }
            if (right.item.sort_text.empty()) {
                return true;
            }
            return left.item.sort_text < right.item.sort_text;
        }
        return left.original_index < right.original_index;
    });

    std::vector<CompletionItem> filtered;
    filtered.reserve(ranked.size());
    for (RankedItem& item : ranked) {
        filtered.push_back(std::move(item.item));
    }
    return filtered;
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
    if (!IsCppCompletionLanguage(language_id) && language_id != LanguageId::Rust) {
        return false;
    }
    if (IsCppCompletionLanguage(language_id) && previous == '>' && cursor.col >= 2 && line[cursor.col - 2] == '-') {
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
