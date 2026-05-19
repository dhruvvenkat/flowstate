#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "syntax/language.h"

namespace flowstate {

constexpr size_t kIndentWidth = 4;

enum class BufferType {
    File,
    AiScratch,
    PatchPreview,
    BuildOutput,
};

enum class LineCommentToggleResult {
    Unchanged,
    Commented,
    Uncommented,
};

struct Cursor {
    size_t row = 0;
    size_t col = 0;
};

class Buffer {
  public:
    Buffer(BufferType type = BufferType::File, std::string name = "", bool read_only = false);

    BufferType type() const;
    bool readOnly() const;
    void setReadOnly(bool read_only);

    const std::string& name() const;
    void setName(std::string name);

    const std::optional<std::filesystem::path>& path() const;
    void setPath(const std::filesystem::path& path);

    bool dirty() const;
    void clearDirty();
    void setDirty(bool dirty);

    size_t lineCount() const;
    const std::string& line(size_t row) const;
    std::string currentLineText(size_t row) const;
    const std::vector<std::string>& lines() const;
    void setLines(std::vector<std::string> lines, bool mark_dirty);
    void setText(const std::string& text, bool mark_dirty);
    std::string text() const;

    void insertChar(const Cursor& position, char ch);
    void insertIndent(Cursor& cursor);
    std::vector<size_t> indentLines(size_t start_row, size_t end_row);
    std::vector<size_t> unindentLines(size_t start_row, size_t end_row);
    void insertText(Cursor& cursor, std::string_view text);
    void insertPairedChars(Cursor& cursor, char open, char close);
    void insertNewline(Cursor& cursor);
    void deleteCharBefore(Cursor& cursor);
    void deleteCharAt(Cursor& cursor);
    void deleteRange(Cursor& cursor, const Cursor& start, const Cursor& end);
    void replaceRange(Cursor& cursor, const Cursor& start, const Cursor& end, std::string_view text);
    LineCommentToggleResult toggleLineComments(size_t start_row, size_t end_row, std::string_view comment_prefix);

    bool save(std::string* error = nullptr);
    LanguageId languageId() const;
    std::string guessLanguage() const;

  private:
    void ensureNonEmpty();

    BufferType type_;
    std::string name_;
    std::optional<std::filesystem::path> path_;
    bool read_only_;
    bool dirty_ = false;
    std::vector<std::string> lines_;
};

Buffer LoadFileBuffer(const std::filesystem::path& path, std::string* error = nullptr);
std::vector<std::string> SplitLines(std::string_view text);

}  // namespace flowstate
