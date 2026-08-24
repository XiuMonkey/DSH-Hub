# DSH Hub

DSH Hub is a desktop client for **DSH (DeepSeek Harness)**, built with **Qt 6 / C++**.

It connects to a local DSH server through HTTP and WebSocket APIs, provides a chat-style interface, and renders rich agent output including Markdown, code blocks, tables, and streaming responses.

## Features

- 🖥️ Qt 6 desktop UI (Windows)
- 🔌 Connects to DSH local server via HTTP + WebSocket
- 💬 Chat-style message interface
- 📝 Markdown rendering with code blocks and tables
- 🎨 Code syntax highlighting based on `highlight_rules.json`
- 📜 Session list and history loading
- ⚡ Streaming agent output with throttled UI updates
- ⌨️ Auto-growing chat input
  - `Enter` to send
  - `Shift+Enter` to insert a newline
- ⚙️ Settings popup
- 🔄 Loading indicators for initialization and message loading

## Project Structure

```text
DSH Hub/
├── src/                  # C++ source files
├── include/              # C++ header files
├── resources/            # Images, icons, QRC, UI, JSON rules, and runtime resources
├── DSH Hub.vcxproj       # Visual Studio project file
├── DSH Hub.slnx          # Visual Studio solution file
└── README.md
```

## Requirements

- Windows 10 or later
- Visual Studio 2022 or compatible MSVC toolset
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

## Usage

1. Start the DSH local server.
2. Launch DSH Hub.
3. The application will automatically connect to the DSH server during initialization.
4. Select or create a session from the session list.
5. Type a message and press `Enter` or click the send button.

## Notes

- The `resources/server` directory is used at runtime by the DSH harness.
- `node.exe` and `node_modules` are intentionally ignored by Git; make sure they are available in the runtime environment if required.
- Build outputs under `x64/` and Visual Studio cache under `.vs/` are not tracked by Git.
