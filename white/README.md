# White native HTML/CSS UI

White is Tokmon's Arche-composed retained-mode UI engine. It deliberately
implements a deterministic HTML/CSS subset instead of browser compatibility:

```text
HTML/CSS -> retained DOM and cascade -> Yoga Flex layout
         -> retained PaintTree and damage regions -> Skia -> SDL3
```

The primary API parses markup and styles once. There is no JavaScript runtime;
product state and commands remain typed C++ or immutable JSON.

```cpp
#include <white/assembly.hpp>

arche::Runtime runtime("product");
white::Assembly white(runtime);
auto view = white.views().create(
    R"(<main id="app"><button data-native="product.run">Run</button></main>)",
    R"(#app { flex-grow:1; padding:12px; })");
view->set_state({{"ready", true}});
view->layout(width, height);
view->render(surface);
```

## Supported model

- Lexbor parses HTML into a stable White DOM.
- The CSS subset supports type/class/id and descendant selectors, cascade,
  inheritance, `:hover`, `:focus`, Flex layout, box metrics, colors, borders,
  typography, absolute positioning and overflow.
- Yoga performs layout in display-independent logical units.
- PaintTree mirrors the DOM and is reconciled only when the document revision
  changes. Damage history lets each raster surface repaint only changes it has
  not consumed.
- Skia performs text shaping and rendering. SDL desktop windows prefer the
  Ganesh/OpenGL GPU path and automatically fall back to Skia CPU rasterization
  when GPU contexts are unavailable (including headless tests).
- `data-native="capability.id"` mounts native components from the composed
  `white.native-components` registry. Editors, virtual lists, file trees and
  other complex controls stay native without escaping the HTML layout model.

## Capability graph

White installs independent `white.*` capabilities for DOM parsing, style,
layout, retained scene construction, native components, HTML views, the legacy
JSON declarative adapter, Skia rendering and SDL3 window hosting. Applications
extend the graph with product plugins instead of modifying White globals.

The JSON `DeclarativeView` API remains as a compatibility adapter, but new
product UI should use `HtmlView` and CSS.

The complete engine contract and module map are documented in
`docs/WHITE_UI_ARCHITECTURE.md`.
