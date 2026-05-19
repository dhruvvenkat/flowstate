#include "buffer.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace flowstate {

namespace {

std::vector<std::string> EnsureNonEmpty(std::vector<std::string> lines) {
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

std::string ReadFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "Unable to open file: " + path.string();
        }
        return {};
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool IsUtf8Continuation(unsigned char ch) {
    return ch >= 0x80 && ch <= 0xbf;
}

bool IsHumanReadableUtf8(std::string_view text) {
    for (size_t index = 0; index < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch < 0x80) {
            if ((ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 0x7f) {
                return false;
            }
            ++index;
            continue;
        }

        if (ch >= 0xc2 && ch <= 0xdf) {
            if (index + 1 >= text.size() ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }

        if (ch == 0xe0) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            const unsigned char third = static_cast<unsigned char>(text[index + 2]);
            if (second < 0xa0 || second > 0xbf || !IsUtf8Continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }

        if ((ch >= 0xe1 && ch <= 0xec) || (ch >= 0xee && ch <= 0xef)) {
            if (index + 2 >= text.size() ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 1])) ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }

        if (ch == 0xed) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            const unsigned char third = static_cast<unsigned char>(text[index + 2]);
            if (second < 0x80 || second > 0x9f || !IsUtf8Continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }

        if (ch == 0xf0) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if (second < 0x90 || second > 0xbf ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 2])) ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }

        if (ch >= 0xf1 && ch <= 0xf3) {
            if (index + 3 >= text.size() ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 1])) ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 2])) ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }

        if (ch == 0xf4) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if (second < 0x80 || second > 0x8f ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 2])) ||
                !IsUtf8Continuation(static_cast<unsigned char>(text[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }

    return true;
}

Cursor ClampCursorToLines(const std::vector<std::string>& lines, Cursor cursor) {
    if (lines.empty()) {
        return {};
    }

    cursor.row = std::min(cursor.row, lines.size() - 1);
    cursor.col = std::min(cursor.col, lines[cursor.row].size());
    return cursor;
}

bool ComesBefore(const Cursor& left, const Cursor& right) {
    if (left.row != right.row) {
        return left.row < right.row;
    }
    return left.col <= right.col;
}

size_t LeadingWhitespaceLength(const std::string& line) {
    size_t length = 0;
    while (length < line.size() && (line[length] == ' ' || line[length] == '\t')) {
        ++length;
    }
    return length;
}

bool StartsWithAt(std::string_view text, size_t index, std::string_view prefix) {
    return index <= text.size() && prefix.size() <= text.size() - index &&
           text.compare(index, prefix.size(), prefix) == 0;
}

size_t PreviousIndentStop(size_t col) {
    if (col == 0) {
        return 0;
    }
    return ((col - 1) / kIndentWidth) * kIndentWidth;
}

size_t SpacesToNextIndentStop(size_t col) {
    const size_t remainder = col % kIndentWidth;
    return remainder == 0 ? kIndentWidth : kIndentWidth - remainder;
}

}  // namespace

Buffer::Buffer(BufferType type, std::string name, bool read_only)
    : type_(type), name_(std::move(name)), read_only_(read_only), lines_(1, "") {}

BufferType Buffer::type() const { return type_; }

bool Buffer::readOnly() const { return read_only_; }

void Buffer::setReadOnly(bool read_only) { read_only_ = read_only; }

const std::string& Buffer::name() const { return name_; }

void Buffer::setName(std::string name) { name_ = std::move(name); }

const std::optional<std::filesystem::path>& Buffer::path() const { return path_; }

void Buffer::setPath(const std::filesystem::path& path) {
    path_ = path;
    if (name_.empty()) {
        name_ = path.filename().string();
    }
}

bool Buffer::dirty() const { return dirty_; }

void Buffer::clearDirty() { dirty_ = false; }

void Buffer::setDirty(bool dirty) { dirty_ = dirty; }

size_t Buffer::lineCount() const { return lines_.size(); }

