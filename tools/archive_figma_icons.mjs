import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [sourceRoot, outputRoot] = process.argv.slice(2);
if (!sourceRoot || !outputRoot) {
  throw new Error("usage: node archive_figma_icons.mjs <figma-make-root> <output-root>");
}

const moduleUrl = (relativePath) =>
  pathToFileURL(join(sourceRoot, "node_modules", relativePath)).href;
const React = await import(moduleUrl("react/index.js"));
const { renderToStaticMarkup } = await import(
  moduleUrl("react-dom/server.node.js")
);
const lucide = await import(
  moduleUrl("lucide-react/dist/esm/lucide-react.mjs")
);

const icons = {
  ArrowLeft: "arrow-left",
  ArrowUp: "arrow-up",
  Bell: "bell",
  Bot: "bot",
  Box: "box",
  Brain: "brain",
  Check: "check",
  CheckCircle2: "check-circle-2",
  ChevronDown: "chevron-down",
  ChevronRight: "chevron-right",
  CircleDashed: "circle-dashed",
  Copy: "copy",
  Cpu: "cpu",
  Download: "download",
  Edit2: "edit-2",
  FileCode: "file-code",
  FileText: "file-text",
  Folder: "folder",
  Keyboard: "keyboard",
  Lock: "lock",
  MessageSquare: "message-square",
  Minus: "minus",
  MoreVertical: "more-vertical",
  Palette: "palette",
  PanelLeftClose: "panel-left-close",
  PanelLeftOpen: "panel-left-open",
  PanelRightClose: "panel-right-close",
  PanelRightOpen: "panel-right-open",
  Paperclip: "paperclip",
  Plus: "plus",
  RotateCw: "rotate-cw",
  Search: "search",
  Send: "send",
  Settings: "settings",
  ShieldAlert: "shield-alert",
  Sliders: "sliders",
  Sparkles: "sparkles",
  Square: "square",
  Terminal: "terminal",
  User: "user",
  X: "x",
};

await mkdir(outputRoot, { recursive: true });
for (const [componentName, fileName] of Object.entries(icons)) {
  const Icon = lucide[componentName];
  if (!Icon) throw new Error(`missing Lucide component: ${componentName}`);
  const markup = renderToStaticMarkup(
    React.createElement(Icon, {
      color: "currentColor",
      height: 24,
      strokeWidth: 2,
      width: 24,
      xmlns: "http://www.w3.org/2000/svg",
    }),
  );
  await writeFile(join(outputRoot, `${fileName}.svg`), `${markup}\n`, "utf8");
}

console.log(`Archived ${Object.keys(icons).length} Lucide icons in ${outputRoot}`);
