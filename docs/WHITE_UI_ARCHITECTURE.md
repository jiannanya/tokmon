# White UI architecture

White is an HTML/CSS-style native retained-mode engine, not an embedded web
browser. Product UI is described with a deterministic markup and style subset,
while application state, commands, security boundaries and complex controls
remain native C++ capabilities composed by Arche.

```text
HTML + CSS resources
        |
        v
Lexbor parser -> White retained DOM -> CSS cascade
                                      |
                                      v
                                 Yoga Flex layout
                                      |
                                      v
                         retained PaintTree + damage journal
                                      |
                         +------------+------------+
                         |                         |
                         v                         v
                   Skia Ganesh GPU          Skia raster CPU
                         |                         |
                         +------------+------------+
                                      |
                              SDL3 native window
```

## Module ownership

White source code is physically separated by responsibility:

```text
white/src/
  composition/   Arche capability assembly
  components/    HtmlView, native registry, compatibility adapter
  dom/           DOM, CSS cascade, Yoga projection and input routing
  input/         UTF-8 editor behavior
  render/        PaintTree, damage regions and Skia backends
  platform/      SDL3 window, DPI conversion and presentation loop
```

Tokmon follows the same rule:

```text
tokmon/src/
  application/     process and window orchestration
  composition/     product plugin graph
  domain/          projection, approvals and settings
  infrastructure/  Snow child-process transport
  ui/components/   product-native White component registrations
  ui/shell/        HTML/CSS resource ownership and region mapping
  ui/workbench/    workbench behavior and native complex controls
tokmon/ui/
  workbench.html
  workbench.css
```

Public headers stay under `include/` so reorganizing implementation modules
does not break consumers.

## Plugin graph

White installs separate Arche capabilities for DOM parsing, style resolution,
layout, retained scenes, native component registration, HTML views, rendering,
window runtime and the legacy JSON adapter. Tokmon installs its workbench as a
product plugin and registers the `data-native` boundaries from that plugin.
Removing the plugin unregisters those factories through the Arche unload
lifecycle.

Native components receive immutable JSON state, their owning DOM node and the
current damage region. They can implement virtualized lists, editors, file
trees or other controls without adding JavaScript or browser semantics.

## Supported CSS contract

The supported subset includes tag/class/id and descendant selectors,
specificity and source order, inline styles, `:hover`, `:focus`, Flex rows and
columns, grow/shrink, alignment, gaps, box dimensions, margin/padding, borders,
colors, opacity, typography, absolute positioning and overflow/scrolling.

White intentionally does not implement JavaScript, networking, browser
navigation, arbitrary web APIs, full CSS Grid, browser quirks or pixel-identical
compatibility with Chrome. New syntax is added only when a native product needs
it and when it can remain deterministic.

## Performance invariants

- DOM, component instances, paint nodes and shaped text are retained.
- Style and Yoga layout run only when their invalidation flags require them.
- Each document keeps a bounded revision/damage journal, so multiple surfaces
  consume changes independently.
- Hover, caret and scrolling updates invalidate local rectangles; Tokmon clips
  native painting and CPU texture uploads to those rectangles.
- Pointer events are drained and coalesced before a frame, and unchanged hover
  regions do not schedule frames.
- Windows border resizing is woken on the initial non-client edge press and
  follows actual `WM_WINDOWPOSCHANGED` messages instead of a periodic timer.
  Every coalesced size recomputes HTML/CSS/Yoga layout and repaints against the
  current logical viewport, so typography and controls retain their proportions
  while responsive panels cross breakpoints during the drag. The physical
  surface is retained as capacity while a pixel-exact viewport maps one-to-one
  onto the native framebuffer; capacity grows with headroom only when needed,
  avoiding both per-frame allocation and low-resolution scaling blur. The
  modal resize exit synchronously submits the final exact high-DPI frame. Other
  platforms retain the SDL live-resize expose fallback.
- Normal desktop windows prefer Skia Ganesh/OpenGL. Unsupported, remote or
  headless environments automatically fall back to Skia raster plus SDL's
  accelerated presenter.
- GPU mode renders into a persistent offscreen Skia surface before presenting,
  preserving retained contents across swap-chain flips.
- Layout uses logical display-independent units; raster dimensions follow the
  SDL display scale multiplied by Tokmon's product UI scale.

These are engine contracts rather than Tokmon-specific conventions, so future
Arche products can reuse White without inheriting the workbench.