const std::string& Buffer::line(size_t row) const {
    static const std::string kEmpty;
    if (row >= lines_.size()) {
        return kEmpty;
    }
    return lines_[row];
}

std::string Buffer::currentLineText(size_t row) const { return line(row); }

const std::vector<std::string>& Buffer::lines() const { return lines_; }

void Buffer::setLines(std::vector<std::string> lines, bool mark_dirty) {
    lines_ = EnsureNonEmpty(std::move(lines));
    if (mark_dirty && !read_only_) {
        dirty_ = true;
    }
}

void Buffer::setText(const std::string& text, bool mark_dirty) {
    setLines(SplitLines(text), mark_dirty);
}

std::string Buffer::text() const {
    std::ostringstream output;
    for (size_t index = 0; index < lines_.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines_[index];
    }
    return output.str();
}

void Buffer::insertChar(const Cursor& position, char ch) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();

    const size_t row = std::min(position.row, lines_.size() - 1);
    const size_t col = std::min(position.col, lines_[row].size());
    lines_[row].insert(lines_[row].begin() + static_cast<std::ptrdiff_t>(col), ch);
    dirty_ = true;
}

void Buffer::insertIndent(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();

    cursor = ClampCursorToLines(lines_, cursor);
    insertText(cursor, std::string(SpacesToNextIndentStop(cursor.col), ' '));
}

std::vector<size_t> Buffer::indentLines(size_t start_row, size_t end_row) {
    if (read_only_) {
        return {};
    }
    ensureNonEmpty();

    start_row = std::min(start_row, lines_.size() - 1);
    end_row = std::min(end_row, lines_.size() - 1);
    if (end_row < start_row) {
        std::swap(start_row, end_row);
    }

    const std::string indentation(kIndentWidth, ' ');
    std::vector<size_t> inserted(end_row - start_row + 1, kIndentWidth);
    for (size_t row = start_row; row <= end_row; ++row) {
        lines_[row].insert(0, indentation);
    }
    dirty_ = true;
    return inserted;
}

std::vector<size_t> Buffer::unindentLines(size_t start_row, size_t end_row) {
    if (read_only_) {
        return {};
    }
    ensureNonEmpty();

    start_row = std::min(start_row, lines_.size() - 1);
    end_row = std::min(end_row, lines_.size() - 1);
    if (end_row < start_row) {
        std::swap(start_row, end_row);
    }

    std::vector<size_t> removed(end_row - start_row + 1, 0);
    bool changed = false;
    for (size_t row = start_row; row <= end_row; ++row) {
        std::string& line = lines_[row];
        size_t erase_length = 0;
        if (!line.empty() && line.front() == '\t') {
            erase_length = 1;
        } else {
            while (erase_length < kIndentWidth && erase_length < line.size() && line[erase_length] == ' ') {
                ++erase_length;
            }
        }
        if (erase_length == 0) {
            continue;
        }
        line.erase(0, erase_length);
        removed[row - start_row] = erase_length;
        changed = true;
    }
    if (changed) {
        dirty_ = true;
    }
    return removed;
}

void Buffer::insertText(Cursor& cursor, std::string_view text) {
    if (read_only_ || text.empty()) {
        return;
    }
    ensureNonEmpty();

    cursor = ClampCursorToLines(lines_, cursor);
    const size_t row = cursor.row;
    const size_t col = cursor.col;
    const std::string prefix = lines_[row].substr(0, col);
    const std::string suffix = lines_[row].substr(col);

    std::vector<std::string> inserted = SplitLines(text);
    if (inserted.size() == 1) {
        lines_[row] = prefix + inserted.front() + suffix;
        cursor.col = col + inserted.front().size();
        dirty_ = true;
        return;
    }

    lines_[row] = prefix + inserted.front();
    const size_t inserted_count = inserted.size();
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row + 1), inserted.begin() + 1, inserted.end());
    lines_[row + inserted_count - 1] += suffix;
    cursor.row = row + inserted_count - 1;
    cursor.col = inserted.back().size();
    dirty_ = true;
}

