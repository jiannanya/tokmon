#include <white/window.hpp>

#include <tokmon/common/types.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <memory>
#include <thread>

namespace white {

Window::Window(WindowOptions options) : options_(std::move(options)) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    throw tokmon::Error("white.sdl.init", SDL_GetError());
  }
  const auto flags = options_.resizable
                         ? SDL_WINDOW_RESIZABLE
                         : static_cast<SDL_WindowFlags>(0);
  window_ = SDL_CreateWindow(options_.title.c_str(), options_.width,
                             options_.height, flags);
  if (!window_) {
    throw tokmon::Error("white.sdl.window", SDL_GetError());
  }
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) {
    throw tokmon::Error("white.sdl.renderer", SDL_GetError());
  }
  int pixel_width = options_.width;
  int pixel_height = options_.height;
  SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
  display_scale_ = SDL_GetWindowDisplayScale(window_);
  surface_ = std::make_unique<RasterSurface>(pixel_width, pixel_height);
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA32,
                               SDL_TEXTUREACCESS_STREAMING, pixel_width,
                               pixel_height);
  if (!texture_) {
    throw tokmon::Error("white.sdl.texture", SDL_GetError());
  }
  SDL_StartTextInput(window_);
}

Window::~Window() {
  if (window_) SDL_StopTextInput(window_);
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
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

void Window::set_builtin_chrome(bool enabled) {
  std::lock_guard lock(mutex_);
  builtin_chrome_ = enabled;
  dirty_ = true;
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
      surface_->width(), surface_->height(), SDL_PIXELFORMAT_BGRA32,
      const_cast<void*>(surface_->pixels()),
      static_cast<int>(surface_->row_bytes()));
  if (!screenshot)
    throw tokmon::Error("white.sdl.screenshot", SDL_GetError());
  const auto saved = SDL_SaveBMP(screenshot, path.string().c_str());
  SDL_DestroySurface(screenshot);
  if (!saved)
    throw tokmon::Error("white.sdl.screenshot", SDL_GetError());
}

int Window::run() {
  running_ = true;
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
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        int width = 1;
        int height = 1;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        display_scale_ = SDL_GetWindowDisplayScale(window_);
        surface_->resize(width, height);
        if (texture_) SDL_DestroyTexture(texture_);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA32,
                                     SDL_TEXTUREACCESS_STREAMING, width,
                                     height);
        dirty_ = true;
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
          if (event.type == SDL_EVENT_MOUSE_MOTION) {
            ui.type = "pointermove";
            ui.x = event.motion.x * display_scale_;
            ui.y = event.motion.y * display_scale_;
          } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            ui.type = "wheel";
            float mouse_x = 0;
            float mouse_y = 0;
            SDL_GetMouseState(&mouse_x, &mouse_y);
            ui.x = mouse_x * display_scale_;
            ui.y = mouse_y * display_scale_;
            ui.delta_x = event.wheel.x;
            ui.delta_y = -event.wheel.y * 48.0F;
          } else {
            ui.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                          ? "pointerdown"
                          : "click";
            ui.x = event.button.x * display_scale_;
            ui.y = event.button.y * display_scale_;
          }
          callback(std::move(ui));
          dirty_ = true;
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
    if (dirty_.exchange(false, std::memory_order_acq_rel)) {
      render();
    }
    SDL_Delay(8);
  }
  return 0;
}

void Window::render() {
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
  surface_->clear({247, 248, 250, 255});
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

  SDL_UpdateTexture(texture_, nullptr, surface_->pixels(),
                    static_cast<int>(surface_->row_bytes()));
  SDL_SetRenderDrawColor(renderer_, 247, 248, 250, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
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
