# White declarative UI

White is Tokmon's Arche-composed native retained-mode UI toolkit. Its primary
integration API is a QML-like, strict-JSON document rather than an immediate
sequence of drawing calls.

```cpp
#include <white/declarative.hpp>

auto view = white::DeclarativeView::load("workbench.white.json");
view->set_command_handler([](const white::Command& command) {
  // Route command.name and command.arguments to the product reducer.
});
view->set_state({{"title", "Tokmon"}, {"sessions", sessions}});
view->layout(width, height);
view->render(surface);
```

## Document model

Every document uses the `org.tokmon.white.view/v1` schema and contains one
`root`. Elements have `type`, optional `id`/`key`, `properties`, `on`, and
`children`. Common properties may also be written directly beside `type` for
readability. JSON is parsed once into `ViewBlueprint`; frames never reparse the
source document. File loading accepts only the `.json` extension; YAML and
JSON-with-comments are not part of the format.

Bindings use explicit JSON expressions:

- `{"$bind":"path.to.value","default":"fallback"}`
- `{"$eq":[left,right]}`, `{"$not":value}`
- `{"$and":[...]}`, `{"$or":[...]}`, `{"$concat":[...]}`

`If` conditionally expands children. `Repeater` expands an array/object model
with `as`, `indexAs`, and `keyPath`. Keys preserve focus and scrolling when a
new immutable state snapshot is reconciled.

## Components and commands

Documents may define reusable composite components. Their properties are
available under `props`; `Slot` inserts the caller's children. Applications can
also extend a local `ComponentRegistry`, without a process-global registry.

Event handlers declare commands. White adds the source's current `value` and
the original `UiEvent`, then calls the host command handler. White never mutates
product state or executes JavaScript.

The default vocabulary includes layout (`Item`, `Row`, `Column`, `Spacer`,
`SplitPane`, `Overlay`), text and input (`Text`, `TextField`, `TextArea`,
`CheckBox`), actions (`Button`, `IconButton`), collections (`ScrollView`,
`ListView`, `ListItem`), and surfaces (`Dialog`, `Menu`, `Tabs`, `Toolbar`,
`Badge`, `ProgressBar`). Low-level DOM/CSS APIs remain available for adapters
and specialized paint contributions.
