# DSH Hub

DSH Hub is a Windows desktop client for **DSH (DeepSeek Harness)**, built with **Qt 6 / C++**.

It connects to a local DSH server through HTTP and WebSocket APIs, provides a chat-style interface, and renders rich agent output including Markdown, code blocks, tables, streaming responses, tool calls, and native DLL extension results.

## Features

- 🖥️ Qt 6 desktop UI (Windows), frameless with a self-drawn title bar and a shared overlay for popups
- 🔌 Connects to a local DSH server via HTTP + WebSocket (mux + host streams)
- 💬 Chat-style message interface with streaming, throttled updates and load-more
- 📝 Markdown rendering with code blocks, tables and syntax highlighting (`highlight_rules.json`)
- 📜 Session/workspace sidebar: groups, rename-free titles, archive/delete, cross-session history cache
- ⌨️ Auto-growing chat input (`Enter` to send, `Shift+Enter` for newline)
- ⚙️ Settings popup, theme switching (light/dark), replaceable style sets (QSS + palette; wallpaper supported — see `StyleExample/`)
- 🌐 ID-based i18n with built-in `zh_CN` / `en` packs
- 🧩 Plugin market and extension manager
- 🧬 **Tool extensions** — native DLL bridge called by the DSH server
  - JSON-style and native-style calling conventions, `LoadingSource` for multi-DLL extensions, COM interface
- 🪟 **Client extensions** — Qt plugin (QPlugin) DLLs loaded **into this client process**
  - Run on the GUI thread and manipulate the host UI directly (e.g. add a widget to the top bar)
  - Delivered as the same `.ext` package; routed by the `Type` field in `regulation.json5`
- 🔗 **Host C exports + global object registry**
  - The host `.exe` exports a small C ABI, so plugins resolve host objects at runtime with zero link-time dependency
  - Program-wide unique objects (main window / sidebar / top bar) are registered under string indexes
- 🔄 Loading indicators for initialization and message loading

## Project Structure

```text
DSH Hub/
├── include/                     # Headers, grouped to mirror src/ (see below)
├── src/
│   ├── core/                    # Main window, MessageHost, server manager, entry point, host C exports
│   ├── ui/                      # UI controls & drawing only: sidebar, chat input, popups, settings
│   ├── chat/                    # Message units, query/cache, code block view
│   ├── network/                 # HTTP/WebSocket client, event parser, session prefetcher
│   ├── ExtensionSystem/         # Named pipe bridge, DLL JSON caller, thunk generator,
│   │                            # extension loader, client-extension loader
│   └── common/                  # Non-UI logic, split into five groups:
│       ├── util/                #   logger, common registry, code highlighter, markdown preprocess
│       ├── settings/            #   settings store, client settings
│       ├── session/             #   session catalog/service, agent presets, model selection
│       ├── extension/           #   extension registry, install task, plugin market client/installer
│       └── appearance/          #   theme, window frame, card shadow, translation, interaction
├── resources/
│   ├── styles/                  # Default style set: 12 QSS modules + 2 palette templates (no wallpaper)
│   ├── translations/            # Built-in .qm language packs (qrc fallback)
│   ├── ToolsFilterPlugin/       # Tools-filter cordis plugin (server side)
│   ├── EXTENSION_DEV.zh-CN.md   # Tool-extension development guide
│   ├── highlight_rules.json     # Syntax highlighting rules
│   └── server/                  # Bundled DSH server (harness profile, launch-root)
├── unit_test/                   # Qt Test unit tests
├── StyleExample/                # Example full style set (QSS + palette + wallpaper) to copy into <exe>/styles/
├── CMake/                       # CMake build (source of truth for the CLI build)
├── tools/                       # Build / i18n / session-log helper scripts
├── DSH Hub.slnx
├── DSH Hub.vcxproj (+ .filters) # Main target
├── DSH Hub.Tests.vcxproj        # Test target
├── README.md
└── README.zh-CN.md
```

