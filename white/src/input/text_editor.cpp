#include <white/text_editor.hpp>

#include <algorithm>

namespace white {

void TextEditor::set_value(std::string value) {
  value_ = std::move(value);
  cursor_ = value_.size();
  anchor_ = cursor_;
  composition_.clear();
}

std::pair<std::size_t, std::size_t> TextEditor::selection() const noexcept {
  return std::minmax(cursor_, anchor_);
}

std::string TextEditor::selected_text() const {
  const auto [start, end] = selection();
  return value_.substr(start, end - start);
}

std::size_t TextEditor::previous_boundary(std::string_view value,
                                          std::size_t position) noexcept {
  if (position == 0) return 0;
  --position;
  while (position > 0 &&
         (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
    --position;
  return position;
}

std::size_t TextEditor::next_boundary(std::string_view value,
                                      std::size_t position) noexcept {
  if (position >= value.size()) return value.size();
  ++position;
  while (position < value.size() &&
         (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
    ++position;
  return position;
}

void TextEditor::erase_selection() {
  const auto [start, end] = selection();
  if (start == end) return;
  value_.erase(start, end - start);
  cursor_ = anchor_ = start;
}

void TextEditor::insert(std::string_view utf8) {
  erase_selection();
  value_.insert(cursor_, utf8);
  cursor_ += utf8.size();
  anchor_ = cursor_;
  composition_.clear();
}

void TextEditor::backspace() {
  if (cursor_ != anchor_) {
    erase_selection();
    return;
  }
  const auto previous = previous_boundary(value_, cursor_);
  value_.erase(previous, cursor_ - previous);
  cursor_ = anchor_ = previous;
}

void TextEditor::erase_forward() {
  if (cursor_ != anchor_) {
    erase_selection();
    return;
  }
  const auto next = next_boundary(value_, cursor_);
  value_.erase(cursor_, next - cursor_);
}

void TextEditor::prepare_move(bool extend) {
  if (!extend) anchor_ = cursor_;
}

void TextEditor::move_left(bool extend) {
  if (!extend && cursor_ != anchor_) cursor_ = selection().first;
  else cursor_ = previous_boundary(value_, cursor_);
  prepare_move(extend);
}

void TextEditor::move_right(bool extend) {
  if (!extend && cursor_ != anchor_) cursor_ = selection().second;
  else cursor_ = next_boundary(value_, cursor_);
  prepare_move(extend);
}

std::size_t TextEditor::line_column(std::size_t position) const noexcept {
  const auto start = position == 0
                         ? std::string::npos
                         : value_.rfind('\n', position - 1);
  auto cursor = start == std::string::npos ? 0 : start + 1;
  std::size_t column = 0;
  while (cursor < position) {
    cursor = next_boundary(value_, cursor);
    ++column;
  }
  return column;
}

std::size_t TextEditor::position_at_column(
    std::size_t line_start, std::size_t line_end,
    std::size_t column) const noexcept {
  auto cursor = line_start;
  while (cursor < line_end && column-- > 0)
    cursor = next_boundary(value_, cursor);
  return cursor;
}

void TextEditor::move_up(bool extend) {
  const auto current_start_marker =
      cursor_ == 0 ? std::string::npos : value_.rfind('\n', cursor_ - 1);
  const auto current_start = current_start_marker == std::string::npos
                                 ? 0
                                 : current_start_marker + 1;
  if (current_start == 0) {
    cursor_ = 0;
  } else {
    const auto previous_end = current_start - 1;
    const auto previous_marker =
        value_.rfind('\n', previous_end == 0 ? 0 : previous_end - 1);
    const auto previous_start = previous_marker == std::string::npos
                                    ? 0
                                    : previous_marker + 1;
    cursor_ = position_at_column(previous_start, previous_end,
                                 line_column(cursor_));
  }
  prepare_move(extend);
}

void TextEditor::move_down(bool extend) {
  const auto current_end = value_.find('\n', cursor_);
  if (current_end == std::string::npos) {
    cursor_ = value_.size();
  } else {
    const auto next_start = current_end + 1;
    const auto next_marker = value_.find('\n', next_start);
    const auto next_end = next_marker == std::string::npos
                              ? value_.size()
                              : next_marker;
    cursor_ = position_at_column(next_start, next_end, line_column(cursor_));
  }
  prepare_move(extend);
}

void TextEditor::move_home(bool extend) {
  const auto marker =
      cursor_ == 0 ? std::string::npos : value_.rfind('\n', cursor_ - 1);
  cursor_ = marker == std::string::npos ? 0 : marker + 1;
  prepare_move(extend);
}

void TextEditor::move_end(bool extend) {
  const auto marker = value_.find('\n', cursor_);
  cursor_ = marker == std::string::npos ? value_.size() : marker;
  prepare_move(extend);
}

void TextEditor::move_document_home(bool extend) {
  cursor_ = 0;
  prepare_move(extend);
}

void TextEditor::move_document_end(bool extend) {
  cursor_ = value_.size();
  prepare_move(extend);
}

void TextEditor::set_cursor(std::size_t byte_offset, bool extend) {
  byte_offset = std::min(byte_offset, value_.size());
  while (byte_offset > 0 && byte_offset < value_.size() &&
         (static_cast<unsigned char>(value_[byte_offset]) & 0xc0U) == 0x80U)
    --byte_offset;
  cursor_ = byte_offset;
  prepare_move(extend);
}

void TextEditor::select_all() {
  anchor_ = 0;
  cursor_ = value_.size();
}

void TextEditor::clear() {
  value_.clear();
  cursor_ = anchor_ = 0;
  composition_.clear();
}

void TextEditor::set_composition(std::string utf8) {
  composition_ = std::move(utf8);
}

void TextEditor::commit_composition(std::string_view utf8) {
  insert(utf8);
  composition_.clear();
}

} // namespace white
