# DSH Hub

DSH Hub 是 **DSH（DeepSeek Harness）** 的 Windows 桌面客户端，使用 **Qt 6 / C++** 构建。

它通过 HTTP 和 WebSocket API 连接本地 DSH 服务端，提供聊天式界面，并支持渲染 Markdown、代码块、表格、流式响应、工具调用以及原生 DLL 扩展结果。

## 功能特性

- 🖥️ Qt 6 桌面界面（Windows）
- 🔌 通过 HTTP + WebSocket 连接本地 DSH 服务端
- 💬 聊天式消息界面
- 📝 支持 Markdown 渲染，包括代码块和表格
- 🎨 基于 `highlight_rules.json` 的代码语法高亮
- 📜 会话/工作区侧边栏，支持右键删除会话
- ⚡ 流式 Agent 输出，带节流 UI 更新
- ⌨️ 自动增长聊天输入框
  - `Enter` 发送消息
  - `Shift+Enter` 插入换行
- ⚙️ 设置弹窗与主题切换
- 🧩 插件市场与扩展管理
- 🧬 原生 DLL 扩展桥接
  - 支持 JSON 风格与 native 风格调用约定
  - 支持 `LoadingSource`，同一个扩展可以从多个 DLL 导出工具
- 🎞️ FFmpeg 示例扩展
  - `ffmpeg_probe`
  - `ffmpeg_convert`
  - `ffmpeg_extract_frame`
  - `ffmpeg_extract_audio`
  - `ffmpeg_compress_video`
  - `ffmpeg_video_to_gif`
- 🔄 初始化与消息加载时的加载指示器

## 项目结构

```text
DSH Hub/
├── include/                    # C++ 头文件
├── src/
│   ├── core/                   # 主窗口、服务端管理、程序入口
│   ├── ui/                     # 侧边栏、聊天输入框、弹窗、插件/扩展管理
│   ├── chat/                   # 消息列表/单元、历史加载与缓存
│   ├── network/                # HTTP/WebSocket 客户端、事件解析、会话预取
│   ├── bridge/                 # 命名管道桥接、DLL JSON 调用器、扩展加载器
│   └── common/                 # 日志、主题、代码高亮、交互弹窗
├── resources/
│   ├── 图标/图片与 QRC
│   ├── highlight_rules.json    # 代码高亮规则
│   ├── registry-snapshot.json  # 插件市场离线快照
│   └── server/
│       └── launch-root/
│           └── FFmpegExt/      # FFmpeg 示例扩展包
├── unit_test/                  # Qt Test 单元测试
├── test extension/             # 本地扩展测试目录
├── DSH Hub.slnx
├── DSH Hub.vcxproj
├── DSH Hub.Tests.vcxproj
└── README.zh-CN.md
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
4. 运行生成的 `DSH Hub.Tests.exe`。

当前测试覆盖范围：

- `DshEventParser` — JSON 事件解析辅助函数（`extractEventText`、`extractToolCall`、`extractApproval` 等）
- `CodeHighlighter` — 语法高亮与 HTML 转义
- `HistoryManager` — 历史加载状态管理

## 扩展格式

扩展以 `.ext` 文件（ZIP 格式）打包，目录结构如下：

```text
Extension.ext
├── main.dll              # 主原生 DLL
├── regulation.json5      # DLL 工具描述文件
├── AttachedPlugin/       # DSH 插件，负责注册工具
│   ├── package.json
│   └── index.js
└── bin/                  # 可选运行时依赖（例如 FFmpeg DLL）
```

`regulation.json5` 支持在每个函数上添加 `LoadingSource` 字段，从而让同一个扩展从不同 DLL 导出工具：

```json5
{
  "Func": "my_function",
  "LoadingSource": "main.dll",
  "Tool": "my_tool"
}
```

## 使用方法

1. 启动 DSH 本地服务端。
2. 启动 DSH Hub。
3. 应用初始化时会自动连接 DSH 服务端。
4. 从会话列表中选择或创建一个会话。
5. 输入消息，按 `Enter` 或点击发送按钮。