#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace white {

class TextEditor final {
public:
  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  void set_value(std::string value);
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
  [[nodiscard]] std::pair<std::size_t, std::size_t> selection() const noexcept;
  [[nodiscard]] std::string selected_text() const;
  [[nodiscard]] const std::string& composition() const noexcept {
    return composition_;
  }

  void insert(std::string_view utf8);
  void backspace();
  void erase_forward();
  void move_left(bool extend = false);
  void move_right(bool extend = false);
  void move_home(bool extend = false);
  void move_end(bool extend = false);
  void select_all();
  void clear();
  void set_composition(std::string utf8);
  void commit_composition(std::string_view utf8);
  void cancel_composition() noexcept { composition_.clear(); }

private:
  void erase_selection();
  void prepare_move(bool extend);
  [[nodiscard]] static std::size_t previous_boundary(
      std::string_view value, std::size_t position) noexcept;
  [[nodiscard]] static std::size_t next_boundary(
      std::string_view value, std::size_t position) noexcept;

  std::string value_;
  std::size_t cursor_{0};
  std::size_t anchor_{0};
  std::string composition_;
};

} // namespace white
