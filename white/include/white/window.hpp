#pragma once

#include <white/renderer.hpp>
#include <white/text_editor.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;
struct SDL_GLContextState;

namespace white {

enum class RendererPreference { automatic, gpu, cpu };

struct WindowOptions {
  std::string title{"White"};
  int width{1280};
  int height{800};
  bool resizable{true};
  bool borderless{false};
  // Product zoom applied on top of the operating system's display scale.
  float ui_scale{1.0F};
  // The draw callback covers every pixel, so the window can skip pre-clearing.
  bool opaque_draw{false};
  // Automatic prefers Skia Ganesh/OpenGL and falls back to Skia raster plus
  // SDL's accelerated presenter (including headless/test environments).
  RendererPreference renderer{RendererPreference::automatic};
};

class Window final {
public:
  using DrawCallback = std::function<void(RasterSurface&)>;
  using SubmitCallback = std::function<void(std::string)>;
  // Return true only when the event changed visible state and needs a frame.
  using EventCallback = std::function<bool(UiEvent)>;
  using FilesCallback =
      std::function<void(std::vector<std::filesystem::path>)>;

  struct EditorSnapshot {
    std::string value;
    std::string composition;
    std::size_t cursor{0};
    std::size_t selection_start{0};
    std::size_t selection_end{0};
    bool focused{true};
    bool caret_visible{true};
  };

  explicit Window(WindowOptions options = {});
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  void set_draw_callback(DrawCallback callback);
  void set_submit_callback(SubmitCallback callback);
  void set_event_callback(EventCallback callback);
  [[nodiscard]] bool set_icon(const RasterSurface& icon);
  void set_builtin_chrome(bool enabled);
  void set_pointer_cursor(bool pointer);
  void minimize();
  void toggle_maximize();
  [[nodiscard]] bool maximized() const noexcept;
  void set_status(std::string status);
  [[nodiscard]] std::string input_text() const;
  [[nodiscard]] EditorSnapshot editor_snapshot() const;
  void set_input_text(std::string value);
  void set_input_cursor(std::size_t byte_offset, bool extend = false);
  void set_input_focused(bool focused);
  void copy_to_clipboard(std::string_view value);
  void submit_input();
  void choose_files(FilesCallback callback, bool allow_many = true,
                    const std::filesystem::path& initial_location = {});
  [[nodiscard]] float display_scale() const noexcept { return display_scale_; }
  [[nodiscard]] float ui_scale() const noexcept { return options_.ui_scale; }
  [[nodiscard]] SurfaceBackend surface_backend() const noexcept {
    return gl_context_ ? SurfaceBackend::gpu : SurfaceBackend::cpu;
  }
  void invalidate();
  void close();
  void render_once();
  void save_screenshot(const std::filesystem::path& path);
  int run();

private:
  void sync_drawable_size();
  void render();
  void handle_key(std::uint32_t key, std::uint16_t modifiers);

  WindowOptions options_;
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
  SDL_Texture* texture_{nullptr};
  SDL_Cursor* default_cursor_{nullptr};
  SDL_Cursor* pointer_cursor_{nullptr};
  SDL_GLContextState* gl_context_{nullptr};
  std::unique_ptr<RasterSurface> surface_;
  mutable std::mutex mutex_;
  DrawCallback draw_;
  SubmitCallback submit_;
  EventCallback events_;
  TextEditor editor_;
  std::string status_;
  bool builtin_chrome_{true};
  bool input_focused_{true};
  bool caret_phase_{true};
  std::atomic_bool running_{false};
  std::atomic_bool dirty_{true};
  float display_scale_{1};
  float window_to_logical_x_{1};
  float window_to_logical_y_{1};
};

} // namespace white
