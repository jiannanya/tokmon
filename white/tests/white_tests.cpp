#include <white/document.hpp>
#include <white/renderer.hpp>
#include <white/text_editor.hpp>
#include <white/virtual_list.hpp>
#include <white/assembly.hpp>

#include <cassert>
#include <iostream>

int main() {
  arche::Runtime white_runtime("white-test");
  white::Assembly white_assembly(white_runtime);
  assert(white_assembly.composition_report().actions.size() == 5);
  auto composed_document = white_assembly.dom().parse(
      "<div id=\"composed\">capability graph</div>");
  white_assembly.styles().apply(
      composed_document, "#composed { width: 200px; height: 40px; }");
  white_assembly.layout().layout(composed_document, 320, 200);
  white::RasterSurface composed_surface(320, 200);
  white_assembly.renderer().render(composed_surface, composed_document);
  assert(composed_document.find_by_id("composed")->layout().width > 0);

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

  std::cout << "white_tests: ok\n";
  return 0;
}