`include/` mirrors `src/` (plus `VirtualClass/` for the cross-DLL plugin interface), and it is a
flat include root: every `#include "..."` resolves as `include/<group>/<Header>.h`.

## Requirements

- Windows 10 or later
- Visual Studio 2022 or a compatible MSVC toolset
- Qt 6.11.2 MSVC 2022 x64 (or a compatible Qt 6 version)
- DSH local server running locally

## Build

**Visual Studio:** open `DSH Hub.slnx`, select **x64**, build.

**CMake + Ninja (CLI):**

```bash
# bash on Windows; exports the MSVC environment, then runs cmake --build
"<PortableGit>/bin/bash.exe" tools/rebuild.sh            # main target
"<PortableGit>/bin/bash.exe" tools/rebuild.sh dshhub_tests
```

`tools/rebuild.sh` kills a running instance first (Windows locks the `.exe`), configures
`build/windows-ninja` on first run, and forwards extra arguments to `cmake`.

Qt modules used:

- Core
- GUI
- Widgets
- Network
- WebSockets

## Unit Tests

Tests live in `unit_test/` and build as a separate console target (`DSH Hub.Tests`). Current
baseline: **209 passed / 0 failed / 3 skipped** across 15 classes.

| Test | Covers |
|---|---|
| `TestDshEventParser` | mux event parsing helpers (text, tool calls, approvals) |
| `TestDshApiClient` | RPC call plumbing, URL/token assembly |
| `TestServerManager` | server start/restart and readiness decisions |
| `TestSessionCatalog` | session/workspace parsing: subagent filtering, title fallback, archived filtering, workspace assignment, auto-select |
| `TestSessionPrefetcher` | session prefetch targets |
| `TestHistoryManager` | history loading state and cache |
| `TestMarkdownPreprocess` | `<br>` replacement and table rendering (Qt 6 regression) |
| `TestCodeHighlighter` | syntax highlighting and HTML escaping |
| `TestThunk` | x86-64 thunk generator: calling conventions, argument types, signature JSON, end-to-end DLL calls |
| `TestPluginMarketModel` | market entry parsing, keyword filtering, pagination slicing |
| `TestModelSelection` | model / reasoning-effort selection resolution |
| `TestSettingsStore` | `settings/update` merge and persistence |
| `TestClientSettings` | `ClientSetting/*.json` read/write |
| `TestTranslationManager` | ID-based translation lookup and fallback |
| `TestLogger` | log levels and per-module file routing |

> `TestThunk` has cases that depend on an example extension package directory, which is not
> part of the repo. When it is absent those cases are reported as **skipped**, not failed
> (that is where the 3 skips in the baseline come from).

## Extensions

`.ext` files are ZIP packages. **Two unrelated routes share that container**, distinguished by
the `Type` field in `regulation.json5`:

### 1. Tool extensions (no `Type`, has `Function[]`)

Installed into the DSH **server** profile, called through JSON text protocol by `DllCaller`
on worker threads. Package layout and the full descriptor reference (json / native / com
styles, `AttachedPlugin`, install & uninstall behaviour, threading model, FAQ):

➡️ **[`resources/EXTENSION_DEV.zh-CN.md`](resources/EXTENSION_DEV.zh-CN.md)**

```text
MyTool.ext
├── main.dll
├── regulation.json5          # Function[] descriptor
├── AttachedPlugin/           # DSH plugin that registers the tools
│   ├── package.json
│   └── index.js
└── bin/                      # optional runtime dependencies
```

### 2. Client extensions (`Type: ClientExtension` / `ClientExtensionDebug`)

A Qt plugin DLL loaded **into the DSH Hub process** on the GUI thread. It receives host objects
through the host's C exports and can modify the host UI directly.