void Buffer::insertPairedChars(Cursor& cursor, char open, char close) {
    if (read_only_) {
        return;
    }

    std::string pair;
    pair.push_back(open);
    pair.push_back(close);
    insertText(cursor, pair);
    if (cursor.col > 0) {
        --cursor.col;
    }
}

void Buffer::insertNewline(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();

    const size_t row = std::min(cursor.row, lines_.size() - 1);
    const size_t col = std::min(cursor.col, lines_[row].size());
    const size_t indentation_length = LeadingWhitespaceLength(lines_[row]);
    const std::string indentation = lines_[row].substr(0, indentation_length);
    std::string remainder = lines_[row].substr(col);
    lines_[row].erase(col);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row + 1), indentation + remainder);
    cursor.row = row + 1;
    cursor.col = indentation.size();
    dirty_ = true;
}

void Buffer::deleteCharBefore(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();
    if (cursor.row >= lines_.size()) {
        cursor.row = lines_.size() - 1;
        cursor.col = lines_[cursor.row].size();
        return;
    }

    if (cursor.col > 0) {
        const size_t indentation_length = LeadingWhitespaceLength(lines_[cursor.row]);
        if (cursor.col <= indentation_length) {
            const size_t target_col = PreviousIndentStop(cursor.col);
            lines_[cursor.row].erase(target_col, cursor.col - target_col);
            cursor.col = target_col;
            dirty_ = true;
            return;
        }

        lines_[cursor.row].erase(cursor.col - 1, 1);
        --cursor.col;
        dirty_ = true;
        return;
    }

    if (cursor.row == 0) {
        return;
    }

    const size_t previous_row = cursor.row - 1;
    const size_t previous_size = lines_[previous_row].size();
    lines_[previous_row] += lines_[cursor.row];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor.row));
    cursor.row = previous_row;
    cursor.col = previous_size;
    ensureNonEmpty();
    dirty_ = true;
}

void Buffer::deleteCharAt(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();
    if (cursor.row >= lines_.size()) {
        return;
    }

    if (cursor.col < lines_[cursor.row].size()) {
        lines_[cursor.row].erase(cursor.col, 1);
        dirty_ = true;
        return;
    }

    if (cursor.row + 1 >= lines_.size()) {
        return;
    }

    lines_[cursor.row] += lines_[cursor.row + 1];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor.row + 1));
    ensureNonEmpty();
    dirty_ = true;
}

void Buffer::deleteRange(Cursor& cursor, const Cursor& start, const Cursor& end) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();

    Cursor range_start = ClampCursorToLines(lines_, start);
    Cursor range_end = ClampCursorToLines(lines_, end);
    if (!ComesBefore(range_start, range_end)) {
        std::swap(range_start, range_end);
    }

    cursor = range_start;
    if (range_start.row == range_end.row && range_start.col == range_end.col) {
        return;
    }

    if (range_start.row == range_end.row) {
        lines_[range_start.row].erase(range_start.col, range_end.col - range_start.col);
        dirty_ = true;
        return;
    }

    const std::string suffix = lines_[range_end.row].substr(range_end.col);
    lines_[range_start.row].erase(range_start.col);
    lines_[range_start.row] += suffix;
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(range_start.row + 1),
                 lines_.begin() + static_cast<std::ptrdiff_t>(range_end.row + 1));
    ensureNonEmpty();
    cursor = ClampCursorToLines(lines_, range_start);
    dirty_ = true;
}

void Buffer::replaceRange(Cursor& cursor, const Cursor& start, const Cursor& end, std::string_view text) {
    deleteRange(cursor, start, end);
    insertText(cursor, text);
}

