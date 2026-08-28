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
├── src/
│   ├── core/       # 主窗口与程序入口（DSHHub、main）
│   ├── ui/         # 通用界面组件（输入框、侧边栏、弹窗、加载控件等）
│   ├── chat/       # 会话/消息相关组件（消息列表、消息单元、缓存等）
│   ├── network/    # DSH 服务通信（HTTP/WebSocket、事件解析、会话预取）
│   ├── bridge/     # 桥接层（命名管道、DLL 调用）
│   └── common/     # 公共基础设施（日志、主题、代码高亮）
├── include/        # C++ 头文件
├── resources/      # 图片、图标、QRC、UI、JSON 规则及运行资源
├── DSH Hub.vcxproj
├── DSH Hub.slnx
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

## Unit Tests

Unit tests are located in `unit_test/` and are built as a separate console project (`DSH Hub.Tests.vcxproj`) using Qt Test.

To run the tests:

1. Open `DSH Hub.slnx` in Visual Studio.
2. Select **x64** configuration and **Debug** (or Release).
3. Build the `DSH Hub.Tests` project.
4. Run `x64\Debug\DSH Hub.Tests.exe` (or `x64\Release\DSH Hub.Tests.exe`).

The test project currently covers:

- `DshEventParser` – JSON event parsing helpers (`extractEventText`, `extractToolCall`, `extractApproval`, etc.)
- `CodeHighlighter` – syntax highlighting and HTML escaping
- `HistoryManager` – history loading state management

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
