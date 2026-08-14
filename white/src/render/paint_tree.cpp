#include <white/paint_tree.hpp>

namespace white {
namespace {

std::size_t sync_node(PaintNode &paint, const Node &node) {
  paint.source = &node;
  paint.bounds = node.layout();
  paint.style = node.style();
  paint.text = node.text();
  paint.scroll_offset_y = node.scroll_offset_y();
  paint.content_height = node.content_height();
  paint.focused = node.focused();
  paint.children.resize(node.children().size());
  std::size_t count = 1;
  for (std::size_t index = 0; index < node.children().size(); ++index)
    count += sync_node(paint.children[index], *node.children()[index]);
  return count;
}

} // namespace

bool PaintTree::sync(const Document &document) {
  if (revision_ == document.revision()) return false;
  node_count_ = sync_node(root_, document.root());
  revision_ = document.revision();
  return true;
}

} // namespace white
