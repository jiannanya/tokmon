#pragma once

#include <white/renderer.hpp>
#include <white/text_editor.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace white {

struct WindowOptions {
  std::string title{"White"};
  int width{1280};
  int height{800};
  bool resizable{true};
};

class Window final {
public:
  using DrawCallback = std::function<void(RasterSurface&)>;
  using SubmitCallback = std::function<void(std::string)>;
  using EventCallback = std::function<void(UiEvent)>;

  explicit Window(WindowOptions options = {});
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  void set_draw_callback(DrawCallback callback);
  void set_submit_callback(SubmitCallback callback);
  void set_event_callback(EventCallback callback);
  void set_status(std::string status);
  [[nodiscard]] std::string input_text() const;
  [[nodiscard]] float display_scale() const noexcept { return display_scale_; }
  void invalidate();
  void close();
  void render_once();
  int run();

private:
  void render();
  void handle_key(std::uint32_t key, std::uint16_t modifiers);

  WindowOptions options_;
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
  SDL_Texture* texture_{nullptr};
  std::unique_ptr<RasterSurface> surface_;
  mutable std::mutex mutex_;
  DrawCallback draw_;
  SubmitCallback submit_;
  EventCallback events_;
  TextEditor editor_;
  std::string status_;
  std::atomic_bool running_{false};
  std::atomic_bool dirty_{true};
  float display_scale_{1};
};

} // namespace white
