# DSH Hub

DSH Hub 是 **DSH（DeepSeek Harness）** 的桌面客户端，使用 **Qt 6 / C++** 构建。

它通过 HTTP 和 WebSocket API 连接本地 DSH 服务端，提供聊天式界面，并支持渲染 Markdown、代码块、表格以及流式响应等富文本 Agent 输出。

## 功能特性

- 🖼️ Qt 6 桌面界面（Windows）
- 🔌 通过 HTTP + WebSocket 连接本地 DSH 服务端
- 💬 聊天式消息界面
- 📑 支持 Markdown 渲染，包括代码块和表格
- 🎨 基于 `highlight_rules.json` 的代码语法高亮
- 📂 会话列表与历史记录加载
- ⚡ 流式 Agent 输出，带节流 UI 更新
- 📏 自动增长聊天输入框
  - `Enter` 发送消息
  - `Shift+Enter` 插入换行
- ⚙️ 设置弹窗
- 🔄 初始化与消息加载时的加载指示器

## 项目结构

```text
DSH Hub/
├── src/
│   ├── core/       # 主窗口与程序入口（DSHHub、ServerManager、main）
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

## 环境要求

- Windows 10 或更高版本
- Visual Studio 2022 或兼容的 MSVC 工具集
- Qt 6.11.2 MSVC 2022 x64（或其他兼容的 Qt 6 版本）
- 本地运行 DSH 服务端

## 构建

1. 在 Visual Studio 中打开 `DSH Hub.slnx`。
2. 选择 **x64** 配置。
3. 构建解决方案。

项目使用以下 Qt 模块：

- Core
- GUI
- Widgets
- Network
- WebSockets

## 单元测试

单元测试位于 `unit_test/` 目录，并通过独立的控制台项目（`DSH Hub.Tests.vcxproj`）使用 Qt Test 构建。

运行测试：

1. 在 Visual Studio 中打开 `DSH Hub.slnx`。
2. 选择 **x64** 配置和 **Debug**（或 Release）。
3. 构建 `DSH Hub.Tests` 项目。
4. 运行 `x64\Debug\DSH Hub.Tests.exe`（或 `x64\Release\DSH Hub.Tests.exe`）。

当前测试覆盖范围：

- `DshEventParser` — JSON 事件解析辅助函数（`extractEventText`、`extractToolCall`、`extractApproval` 等）
- `CodeHighlighter` — 语法高亮与 HTML 转义
- `HistoryManager` — 历史加载状态管理

## 使用方法

1. 启动 DSH 本地服务端。
2. 启动 DSH Hub。
3. 应用初始化时会自动连接 DSH 服务端。
4. 从会话列表中选择或创建一个会话。
5. 输入消息，按 `Enter` 或点击发送按钮。

## 说明

- `resources/server` 目录由 DSH harness 在运行时使用。
- `node.exe` 和 `node_modules` 默认被 Git 忽略；如果运行环境需要，请确保它们可用。
- `x64/` 下的构建输出和 `.vs/` 下的 Visual Studio 缓存不会被 Git 跟踪。