```text
MyClientExt.ext
├── regulation.json5          # { "Name": ..., "Type": "ClientExtensionDebug" }
└── my_ext.dll                # QPlugin (Q_PLUGIN_METADATA)
```

- Installed to `<exe>/clientExtensions/<Name>/` — `regulation.json5` there is the "installed"
  marker used by loading, listing and removal.
- The host main window implements the **`VirtualWindow`** interface from
  `VirtualClass/VirtualCommon.h`: an extension casts the `mainWindow` object it got from the registry
  to `VirtualWindow*` and can then have its own top-level window treated as a host popup
  (`ExternalShowOverlay()` / `ExternalHideOverlay()` — scrim + centered, the same path the
  settings/plugin-market popups take). The cast goes through `qt_metacast(IID)` and the call through
  the vtable, so a plugin imports no host symbols. Two more optional hooks exist: `detachHost()`
  (see `include/core/DshHostPlugin.h`) and `VirtualTheme` (same VirtualCommon.h).
- **Install copies the whole package**: everything in the `.ext` besides `main.dll` /
  `regulation.json5` — files and subdirectories — lands in `<exe>/clientExtensions/<Name>/`, so an
  extension can ship its own resources (for example its own `styles/`).
- Removal has two paths:
  - The plugin implements the optional `detachHost()` slot (see `include/core/DshHostPlugin.h`)
    — the host lets it tear down everything it put on screen and then really `unload()`s it: the
    dll is unmapped and the extension directory is **deleted on the spot**;
  - Otherwise (older plugins) the `regulation.json5` marker is deleted (from that moment it no
    longer counts as installed and won't be loaded again), but a dll still mapped by this
    process cannot be deleted, so a `.pending-removal` marker is left for the sweep at the
    **next start** (top of `loadAll()`).
  Neither path leaves a directory that still looks installed. Note that renaming/moving the
  directory does not get around the Windows file lock either, so without `detachHost()` the only
  fallback is cleaning up at the next start.
- `Type` is matched case-insensitively; anything else routes to the tool-extension path.
- The plugin must be built in the **same configuration as the host** (Debug/Release, same Qt
  binary, same MSVC toolset) — Qt rejects mixed debug/release plugins at load time.
- Authoring guide and a working example (top-bar button):

➡️ **[`../QPluginExtension/README.md`](../QPluginExtension/README.md)**

### Host C exports and the global object registry

The host `.exe` exports two `extern "C"` symbols (`DshHubHostAbiVersion`,
`DshHubHostRegistryFind`). Plugins resolve them at runtime via `QLibrary::resolve`, so a plugin
DLL imports **no host symbols** and needs no `dllimport` / import library.

Registered objects (one instance per process): `mainWindow`, `sidebar`, `topbar`, `themeManager`.
Queries must run on the GUI thread; see `include/core/HostExports.h`. `themeManager` is the
stylesheet singleton (`ThemeManager`, implementing the `VirtualTheme` interface); a plugin can cast
it to `VirtualTheme*` and:

- `ExternalApplyToWindow(window)` — apply the current theme's stylesheet to a window of its own;
- `ExternalReloadStyles()` — make the host re-read the local style files (`*.qss` and
  `theme-*.json` under `<exe>/styles/`, external first with the qrc as fallback) and re-mount the
  result on **every** window. A plugin that overwrites those files itself calls this to restyle the
  whole UI; a `false` return means the reloaded stylesheet is empty (files unreadable: write
  failure, wrong path or missing permissions), so it doubles as a self-check. It does not rebuild
  windows, so anything fixed at construction time (e.g. logo resources chosen per theme) stays put.

`VirtualTheme` virtual methods may only be **appended at the end** (host vtable slots follow
declaration order, while plugin DLLs are compiled separately).

## Usage

1. Start the DSH local server.
2. Launch DSH Hub.
3. The application connects to the DSH server during initialization.
4. Select or create a session from the sidebar.
5. Type a message and press `Enter` or click the send button.
