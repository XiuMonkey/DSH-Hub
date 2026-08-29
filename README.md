# DSH Hub

DSH Hub is a Windows desktop client for **DSH (DeepSeek Harness)**, built with **Qt 6 / C++**.

It connects to a local DSH server through HTTP and WebSocket APIs, provides a chat-style interface, and renders rich agent output including Markdown, code blocks, tables, streaming responses, tool calls, and native DLL extension results.

## Features

- 🖥️ Qt 6 desktop UI (Windows)
- 🔌 Connects to DSH local server via HTTP + WebSocket
- 💬 Chat-style message interface
- 📝 Markdown rendering with code blocks and tables
- 🎨 Code syntax highlighting based on `highlight_rules.json`
- 📜 Session/workspace sidebar with right-click session deletion
- ⚡ Streaming agent output with throttled UI updates
- ⌨️ Auto-growing chat input (`Enter` to send, `Shift+Enter` for newline)
- ⚙️ Settings popup and theme switching
- 🧩 Plugin market and extension management
- 🧬 Native DLL extension bridge
  - JSON-style and native-style calling conventions
  - `LoadingSource` support so one extension can load functions from multiple DLLs
- 🎞️ Example FFmpeg extension
  - `ffmpeg_probe`
  - `ffmpeg_convert`
  - `ffmpeg_extract_frame`
  - `ffmpeg_extract_audio`
  - `ffmpeg_compress_video`
  - `ffmpeg_video_to_gif`
- 🔄 Loading indicators for initialization and message loading

## Project Structure

```text
DSH Hub/
├── include/                    # C++ headers
├── src/
│   ├── core/                   # Main window, server manager, entry point
│   ├── ui/                     # Sidebar, chat input, popups, plugin/extension managers
│   ├── chat/                   # Message list/units, history loading and caching
│   ├── network/                # HTTP/WebSocket client, event parser, session prefetcher
│   ├── bridge/                 # Named pipe bridge, DLL JSON caller, extension loader
│   └── common/                 # Logger, theme manager, code highlighter, interaction dialogs
├── resources/
│   ├── icons/ images and QRC
│   ├── highlight_rules.json    # Syntax highlighting rules
│   ├── registry-snapshot.json  # Plugin market offline snapshot
│   └── server/
│       └── launch-root/
│           └── FFmpegExt/      # Example FFmpeg extension package
├── unit_test/                  # Qt Test unit tests
├── test extension/             # Local extension testing workspace
├── DSH Hub.slnx
├── DSH Hub.vcxproj
├── DSH Hub.Tests.vcxproj
└── README.md
```

## Requirements

- Windows 10 or later
- Visual Studio 2022 or a compatible MSVC toolset
- Qt 6.11.2 MSVC 2022 x64 (or a compatible Qt 6 version)
- DSH local server running locally

## Build

1. Open `DSH Hub.slnx` in Visual Studio.
2. Select **x64** configuration.
3. Build the solution.

The project uses the following Qt modules:

- Core
- GUI
- Widgets
- Network
- WebSockets

## Unit Tests

Unit tests are located in `unit_test/` and are built as a separate console project (`DSH Hub.Tests.vcxproj`) using Qt Test.

To run the tests:

1. Open `DSH Hub.slnx` in Visual Studio.
2. Select **x64** configuration and **Debug** (or Release).
3. Build the `DSH Hub.Tests` project.
4. Run the generated `DSH Hub.Tests.exe`.

The test project currently covers:

- `DshEventParser` – JSON event parsing helpers (`extractEventText`, `extractToolCall`, `extractApproval`, etc.)
- `CodeHighlighter` – syntax highlighting and HTML escaping
- `HistoryManager` – history loading state management

## Extension Format

Extensions are packaged as `.ext` files (ZIP format) with this layout:

```text
Extension.ext
├── main.dll              # Primary native DLL
├── regulation.json5      # DLL tool descriptor
├── AttachedPlugin/       # DSH plugin that registers the tools
│   ├── package.json
│   └── index.js
└── bin/                  # Optional runtime dependencies (e.g. FFmpeg DLLs)
```

`regulation.json5` supports a `LoadingSource` field on each function so tools can be exported from different DLLs:

```json5
{
  "Func": "my_function",
  "LoadingSource": "main.dll",
  "Tool": "my_tool"
}
```

## Usage

1. Start the DSH local server.
2. Launch DSH Hub.
3. The application will automatically connect to the DSH server during initialization.
4. Select or create a session from the session list.
5. Type a message and press `Enter` or click the send button.