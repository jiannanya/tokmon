#include <white/window.hpp>

#include <tokmon/common/types.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <SDL3/SDL_system.h>
#endif

namespace white {
namespace {

float normalized_scale(float scale) {
  return std::isfinite(scale) && scale > 0 ? scale : 1.0F;
}

float window_display_scale(SDL_Window* window) {
  auto scale = SDL_GetWindowDisplayScale(window);
  if (!std::isfinite(scale) || scale <= 0) {
    const auto display = SDL_GetDisplayForWindow(window);
    if (display) scale = SDL_GetDisplayContentScale(display);
  }
  return normalized_scale(scale);
}

float window_pixel_density(SDL_Window* window) {
  return normalized_scale(SDL_GetWindowPixelDensity(window));
}

float window_units_per_logical_pixel(SDL_Window* window) {
  return normalized_scale(window_display_scale(window) /
                          window_pixel_density(window));
}

int scaled_dimension(int value, float scale) {
  return std::max(
      1, static_cast<int>(std::lround(static_cast<double>(value) * scale)));
}

SDL_HitTestResult SDLCALL borderless_hit_test(SDL_Window* window,
                                              const SDL_Point* point,
                                              void* userdata) {
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(window, &width, &height);
  const bool maximized =
      (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
  const auto* owner = static_cast<const Window*>(userdata);
  const auto content_scale =
      window_units_per_logical_pixel(window) *
      normalized_scale(owner ? owner->ui_scale() : 1.0F);
  const auto edge = scaled_dimension(6, content_scale);
  if (!maximized) {
    const bool left = point->x < edge;
    const bool right = point->x >= width - edge;
    const bool top = point->y < edge;
    const bool bottom = point->y >= height - edge;
    if (left && top) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (right && top) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (left && bottom) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (right && bottom) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
  }
  // The product supplies a precise header drag region in logical UI units;
  // convert it to window coordinates. Menus and window controls remain normal
  // hit targets.
  if (owner && owner->drag_region().has_value()) {
    const auto& region = *owner->drag_region();
    const auto left = region.x * content_scale;
    const auto top = region.y * content_scale;
    const auto right = (region.x + region.width) * content_scale;
    const auto bottom = (region.y + region.height) * content_scale;
    if (point->x >= left && point->x < right && point->y >= top &&
        point->y < bottom)
      return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
  }
  // Legacy fallback: the quiet center of a classic application bar.
  if (point->y < scaled_dimension(44, content_scale) &&
      point->x >= scaled_dimension(340, content_scale) &&
      point->x < width - scaled_dimension(320, content_scale))
    return SDL_HITTEST_DRAGGABLE;
  return SDL_HITTEST_NORMAL;
}

} // namespace

Window::Window(WindowOptions options) : options_(std::move(options)) {
  options_.ui_scale = std::clamp(options_.ui_scale, 0.75F, 2.0F);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    throw tokmon::Error("white.sdl.init", SDL_GetError());
  }
  auto flags = static_cast<SDL_WindowFlags>(0);
  flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
  if (options_.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (options_.borderless) flags |= SDL_WINDOW_BORDERLESS;
  const auto create_window = [&](SDL_WindowFlags create_flags) {
    return SDL_CreateWindow(options_.title.c_str(), options_.width,
                            options_.height, create_flags);
  };
  if (options_.renderer != RendererPreference::cpu) {
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                              SDL_GL_CONTEXT_PROFILE_CORE);
    (void)SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    (void)SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    window_ = create_window(flags | SDL_WINDOW_OPENGL);
    if (window_) {
      gl_context_ = SDL_GL_CreateContext(window_);
      if (!gl_context_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
      }
    }
    if (!gl_context_ && options_.renderer == RendererPreference::gpu)
      throw tokmon::Error("white.sdl.opengl", SDL_GetError());
  }
  if (!window_) window_ = create_window(flags);
  if (!window_) {
    throw tokmon::Error("white.sdl.window", SDL_GetError());
  }
#ifdef _WIN32
  native_window_handle_ = SDL_GetPointerProperty(
      SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
      nullptr);
#endif
  const auto initial_scale = window_units_per_logical_pixel(window_);
  if (std::abs(initial_scale - 1.0F) > 0.001F) {
    (void)SDL_SetWindowSize(window_,
                            scaled_dimension(options_.width, initial_scale),
                            scaled_dimension(options_.height, initial_scale));
    (void)SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED);
    (void)SDL_SyncWindow(window_);
  }
  if (options_.borderless)
    (void)SDL_SetWindowHitTest(window_, borderless_hit_test, this);
  (void)SDL_SetHint(SDL_HINT_MOUSE_DPI_SCALE_CURSORS, "1");
  default_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  pointer_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
  if (gl_context_) {
    (void)SDL_GL_MakeCurrent(window_, gl_context_);
    (void)SDL_GL_SetSwapInterval(1);
  } else {
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
      throw tokmon::Error("white.sdl.renderer", SDL_GetError());
    }
    // Let presentation pace high-DPI full-surface uploads to the display.
    (void)SDL_SetRenderVSync(renderer_, 1);
  }
  sync_drawable_size();
  (void)SDL_ShowWindow(window_);
  SDL_StartTextInput(window_);
}

