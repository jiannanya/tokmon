#include <white/document.hpp>
#include <white/declarative.hpp>
#include <white/renderer.hpp>
#include <white/text_editor.hpp>
#include <white/virtual_list.hpp>
#include <white/assembly.hpp>
#include <white/html_view.hpp>

#include <cassert>
#include <iostream>

namespace {

class TestNativeComponent final : public white::NativeComponent {
public:
  explicit TestNativeComponent(int* clicks) : clicks_(clicks) {}

  void paint(white::RasterSurface& surface, const white::Node& node,
             const white::DamageRegion&) override {
    surface.fill_rect(node.layout(), {20, 112, 226, 255}, 4);
  }

  bool dispatch(white::Node&, white::UiEvent& event) override {
    if (event.type != "click") return false;
    ++*clicks_;
    return true;
  }

private:
  int* clicks_;
};

} // namespace

int main() {
  arche::Runtime white_runtime("white-test");
  white::Assembly white_assembly(white_runtime);
  assert(white_assembly.composition_report().actions.size() == 9);
  int native_clicks = 0;
  white_assembly.native_components().register_factory(
      "test.button", [&] {
        return std::make_unique<TestNativeComponent>(&native_clicks);
      });
  auto html_view = white_assembly.views().create(
      R"(<main id="app"><button id="native" data-native="test.button">Run</button></main>)",
      R"(body { background:#ffffff; } #app { flex-grow:1; padding:8px; }
          #native { width:120px; height:40px; background:#eeeeee; }
          #native:hover { background:#dddddd; })");
  html_view->layout(320, 200);
  white::RasterSurface html_surface(320, 200);
  html_view->render(html_surface);
  assert(html_surface.frame_metrics().full_repaint);
  const auto* native_node = html_view->find_by_id("native");
  assert(native_node);
  const auto native_bounds = native_node->layout();
  assert(html_view->pointer_move(native_bounds.x + 2, native_bounds.y + 2));
  html_view->render(html_surface);
  assert(!html_surface.frame_metrics().full_repaint);
  assert(html_view->dispatch(
      {.type = "click", .x = native_bounds.x + 2, .y = native_bounds.y + 2}));
  assert(native_clicks == 1);
  auto composed_document = white_assembly.dom().parse(
      "<div id=\"composed\">capability graph</div>");
  white_assembly.styles().apply(
      composed_document, "#composed { width: 200px; height: 40px; }");
  white_assembly.layout().layout(composed_document, 320, 200);
  white::RasterSurface composed_surface(320, 200);
  white_assembly.renderer().render(composed_surface, composed_document);
  assert(composed_document.find_by_id("composed")->layout().width > 0);

  auto declarative = white_assembly.components().parse(R"json(
    {
      "schema": "org.tokmon.white.view/v1",
      "imports": ["White.Controls@1"],
      "components": {
        "SessionRow": {
          "type": "Button",
          "text": {"$bind": "props.title"},
          "height": 36,
          "on": {
            "click": {
              "command": "session.open",
              "arguments": {"id": {"$bind": "props.id"}}
            }
          }
        },
        "Panel": {
          "type": "Column",
          "children": [{"type": "Slot"}]
        }
      },
      "root": {
        "type": "Application",
        "id": "application",
        "gap": 6,
        "padding": 8,
        "children": [
          {"type": "Text", "id": "heading",
           "text": {"$bind": "title"}, "height": 24},
          {"type": "If", "condition": {"$bind": "online"},
           "children": [
             {"type": "Badge", "id": "online", "text": "online",
              "height": 20}
           ]},
          {"type": "Repeater", "key": "sessions",
           "model": {"$bind": "sessions"}, "as": "session",
           "keyPath": "id", "children": [
             {"type": "SessionRow", "id": "session-row",
              "properties": {
                "title": {"$bind": "session.title"},
                "id": {"$bind": "session.id"}
              }}
           ]},
          {"type": "Panel", "children": [
            {"type": "Text", "id": "slotted", "text": "slot content",
             "height": 20}
          ]}
        ]
      }
    }
  )json");
  std::string opened_session;
  declarative->set_command_handler([&](const white::Command& command) {
    assert(command.name == "session.open");
    opened_session = command.arguments.at("id").get<std::string>();
  });
  declarative->set_state({{"title", "Declarative White"},
                          {"online", true},
                          {"sessions", {{{"id", "one"}, {"title", "First"}},
                                        {{"id", "two"}, {"title", "Second"}}}}});
  declarative->layout(420, 300);
  white::RasterSurface declarative_surface(420, 300);
  declarative_surface.clear({255, 255, 255, 255});
  declarative->render(declarative_surface);
  assert(declarative_surface.pixels());
  assert(declarative->find_by_id("heading")->text() == "Declarative White");
  assert(declarative->find_by_id("online"));
  assert(declarative->find_by_id("slotted")->text() == "slot content");
  auto* first_session = declarative->find_by_id("session-row");
  assert(first_session && first_session->text() == "First");
  const auto session_bounds = first_session->layout();
  declarative->dispatch({"click", session_bounds.x + 2, session_bounds.y + 2});
  assert(opened_session == "one");
  declarative->set_state({{"title", "Updated"},
                          {"online", false},
                          {"sessions", {{{"id", "one"}, {"title", "First"}},
                                        {{"id", "two"}, {"title", "Second"}}}}});
  declarative->layout(420, 300);
  assert(!declarative->find_by_id("online"));
  assert(declarative->find_by_id("heading")->text() == "Updated");
  assert(declarative->document().focused());
  assert(declarative->document().focused()->text() == "First");

  auto document = white::Document::parse_html(
      R"(<div id="root"><div class="title">Tokmon &amp; Arche</div><input id="editor" aria-label="Message"/><div id="body">Hello 世界 مرحبا 🙂</div></div>)",
      R"(
        body { padding: 8; background: #ffffff; }
        #root { flex-grow: 1; gap: 6; padding: 10; }
        .title { height: 28; font-size: 20; color: #3050d0; }
        #root .title:hover { color: red; }
        #editor { height: 32; }
        #body { flex-grow: 1; background: #f0f2f8; border-radius: 8; }
      )");
  document.layout(800, 600);
  assert(document.find_by_id("root"));
  assert(document.find_by_id("body"));
  assert(document.find_by_id("root")->layout().width > 0);
  assert(document.find_by_id("body")->layout().height > 0);
  assert(document.find_by_id("root")->children()[0]->text() ==
         "Tokmon & Arche");

  bool captured = false;
  bool bubbled = false;
  document.root().on("click", [&](white::UiEvent&) { captured = true; }, true);
  document.find_by_id("body")->on("click",
                                  [&](white::UiEvent&) { bubbled = true; });
  const auto rect = document.find_by_id("body")->layout();
  document.dispatch({"click", rect.x + 1, rect.y + 1});
  assert(captured && bubbled);

  const auto title_rect = document.find_by_id("root")->children()[0]->layout();
  document.pointer_move(title_rect.x + 1, title_rect.y + 1);
  assert(document.find_by_id("root")->children()[0]->hovered());
  assert(document.find_by_id("root")->children()[0]->style().color ==
         (white::Color{255, 0, 0, 255}));
  document.pointer_move(799, 599);
  assert(document.find_by_id("root")->children()[0]->style().color ==
         (white::Color{48, 80, 208, 255}));

  document.focus_next();
  assert(document.focused() == document.find_by_id("editor"));
  document.text_input("你好🙂");
  assert(document.find_by_id("editor")->text() == "你好🙂");
  document.key_input(8);
  assert(document.find_by_id("editor")->text() == "你好");
  const auto accessibility = document.accessibility_tree();
  assert(accessibility.dump().find("Message") != std::string::npos);

  auto scrolling = white::Document::parse_html(
      R"(<div id="scroll"><div style="height:80px">one</div><div style="height:80px">two</div><div style="height:80px">three</div></div>)",
      R"(#scroll { width: 200px; height: 100px; overflow: scroll; })");
  scrolling.layout(240, 140);
  const auto scroll_rect = scrolling.find_by_id("scroll")->layout();
  scrolling.scroll(scroll_rect.x + 5, scroll_rect.y + 5, 60);
  assert(scrolling.find_by_id("scroll")->scroll_offset_y() > 0);

  white::TextEditor editor;
  editor.set_value("A世界🙂");
  editor.backspace();
  assert(editor.value() == "A世界");
  editor.move_left();
  editor.backspace();
  assert(editor.value() == "A界");
  editor.select_all();
  editor.insert("مرحبا");
  assert(editor.value() == "مرحبا");

  editor.set_value("ab\ncde\nxy");
  editor.set_cursor(5);
  editor.move_up();
  assert(editor.cursor() == 2);
  editor.move_down();
  assert(editor.cursor() == 5);
  editor.move_end();
  assert(editor.cursor() == 6);
  editor.move_home(true);
  assert(editor.selected_text() == "cde");
  editor.move_document_end();
  assert(editor.cursor() == editor.value().size());
  editor.move_document_home(true);
  assert(editor.selected_text() == editor.value());
  editor.set_value("A世界");
  editor.set_cursor(2);
  assert(editor.cursor() == 1);

  white::VirtualList virtual_list;
  virtual_list.configure(10000, 24, 480, 4);
  virtual_list.set_scroll_offset(120000);
  const auto [first, end] = virtual_list.visible_range();
  assert(first > 0 && end <= 10000);
  assert(end - first < 40);

  white::RasterSurface surface(800, 600);
  surface.clear({255, 255, 255, 255});
  surface.render(document);
  surface.render(scrolling);
  const auto paragraph_height = surface.paragraph(
      "White shapes 中文、emoji 🙂 and RTL العربية.",
      {20, 20, 260, 80}, 15, {25, 27, 32, 255}, 500, 1.4F, 3);
  assert(paragraph_height > 0);
  const std::vector<white::RichTextSpan> rich_spans = {
      {"Rich ", 14, {25, 27, 32, 255}, 400},
      {"markdown", 14, {20, 112, 226, 255}, 650},
      {" inline code ", 14, {25, 27, 32, 255}, 400},
      {"Shift+Enter", 13, {25, 27, 32, 255}, 500, true,
       white::Color{235, 236, 238, 255}}};
  assert(surface.rich_paragraph(rich_spans, {20, 105, 260, 80}) > 0);
  surface.push_clip({0, 0, 100, 100});
  surface.fill_circle(50, 50, 20, {20, 112, 226, 255});
  surface.line(10, 10, 90, 90, {255, 255, 255, 255}, 2);
  surface.pop_clip();
  assert(surface.pixels() != nullptr);
  assert(surface.row_bytes() >= 800 * 4);

  surface.begin_frame();
  const white::Rect local_damage{12, 18, 80, 32};
  document.invalidate(white::Invalidation::paint, local_damage);
  surface.render(document);
  const auto retained_damage = surface.frame_damage();
  assert(!retained_damage.empty());
  assert(!retained_damage.full());
  assert(retained_damage.intersects(local_damage));
  assert(!surface.frame_metrics().full_repaint);

  for (const int scale_percent : {100, 125, 150, 200}) {
    const int pixel_width = 320 * scale_percent / 100;
    const int pixel_height = 200 * scale_percent / 100;
    white::RasterSurface scaled_surface(320, 200, pixel_width, pixel_height);
    scaled_surface.clear({0, 0, 0, 0});
    scaled_surface.fill_rect({10, 10, 100, 40}, {20, 112, 226, 255}, 6);
    assert(scaled_surface.width() == 320);
    assert(scaled_surface.height() == 200);
    assert(scaled_surface.pixel_width() == pixel_width);
    assert(scaled_surface.pixel_height() == pixel_height);
    assert(scaled_surface.pixels() != nullptr);
    assert(scaled_surface.row_bytes() >=
           static_cast<std::size_t>(pixel_width) * 4);
    const int sample_x = 100 * scale_percent / 100;
    const int sample_y = 25 * scale_percent / 100;
    const auto* sample = static_cast<const unsigned char*>(
        scaled_surface.pixels()) +
                         static_cast<std::size_t>(sample_y) *
                             scaled_surface.row_bytes() +
                         static_cast<std::size_t>(sample_x) * 4;
    assert(sample[0] || sample[1] || sample[2] || sample[3]);
  }

  std::cout << "white_tests: ok\n";
  return 0;
}