LineCommentToggleResult Buffer::toggleLineComments(size_t start_row,
                                                   size_t end_row,
                                                   std::string_view comment_prefix) {
    if (read_only_ || comment_prefix.empty()) {
        return LineCommentToggleResult::Unchanged;
    }
    ensureNonEmpty();

    start_row = std::min(start_row, lines_.size() - 1);
    end_row = std::min(end_row, lines_.size() - 1);
    if (end_row < start_row) {
        std::swap(start_row, end_row);
    }

    bool has_non_blank_line = false;
    bool all_non_blank_lines_commented = true;
    for (size_t row = start_row; row <= end_row; ++row) {
        const std::string& line = lines_[row];
        const size_t indentation = LeadingWhitespaceLength(line);
        if (indentation == line.size()) {
            continue;
        }

        has_non_blank_line = true;
        if (!StartsWithAt(line, indentation, comment_prefix)) {
            all_non_blank_lines_commented = false;
            break;
        }
    }

    if (has_non_blank_line && all_non_blank_lines_commented) {
        for (size_t row = start_row; row <= end_row; ++row) {
            std::string& line = lines_[row];
            const size_t indentation = LeadingWhitespaceLength(line);
            if (indentation == line.size() || !StartsWithAt(line, indentation, comment_prefix)) {
                continue;
            }

            size_t erase_length = comment_prefix.size();
            if (indentation + erase_length < line.size() && line[indentation + erase_length] == ' ') {
                ++erase_length;
            }
            line.erase(indentation, erase_length);
        }
        dirty_ = true;
        return LineCommentToggleResult::Uncommented;
    }

    const std::string marker = std::string(comment_prefix) + " ";
    for (size_t row = start_row; row <= end_row; ++row) {
        std::string& line = lines_[row];
        const size_t indentation = has_non_blank_line ? LeadingWhitespaceLength(line) : 0;
        if (has_non_blank_line && indentation == line.size()) {
            continue;
        }
        line.insert(indentation, marker);
    }
    dirty_ = true;
    return LineCommentToggleResult::Commented;
}

bool Buffer::save(std::string* error) {
    if (read_only_) {
        if (error != nullptr) {
            *error = "Buffer is read-only.";
        }
        return false;
    }
    if (!path_.has_value()) {
        if (error != nullptr) {
            *error = "No file path set.";
        }
        return false;
    }

    std::ofstream output(*path_);
    if (!output) {
        if (error != nullptr) {
            *error = "Unable to write file: " + path_->string();
        }
        return false;
    }

    output << text();
    output.flush();
    dirty_ = false;
    return output.good();
}

LanguageId Buffer::languageId() const { return DetectLanguageId(path_); }

std::string Buffer::guessLanguage() const {
    return std::string(LanguageDisplayName(languageId()));
}

void Buffer::ensureNonEmpty() {
    if (lines_.empty()) {
        lines_.emplace_back();
    }
}

Buffer LoadFileBuffer(const std::filesystem::path& path, std::string* error) {
    Buffer buffer(BufferType::File, path.filename().string(), false);
    buffer.setPath(path);

    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        if (error != nullptr) {
            *error = "Unable to inspect file: " + path.string();
        }
        return buffer;
    }
    if (!exists) {
        buffer.setLines({""}, false);
        buffer.clearDirty();
        return buffer;
    }
    if (!std::filesystem::is_regular_file(path, filesystem_error) || filesystem_error) {
        if (error != nullptr) {
            *error = "Cannot open non-file path: " + path.string();
        }
        return buffer;
    }

    std::string read_error;
    const std::string contents = ReadFile(path, &read_error);
    if (!read_error.empty()) {
        if (error != nullptr) {
            *error = read_error;
        }
        return buffer;
    }
    if (!IsHumanReadableUtf8(contents)) {
        if (error != nullptr) {
            *error = "Cannot open non-text or encoded file: " + path.string();
        }
        return buffer;
    }

    buffer.setText(contents, false);
    buffer.clearDirty();
    return buffer;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;

    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch != '\r') {
            current.push_back(ch);
        }
    }

    lines.push_back(current);
    return EnsureNonEmpty(std::move(lines));
}

}  // namespace flowstate