Window::~Window() {
#ifdef _WIN32
  if (windows_message_hook_installed_)
    SDL_SetWindowsMessageHook(nullptr, nullptr);
#endif
  if (live_resize_watch_installed_)
    SDL_RemoveEventWatch(&Window::live_resize_watch, this);
  if (window_) SDL_StopTextInput(window_);
  if (pointer_cursor_) SDL_DestroyCursor(pointer_cursor_);
  if (default_cursor_) SDL_DestroyCursor(default_cursor_);
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
  surface_.reset();
  if (gl_context_) SDL_GL_DestroyContext(gl_context_);
  if (window_) SDL_DestroyWindow(window_);
  SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

void Window::set_draw_callback(DrawCallback callback) {
  std::lock_guard lock(mutex_);
  draw_ = std::move(callback);
  dirty_ = true;
}

void Window::set_submit_callback(SubmitCallback callback) {
  std::lock_guard lock(mutex_);
  submit_ = std::move(callback);
}

void Window::set_event_callback(EventCallback callback) {
  std::lock_guard lock(mutex_);
  events_ = std::move(callback);
}

bool Window::set_icon(const RasterSurface& icon) {
  auto* native_icon = SDL_CreateSurfaceFrom(
      icon.pixel_width(), icon.pixel_height(), SDL_PIXELFORMAT_BGRA32,
      const_cast<void*>(icon.pixels()), static_cast<int>(icon.row_bytes()));
  if (!native_icon) return false;
  const auto applied = SDL_SetWindowIcon(window_, native_icon);
  SDL_DestroySurface(native_icon);
  return applied;
}

void Window::set_builtin_chrome(bool enabled) {
  std::lock_guard lock(mutex_);
  builtin_chrome_ = enabled;
  dirty_ = true;
}

void Window::set_pointer_cursor(bool pointer) {
  auto* cursor = pointer ? pointer_cursor_ : default_cursor_;
  if (cursor) (void)SDL_SetCursor(cursor);
}

void Window::minimize() { (void)SDL_MinimizeWindow(window_); }

void Window::toggle_maximize() {
  if (maximized())
    (void)SDL_RestoreWindow(window_);
  else
    (void)SDL_MaximizeWindow(window_);
  dirty_ = true;
}

bool Window::maximized() const noexcept {
  return window_ &&
         (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
}

void Window::set_status(std::string status) {
  std::lock_guard lock(mutex_);
  status_ = std::move(status);
  dirty_ = true;
}

std::string Window::input_text() const {
  std::lock_guard lock(mutex_);
  return editor_.value() + editor_.composition();
}

Window::EditorSnapshot Window::editor_snapshot() const {
  std::lock_guard lock(mutex_);
  const auto [start, end] = editor_.selection();
  return {.value = editor_.value(),
          .composition = editor_.composition(),
          .cursor = editor_.cursor(),
          .selection_start = start,
          .selection_end = end,
          .focused = input_focused_,
          .caret_visible = caret_phase_};
}

void Window::set_input_text(std::string value) {
  std::lock_guard lock(mutex_);
  editor_.set_value(std::move(value));
  caret_phase_ = true;
  dirty_ = true;
}

void Window::set_input_cursor(std::size_t byte_offset, bool extend) {
  std::lock_guard lock(mutex_);
  editor_.set_cursor(byte_offset, extend);
  caret_phase_ = true;
  dirty_ = true;
}

void Window::set_input_focused(bool focused) {
  {
    std::lock_guard lock(mutex_);
    input_focused_ = focused;
    caret_phase_ = true;
    dirty_ = true;
  }
  if (focused)
    SDL_StartTextInput(window_);
  else
    SDL_StopTextInput(window_);
}

void Window::copy_to_clipboard(std::string_view value) {
  const auto copy = std::string(value);
  if (!SDL_SetClipboardText(copy.c_str()))
    throw tokmon::Error("white.sdl.clipboard", SDL_GetError());
}

void Window::submit_input() {
  SubmitCallback submit;
  std::string value;
  {
    std::lock_guard lock(mutex_);
    value = editor_.value();
    if (value.empty()) return;
    editor_.clear();
    submit = submit_;
    dirty_ = true;
  }
  if (submit) submit(std::move(value));
}

void Window::choose_files(FilesCallback callback, bool allow_many,
                          const std::filesystem::path& initial_location) {
  struct DialogRequest {
    FilesCallback callback;
  };
  auto* request = new DialogRequest{std::move(callback)};
  static constexpr SDL_DialogFileFilter filters[] = {
      {"Text and source files",
       "txt;md;json;c;cc;cpp;h;hpp;cmake;css;js;mjs;ts;py"},
      {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* files, int) {
        std::unique_ptr<DialogRequest> request(
            static_cast<DialogRequest*>(userdata));
        std::vector<std::filesystem::path> selected;
        if (files) {
          for (std::size_t index = 0; files[index]; ++index)
            selected.emplace_back(files[index]);
        }
        if (request->callback) request->callback(std::move(selected));
      },
      request, window_, filters, static_cast<int>(std::size(filters)),
      initial_location.empty() ? nullptr : initial_location.string().c_str(),
      allow_many);
}

void Window::invalidate() { dirty_.store(true, std::memory_order_release); }
void Window::close() { running_.store(false, std::memory_order_release); }
void Window::render_once() {
  dirty_.store(false, std::memory_order_release);
  render();
}

void Window::save_screenshot(const std::filesystem::path& path) {
  render_once();
  auto* screenshot = SDL_CreateSurfaceFrom(
      surface_->pixel_width(), surface_->pixel_height(),
      SDL_PIXELFORMAT_BGRA32,
      const_cast<void*>(surface_->pixels()),
      static_cast<int>(surface_->row_bytes()));
  if (!screenshot)
    throw tokmon::Error("white.sdl.screenshot", SDL_GetError());
  const auto saved = SDL_SaveBMP(screenshot, path.string().c_str());
  SDL_DestroySurface(screenshot);
  if (!saved)
    throw tokmon::Error("white.sdl.screenshot", SDL_GetError());
}

void Window::sync_drawable_size() {
  int window_width = 1;
  int window_height = 1;
  int pixel_width = 1;
  int pixel_height = 1;
  (void)SDL_GetWindowSize(window_, &window_width, &window_height);
  (void)SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
  window_width = std::max(1, window_width);
  window_height = std::max(1, window_height);
  pixel_width = std::max(1, pixel_width);
  pixel_height = std::max(1, pixel_height);

  display_scale_ = window_display_scale(window_);
  const auto render_scale = display_scale_ * options_.ui_scale;
  const int width = std::max(
      1, static_cast<int>(std::lround(static_cast<double>(pixel_width) /
                                     render_scale)));
  const int height = std::max(
      1, static_cast<int>(std::lround(static_cast<double>(pixel_height) /
                                     render_scale)));
  window_to_logical_x_ = static_cast<float>(pixel_width) /
                         static_cast<float>(window_width) / render_scale;
  window_to_logical_y_ = static_cast<float>(pixel_height) /
                         static_cast<float>(window_height) / render_scale;

  if (options_.resizable) {
    const auto minimum_width = scaled_dimension(
        720, 1.0F / normalized_scale(window_to_logical_x_));
    const auto minimum_height = scaled_dimension(
        480, 1.0F / normalized_scale(window_to_logical_y_));
    (void)SDL_SetWindowMinimumSize(window_, minimum_width, minimum_height);
  }

  const bool pixel_size_changed =
      !surface_ || surface_->pixel_width() != pixel_width ||
      surface_->pixel_height() != pixel_height;
  const bool logical_size_changed =
      !surface_ || surface_->width() != width || surface_->height() != height;
  if (!pixel_size_changed && !logical_size_changed) return;

  if (surface_)
    surface_->resize(width, height, pixel_width, pixel_height);
  else
    surface_ = std::make_unique<RasterSurface>(
        width, height, pixel_width, pixel_height,
        gl_context_ ? SurfaceBackend::gpu : SurfaceBackend::cpu);

  if (gl_context_) return;

  if (texture_ && !pixel_size_changed) return;
  resize_cpu_texture(pixel_width, pixel_height);
}

void Window::resize_cpu_texture(int pixel_width, int pixel_height) {
  auto* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA32,
                                    SDL_TEXTUREACCESS_STREAMING, pixel_width,
                                    pixel_height);
  if (!texture) {
    throw tokmon::Error("white.sdl.texture", SDL_GetError());
  }
  if (texture_) SDL_DestroyTexture(texture_);
  texture_ = texture;
}

bool Window::live_resize_watch(void* userdata, SDL_Event* event) {
  auto* owner = static_cast<Window*>(userdata);
  if (!owner || !event || event->type != SDL_EVENT_WINDOW_EXPOSED ||
      event->window.data1 != 1 ||
      std::this_thread::get_id() != owner->run_thread_ ||
      event->window.windowID != SDL_GetWindowID(owner->window_))
    return true;
#ifdef _WIN32
  // Actual size changes render immediately through the Win32 hook. The SDL
  // timer is retained as a cheap catch-up signal when frame pacing coalesces
  // the final size message; unchanged dimensions return without painting.
#endif
  // SDL guarantees live-resize expose events originate on the main OS thread
  // and explicitly permits drawing from an event watcher. Windows' modal
  // sizing loop otherwise prevents Window::run() from reaching SDL_PollEvent.
  try {
    owner->render_live_resize();
  } catch (...) {
    // Never allow a C callback to unwind through SDL. The queued resize event
    // will request a normal recovery frame when the modal loop yields.
    owner->dirty_.store(true, std::memory_order_release);
  }
  return true;
}

void Window::begin_live_resize() {
  if (live_resize_active_) return;
  live_resize_active_ = true;
  live_preview_width_ = surface_ ? surface_->pixel_width() : 0;
  live_preview_height_ = surface_ ? surface_->pixel_height() : 0;
  last_live_resize_ns_ = 0;
  if (gl_context_)
    (void)SDL_GL_SetSwapInterval(0);
  else
    (void)SDL_SetRenderVSync(renderer_, 0);
  if (surface_) surface_->prepare_preview();
}

void Window::end_live_resize() {
  if (!live_resize_active_) return;
  if (gl_context_)
    (void)SDL_GL_SetSwapInterval(1);
  else
    (void)SDL_SetRenderVSync(renderer_, 1);
  live_resize_active_ = false;
  live_preview_width_ = 0;
  live_preview_height_ = 0;
  last_live_resize_ns_ = 0;
  dirty_.store(true, std::memory_order_release);
}

void Window::commit_live_resize() {
  end_live_resize();
  // WM_EXITSIZEMOVE is the first point at which the final framebuffer size is
  // stable. Resize the backing surface and submit the exact high-DPI frame
  // here; merely marking the window dirty can otherwise leave SDL's normal
  // event pump asleep until the next pointer event.
  sync_drawable_size();
  dirty_.store(false, std::memory_order_release);
  render();
}

#ifdef _WIN32
bool Window::windows_message_hook(void* userdata, MSG* message) {
  auto* owner = static_cast<Window*>(userdata);
  if (!owner || !message || message->hwnd != owner->native_window_handle_ ||
      std::this_thread::get_id() != owner->run_thread_)
    return true;

  const auto resize_hit = [](WPARAM hit) {
    return hit == HTLEFT || hit == HTRIGHT || hit == HTTOP ||
           hit == HTBOTTOM || hit == HTTOPLEFT || hit == HTTOPRIGHT ||
           hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT;
  };

  switch (message->message) {
  case WM_NCLBUTTONDOWN:
    owner->native_resize_gesture_ = resize_hit(message->wParam);
    if (owner->native_resize_gesture_) {
      owner->begin_live_resize();

      // SDL works around Windows' roughly 500 ms non-client drag startup wait
      // for HTCAPTION, but not for custom resize edges. Queueing a zero-delta
      // move wakes DefWindowProc's sizing loop before the physical pointer has
      // crossed the system drag threshold, so the first pixel is observable.
      POINT cursor{};
      if (GetCursorPos(&cursor) && ScreenToClient(message->hwnd, &cursor)) {
        (void)PostMessageW(message->hwnd, WM_MOUSEMOVE, 0,
                           MAKELPARAM(cursor.x, cursor.y));
      }
    }
    break;
  case WM_WINDOWPOSCHANGED:
    if (owner->native_resize_gesture_) {
      try {
        owner->render_live_resize();
      } catch (...) {
        owner->dirty_.store(true, std::memory_order_release);
      }
    }
    break;
  case WM_EXITSIZEMOVE:
  case WM_CAPTURECHANGED:
    if (owner->native_resize_gesture_) {
      owner->native_resize_gesture_ = false;
      try {
        owner->commit_live_resize();
      } catch (...) {
        owner->end_live_resize();
        owner->dirty_.store(true, std::memory_order_release);
      }
    }
    break;
  default:
    break;
  }
  return true;
}
#endif

void Window::render_live_resize() {
  if (!running_.load(std::memory_order_acquire)) return;
  if (live_resize_rendering_.exchange(true, std::memory_order_acq_rel)) return;
  struct RenderGuard {
    std::atomic_bool& active;
    ~RenderGuard() { active.store(false, std::memory_order_release); }
  } guard{live_resize_rendering_};

  begin_live_resize();

  int pixel_width = 1;
  int pixel_height = 1;
  (void)SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
  pixel_width = std::max(1, pixel_width);
  pixel_height = std::max(1, pixel_height);
  if (live_preview_width_ == pixel_width &&
      live_preview_height_ == pixel_height)
    return;

  // Keep native messages responsive even on high-refresh mice. The SDL live
  // resize timer calls this method again, so the newest coalesced size is
  // always painted even if the pointer stops immediately after a skipped hit.
  const auto now = SDL_GetTicksNS();
  constexpr std::uint64_t minimum_interval_ns = 8'000'000;
  if (last_live_resize_ns_ &&
      now - last_live_resize_ns_ < minimum_interval_ns)
    return;

  display_scale_ = window_display_scale(window_);
  const auto render_scale = display_scale_ * options_.ui_scale;
  const int logical_width = std::max(
      1, static_cast<int>(std::lround(static_cast<double>(pixel_width) /
                                     render_scale)));
  const int logical_height = std::max(
      1, static_cast<int>(std::lround(static_cast<double>(pixel_height) /
                                     render_scale)));

  // Treat the physical surface as reusable capacity. The active pixel viewport
  // always matches the native framebuffer exactly, so live text and hairlines
  // are never enlarged from a lower-resolution intermediate image. Capacity
  // grows with headroom only when the user drags beyond the previous maximum.
  if (pixel_width > surface_->pixel_width() ||
      pixel_height > surface_->pixel_height()) {
    const auto grow = [](int capacity, int required) {
      const auto headroom = std::max(256, capacity / 4);
      return std::max(required, capacity + headroom);
    };
    const int capacity_width =
        pixel_width > surface_->pixel_width()
            ? grow(surface_->pixel_width(), pixel_width)
            : surface_->pixel_width();
    const int capacity_height =
        pixel_height > surface_->pixel_height()
            ? grow(surface_->pixel_height(), pixel_height)
            : surface_->pixel_height();
    surface_->resize(logical_width, logical_height, capacity_width,
                     capacity_height);
    if (!gl_context_)
      resize_cpu_texture(capacity_width, capacity_height);
  }
  surface_->reconfigure(logical_width, logical_height, pixel_width,
                        pixel_height);

  // Draw against every coalesced logical size into a pixel-exact viewport, so
  // fonts, icons, borders and radii retain both their layout proportions and
  // native framebuffer sharpness.
  // Clear the frame request before drawing so a concurrent invalidation stays
  // pending, while this completed resize frame does not trigger an immediate
  // non-preview presentation of the larger backing capacity.
  dirty_.store(false, std::memory_order_release);
  render(true, pixel_width, pixel_height);
  live_preview_width_ = pixel_width;
  live_preview_height_ = pixel_height;
  last_live_resize_ns_ = SDL_GetTicksNS();
}

int Window::run() {
  run_thread_ = std::this_thread::get_id();
  running_ = true;
#ifdef _WIN32
  if (native_window_handle_ && !windows_message_hook_installed_) {
    SDL_SetWindowsMessageHook(&Window::windows_message_hook, this);
    windows_message_hook_installed_ = true;
  }
#endif
  if (!live_resize_watch_installed_)
    live_resize_watch_installed_ =
        SDL_AddEventWatch(&Window::live_resize_watch, this);
  auto next_caret = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(500);
  while (running_.load(std::memory_order_acquire)) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_EVENT_QUIT:
        running_ = false;
        break;
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
        if (live_resize_active_) {
          end_live_resize();
        }
        sync_drawable_size();
        dirty_ = true;
        break;
      }
      case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
        EventCallback callback;
        {
          std::lock_guard lock(mutex_);
          callback = events_;
        }
        if (callback && callback(UiEvent{.type = "pointerleave"})) dirty_ = true;
        break;
      }
      case SDL_EVENT_MOUSE_MOTION:
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
      case SDL_EVENT_MOUSE_WHEEL: {
        EventCallback callback;
        {
          std::lock_guard lock(mutex_);
          callback = events_;
        }
        if (callback) {
          UiEvent ui;
          // SDL events use window coordinates. Convert them to the same
          // display-independent coordinates used by layout and hit testing.
          if (event.type == SDL_EVENT_MOUSE_MOTION) {
            ui.type = "pointermove";
            ui.x = event.motion.x * window_to_logical_x_;
            ui.y = event.motion.y * window_to_logical_y_;
          } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            ui.type = "wheel";
            float mouse_x = 0;
            float mouse_y = 0;
            SDL_GetMouseState(&mouse_x, &mouse_y);
            ui.x = mouse_x * window_to_logical_x_;
            ui.y = mouse_y * window_to_logical_y_;
            ui.delta_x = event.wheel.x;
            ui.delta_y = -event.wheel.y * 48.0F;
          } else {
            ui.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                          ? "pointerdown"
                          : "click";
            ui.x = event.button.x * window_to_logical_x_;
            ui.y = event.button.y * window_to_logical_y_;
          }
          if (callback(std::move(ui))) dirty_ = true;
        }
        break;
      }
      case SDL_EVENT_TEXT_INPUT: {
        std::lock_guard lock(mutex_);
        if (input_focused_) editor_.commit_composition(event.text.text);
        caret_phase_ = true;
        dirty_ = true;
        break;
      }
      case SDL_EVENT_TEXT_EDITING: {
        std::lock_guard lock(mutex_);
        if (input_focused_) editor_.set_composition(event.edit.text);
        caret_phase_ = true;
        dirty_ = true;
        break;
      }
      case SDL_EVENT_KEY_DOWN:
        handle_key(static_cast<std::uint32_t>(event.key.key),
                   static_cast<std::uint16_t>(event.key.mod));
        break;
      default:
        break;
      }
    }
    if (std::chrono::steady_clock::now() >= next_caret) {
      {
        std::lock_guard lock(mutex_);
        caret_phase_ = !caret_phase_;
      }
      dirty_ = true;
      next_caret = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(500);
    }
    const bool rendered = dirty_.exchange(false, std::memory_order_acq_rel);
    if (rendered) {
      render();
    }
    SDL_Delay(rendered ? 1 : 8);
  }
  if (live_resize_watch_installed_) {
    SDL_RemoveEventWatch(&Window::live_resize_watch, this);
    live_resize_watch_installed_ = false;
  }
  end_live_resize();
