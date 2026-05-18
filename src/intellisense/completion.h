#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.h"

namespace flowstate {

struct CompletionTextEdit {
    Cursor start;
    Cursor end;
    std::string new_text;
};

struct CompletionItem {
    std::string label;
    std::string detail;
    std::string insert_text;
    std::string filter_text;
    std::string sort_text;
    std::optional<CompletionTextEdit> text_edit;
};

struct CompletionSession {
    bool active = false;
    bool waiting = false;
    int request_id = 0;
    Cursor replace_start;
    Cursor replace_end;
    std::vector<CompletionItem> items;
    size_t selected = 0;
    std::string message;
};

bool IsCppCompletionLanguage(LanguageId language_id);
bool IsCompletionLanguage(LanguageId language_id);
Cursor CompletionPrefixStart(const Buffer& buffer, Cursor cursor);
std::string CompletionPrefixText(const Buffer& buffer, Cursor start, Cursor end);
std::vector<CompletionItem> FilterAndRankCompletionItems(std::vector<CompletionItem> items,
                                                         std::string_view prefix);
bool IsCompletionAutoTrigger(const Buffer& buffer, Cursor cursor);
bool ApplyCompletionItem(Buffer& buffer,
                         Cursor& cursor,
                         const CompletionItem& item,
                         Cursor fallback_start,
                         Cursor fallback_end);

}  // namespace flowstate