#ifdef _WIN32
  if (windows_message_hook_installed_) {
    SDL_SetWindowsMessageHook(nullptr, nullptr);
    windows_message_hook_installed_ = false;
  }
#endif
  return 0;
}

void Window::render(bool responsive_preview, int target_pixel_width,
                    int target_pixel_height) {
  DrawCallback draw;
  std::string input;
  std::string status;
  bool builtin_chrome = true;
  {
    std::lock_guard lock(mutex_);
    draw = draw_;
    input = editor_.value();
    if (!editor_.composition().empty()) input += editor_.composition();
    status = status_;
    builtin_chrome = builtin_chrome_;
  }
  surface_->begin_frame();
  if (!options_.opaque_draw) surface_->clear({247, 248, 250, 255});
  if (draw) draw(*surface_);

  if (builtin_chrome) {
    const auto height = static_cast<float>(surface_->height());
    surface_->fill_rect(
        {16, height - 64, static_cast<float>(surface_->width()) - 32, 46},
        {255, 255, 255, 255}, 8);
    surface_->stroke_rect(
        {16, height - 64, static_cast<float>(surface_->width()) - 32, 46},
        {205, 209, 218, 255}, 1, 8);
    surface_->text(input.empty() ? "Type a message and press Enter" : input,
                   30, height - 35, 16,
                   input.empty() ? Color{130, 135, 145, 255}
                                 : Color{25, 27, 32, 255});
    if (!status.empty()) {
      surface_->text(status, 20, 24, 13, {95, 100, 112, 255});
    }
  }

  if (gl_context_) {
    if (responsive_preview)
      surface_->present_preview(target_pixel_width, target_pixel_height);
    else
      surface_->flush();
    (void)SDL_GL_SwapWindow(window_);
    return;
  }
  const auto damage = surface_->frame_damage();
  const auto* pixels = static_cast<const std::byte*>(surface_->pixels());
  const auto pitch = static_cast<int>(surface_->row_bytes());
  if (responsive_preview || damage.empty() || damage.full()) {
    SDL_UpdateTexture(texture_, nullptr, pixels, pitch);
  } else {
    const auto scale_x = static_cast<float>(surface_->pixel_width()) /
                         static_cast<float>(surface_->width());
    const auto scale_y = static_cast<float>(surface_->pixel_height()) /
                         static_cast<float>(surface_->height());
    for (const auto& logical : damage.rects()) {
      const auto left = std::clamp(
          static_cast<int>(std::floor(logical.x * scale_x)), 0,
          surface_->pixel_width());
      const auto top = std::clamp(
          static_cast<int>(std::floor(logical.y * scale_y)), 0,
          surface_->pixel_height());
      const auto right = std::clamp(
          static_cast<int>(std::ceil((logical.x + logical.width) * scale_x)),
          left, surface_->pixel_width());
      const auto bottom = std::clamp(
          static_cast<int>(std::ceil((logical.y + logical.height) * scale_y)),
          top, surface_->pixel_height());
      if (right == left || bottom == top) continue;
      const SDL_Rect physical{left, top, right - left, bottom - top};
      const auto offset = static_cast<std::size_t>(top) *
                              surface_->row_bytes() +
                          static_cast<std::size_t>(left) *
                              sizeof(std::uint32_t);
      SDL_UpdateTexture(texture_, &physical, pixels + offset, pitch);
    }
  }
  SDL_SetRenderDrawColor(renderer_, 247, 248, 250, 255);
  SDL_RenderClear(renderer_);
  if (responsive_preview) {
    const SDL_FRect source{
        0, 0, static_cast<float>(surface_->viewport_pixel_width()),
        static_cast<float>(surface_->viewport_pixel_height())};
    SDL_RenderTexture(renderer_, texture_, &source, nullptr);
  } else {
    SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
  }
  SDL_RenderPresent(renderer_);
}

void Window::handle_key(std::uint32_t key, std::uint16_t modifiers) {
  SubmitCallback submit;
  std::string value;
  {
    std::lock_guard lock(mutex_);
    if (!input_focused_) return;
    const bool control = (modifiers & SDL_KMOD_CTRL) != 0;
    const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;
    if (control && key == SDLK_A) {
      editor_.select_all();
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (control && (key == SDLK_C || key == SDLK_X)) {
      const auto selected = editor_.selected_text();
      if (!selected.empty()) SDL_SetClipboardText(selected.c_str());
      if (key == SDLK_X) editor_.insert("");
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (control && key == SDLK_V) {
      if (char* clipboard = SDL_GetClipboardText()) {
        editor_.insert(clipboard);
        SDL_free(clipboard);
      }
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_BACKSPACE) {
      editor_.backspace();
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_DELETE) {
      editor_.erase_forward();
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_LEFT) {
      editor_.move_left(shift);
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_RIGHT) {
      editor_.move_right(shift);
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_UP) {
      editor_.move_up(shift);
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_DOWN) {
      editor_.move_down(shift);
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_HOME) {
      if (control)
        editor_.move_document_home(shift);
      else
        editor_.move_home(shift);
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key == SDLK_END) {
      if (control)
        editor_.move_document_end(shift);
      else
        editor_.move_end(shift);
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    if (key != SDLK_RETURN && key != SDLK_KP_ENTER) return;
    if (shift) {
      editor_.insert("\n");
      caret_phase_ = true;
      dirty_ = true;
      return;
    }
    value = editor_.value();
    editor_.clear();
    caret_phase_ = true;
    submit = submit_;
    dirty_ = true;
  }
  if (submit && !value.empty()) submit(std::move(value));
}

} // namespace white
