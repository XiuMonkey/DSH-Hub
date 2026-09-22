# DSH Hub

DSH Hub 是 **DSH（DeepSeek Harness）** 的 Windows 桌面客户端，使用 **Qt 6 / C++** 构建。

它通过 HTTP 和 WebSocket API 连接本地 DSH 服务端，提供聊天式界面，并支持渲染 Markdown、代码块、表格、流式响应、工具调用以及原生 DLL 扩展结果。

## 功能特性

- 🖥️ Qt 6 桌面界面（Windows），无边框 + 自绘标题栏，所有弹窗共用一层遮罩
- 🔌 通过 HTTP + WebSocket 连接本地 DSH 服务端（mux + host 两条流）
- 💬 聊天式消息界面：流式输出、节流更新、加载更多
- 📝 Markdown 渲染：代码块、表格，以及基于 `highlight_rules.json` 的语法高亮
- 📜 会话/工作区侧边栏：分组、归档/删除、跨会话历史缓存
- ⌨️ 自动增长聊天输入框（`Enter` 发送，`Shift+Enter` 换行）
- ⚙️ 设置弹窗、主题切换（浅/深）、样式整包可换（QSS + 调色板，支持壁纸，示例见 `assets/StyleExample/`）
- 🌐 ID-based 国际化，内置 `zh_CN` / `en` 语言包
- 🧩 插件市场与扩展管理
- 🧬 **工具扩展** —— 由 DSH 服务端调用的原生 DLL 桥接
  - 支持 JSON 风格与 native 风格调用约定、`LoadingSource` 多 DLL、COM 接口
- 🪟 **客户端扩展** —— 装进**本客户端进程**的 Qt 插件（QPlugin）DLL
  - 跑在 GUI 线程，直接操作宿主界面（例如往顶栏布局里加控件）
  - 同样用 `.ext` 交付，靠 `regulation.json5` 里的 `Type` 字段分流
- 🔗 **宿主 C 导出 + 全局对象注册表**
  - 宿主 exe 导出一小组 C ABI，插件运行期取宿主对象，**链接期零依赖**
  - 程序级唯一对象（主窗口 / 侧边栏 / 顶栏）以字符串索引登记在注册表里
- 🔄 初始化与消息加载时的加载指示器

## 项目结构

```text
DSH Hub/                              ← 仓库根：只放 .gitignore / .gitattributes
├── source/                           # 源代码 + 构建系统 + 解决方案
│   ├── include/                      # 头文件，按 src/ 镜像分组（见下）
│   ├── src/
│   │   ├── core/                     # 主窗口、MessageHost、服务端管理、程序入口、宿主 C 导出
│   │   ├── ui/                       # 仅界面控制与绘制：侧边栏、聊天输入框、弹窗、设置
│   │   ├── chat/                     # 消息单元、查询/缓存、代码块视图
│   │   ├── network/                  # HTTP/WebSocket 客户端、事件解析、会话预取
│   │   ├── ExtensionSystem/          # 命名管道桥接、DLL JSON 调用器、Thunk 生成器、
│   │   │                             # 扩展加载器、客户端扩展装载
│   │   └── common/                   # 与界面无关的逻辑，分五组：
│   │       ├── util/                 #   日志、全局注册表、代码高亮、Markdown 预处理
│   │       ├── settings/             #   设置存储、客户端设置
│   │       ├── session/              #   会话目录/服务、Agent 预设、模型选择
│   │       ├── extension/            #   扩展注册表、安装任务、插件市场客户端与安装器
│   │       └── appearance/           #   主题、窗口框架、卡片阴影、翻译、交互
│   ├── CMake/                        # CMake 构建（命令行构建的事实来源）
│   ├── DSH Hub.slnx
│   ├── DSH Hub.vcxproj（+ .filters）  # 主目标
│   └── .vs/                          # VS 缓存（IDE 自动生成）
├── tests/                            # 单元测试（Qt Test）+ DSH Hub.Tests.vcxproj
├── assets/                           # 运行期资源与被主题引用的素材
│   ├── resources/
│   │   ├── styles/                   # 内置默认样式套：12 个 QSS 模块 + 2 个调色板模板（不含壁纸）
│   │   ├── translations/             # 内置 .qm 语言包（qrc 保底）
│   │   ├── ToolsFilterPlugin/        # 工具过滤用的 cordis 插件（服务端侧）
│   │   ├── EXTENSION_DEV.zh-CN.md    # 工具扩展开发文档
│   │   ├── highlight_rules.json      # 代码高亮规则
│   │   └── server/                   # 内置 DSH 服务端（harness profile、launch-root）
│   ├── translations/                 # .ts 译文源（lrelease 的输入）
│   └── StyleExample/                 # 样式整包示例（QSS + 调色板 + 壁纸），可整包替换 <exe>/styles/
└── misc/                             # 辅助：脚本与文档，不参与构建
    ├── tools/                        # 构建 / 国际化 / 会话日志辅助脚本
    ├── ReleaseBuild/                 # 打包产物（由 build-release.sh 生成）
    ├── README.md  README.zh-CN.md
    ├── DESIGN_NOTES.zh-CN.md         # 注释瘦身时搬出来的「为什么」
    └── Count-Lines.ps1
```

`build/`、`x64/`、`DSH Hub/`、`DSH Hub.Tests/` 是生成物（见 `.gitignore`）：`build/` 是 CMake
构建目录，`x64/` 是 VS 的输出与中间目录，另外两个是重组前的 VS 中间目录残留、可以删。

`include/` 与 `src/` 镜像对应（另加 `VirtualClass/`，放跨 DLL 的插件接口），并且它是**扁平的
include 根**：所有 `#include "..."` 都按 `include/<分组>/<头文件>.h` 解析。

> ⚠️ 两个工程的 `OutDir` 刻意用 `$(SolutionDir)..\x64\` **钉回仓库根**：`.slnx` 住在 `source/`
> 里，不钉的话 VS 的输出会跟着搬进 `source\x64\`，而 `misc/tools/` 下的脚本与既有习惯都按
> `<repo>\x64\<Config>\` 找 exe。改这两个工程文件时别删掉那段 `OutputPaths`。

### 运行库收在 `dependence\`，不跟 exe 平铺

Qt 与 MSVC 的 11 个运行库**不在 exe 旁边**，而在 `<exe 目录>\dependence\` 里：

```text
x64\Release\
├── DSH Hub.exe
├── dependence\
│   ├── dependence.manifest      # 私有程序集清单：逐个列出 11 个 dll
│   ├── Qt6Core.dll  Qt6Gui.dll  Qt6Widgets.dll  Qt6Network.dll  Qt6WebSockets.dll
│   └── concrt140.dll  msvcp140*.dll  vcruntime140*.dll
├── platforms\  styles\  translations\  resources\  ...
└── logs\  ClientSetting\  tls\  clientExtensions\
```

**原理**：Windows 的加载器**默认不搜索 exe 的子目录**（顺序是 exe 目录 → 系统目录 →
`AddDllDirectory` 加进去的 → 当前目录 → `PATH`），所以"把 dll 挪进去"单靠移动文件不成立。
真正让它成立的是**私有程序集**：

- `source/dependence.manifest`（部署到 `dependence/`）声明程序集 `dependence` 并逐条列出 dll；
- `source/dependence.deps.manifest` 被合并进 exe 自身的 manifest，声明对它的依赖
  （vcxproj 走 `<AdditionalManifestFiles>`，CMake 走 `/MANIFESTINPUT`）。

⚠️ **两条维护约定**：

1. **加/换 DLL 必须同步改 `source/dependence.manifest` 的 `<file>` 列表**，并让它与
   `dependence.deps.manifest` 里的 `version` 保持一致 —— 漏一条就是启动即失败。
   目录名必须**恰好**等于程序集名 `dependence`。
2. 部署到新目录后跑一次 `misc/tools/make-dependence.ps1 -Target <目录>`：DLL 名单**从清单里读**
   （清单即事实来源），把 dll 收进 `dependence/` 并把清单本身也拷过去。漏了哪个它会告警。

## 环境要求

- Windows 10 或更高版本
- Visual Studio 2022 或兼容的 MSVC 工具集
- Qt 6.11.2 MSVC 2022 x64（或其他兼容的 Qt 6 版本）
- 本地运行 DSH 服务端

## 构建

**Visual Studio：** 打开 `source/DSH Hub.slnx`，选择 **x64**，构建。

**CMake + Ninja（命令行）：**

```bash
# Windows 下的 bash；脚本自己导出 MSVC 环境后再调 cmake
"<PortableGit>/bin/bash.exe" misc/tools/rebuild.sh            # 主目标
"<PortableGit>/bin/bash.exe" misc/tools/rebuild.sh dshhub_tests
```

`misc/tools/rebuild.sh` 会先关掉正在运行的实例（Windows 会锁住 exe），首次运行自动 configure
`build/windows-ninja`，多余的参数原样转给 `cmake`。

项目使用以下 Qt 模块：

- Core
- GUI
- Widgets
- Network
- WebSockets

## 单元测试

测试位于 `tests/`，通过独立控制台目标（`DSH Hub.Tests`）构建。当前基线：
**208 passed / 0 failed / 3 skipped**，共 16 个测试类。

| 测试 | 覆盖 |
|---|---|
| `TestDshEventParser` | mux 事件解析辅助（文本、工具调用、审批） |
| `TestDshApiClient` | RPC 调用链路、URL/token 组装 |
| `TestServerManager` | 服务端启动/重启与就绪判定 |
| `TestSessionCatalog` | 会话/工作区解析：子代理过滤、标题回退、归档过滤、工作区归属、自动选中 |
| `TestSessionPrefetcher` | 会话预取目标 |
| `TestHistoryManager` | 历史加载状态与缓存 |
| `TestMarkdownPreprocess` | `<br>` 替换与表格渲染（Qt 6 回归） |
| `TestCodeHighlighter` | 语法高亮与 HTML 转义 |
| `TestThunk` | x86-64 Thunk 生成器：调用约定、参数类型、签名 JSON、端到端 DLL 调用 |
| `TestPluginMarketModel` | 市场条目解析、关键词过滤、分页切片 |
| `TestModelSelection` | 模型 / 思考深度的选中决策 |
| `TestSettingsStore` | `settings/update` 合并与持久化 |
| `TestClientSettings` | `ClientSetting/*.json` 读写 |
| `TestTranslationManager` | ID-based 翻译查表与回退 |
| `TestLogger` | 日志分级与按模块落盘 |

> `TestThunk` 里有用例依赖示例扩展包目录，而该目录不在仓库里。缺它时这些用例报告为
> **skipped** 而不是失败（基线里的 3 个 skip 就来自这里）。

## 扩展

`.ext` 是 ZIP 包。**两条互不相干的路线共用这个容器**，靠 `regulation.json5` 里的
`Type` 字段区分：

### 1. 工具扩展（无 `Type`，带 `Function[]`）

装进 DSH **服务端** profile，由 `DllCaller` 在 Worker 线程按 JSON 文本协议调用。包结构、
描述文件全字段（json / native / com 三种风格、`AttachedPlugin`、安装/卸载行为、线程模型、
常见问题）：

➡️ **[`assets/resources/EXTENSION_DEV.zh-CN.md`](assets/resources/EXTENSION_DEV.zh-CN.md)**

```text
MyTool.ext
├── main.dll
├── regulation.json5          # Function[] 描述
├── AttachedPlugin/           # 注册工具的 DSH 插件
│   ├── package.json
│   └── index.js
└── bin/                      # 可选运行时依赖
```

### 2. 客户端扩展（`Type: ClientExtension` / `ClientExtensionDebug`）

装进 **DSH Hub 进程**、在 GUI 线程运行的 Qt 插件 DLL。它通过宿主的 C 导出拿到宿主对象，
可以直接改宿主界面。

```text
MyClientExt.ext
├── regulation.json5          # { "Name": ..., "Type": "ClientExtensionDebug" }
└── my_ext.dll                # QPlugin（Q_PLUGIN_METADATA）
```

- 装到 `<exe>/clientExtensions/<Name>/` —— 那里的 `regulation.json5` 就是"已安装"的判据，
  装载、列表、移除三处都按它认。
- 宿主主窗口实现了 `VirtualClass/VirtualCommon.h` 里的 **`VirtualWindow`** 接口：扩展把注册表里
  取到的 `mainWindow` 转成 `VirtualWindow*`，就能把自己的顶层窗口当成宿主弹窗
  （`ExternalShowOverlay()` / `ExternalHideOverlay()`：遮罩 + 居中显示，与设置/插件市场走同一条
  路径）。转换走 `qt_metacast(IID)`、调用走 vtable，插件侧零宿主符号。另有两个可选槽/接口：
  `detachHost()` 见 `source/include/core/DshHostPlugin.h`，`VirtualTheme` 见同一份 VirtualCommon.h。
- **安装是整包落地**：`.ext` 里除 `main.dll` / `regulation.json5` 之外的文件与子目录也会一起
  拷进 `<exe>/clientExtensions/<Name>/` —— 扩展可以带自己的资源（例如它自己的 `styles/`）。
- 移除有两条路径：
  - 插件实现了可选的 `detachHost()` 槽（见 `source/include/core/DshHostPlugin.h`）→ 宿主先让它
    同步拆掉自己挂在界面上的东西，再真正 `unload()`：dll 从进程里解映射，**扩展目录当场
    删干净**；
  - 没实现（老插件）→ 删掉 `regulation.json5` 判据（此刻起就不算已安装、下次启动也不会
    装载），而被本进程映射着的 dll 当次删不掉，于是留一个 `.pending-removal` 标记，由
    **下次启动**的清扫（`loadAll()` 开头）彻底删掉。
  两条路径都不会留下"看着像已安装"的残骸。注：Windows 上改名/移动目录也躲不开这个文件锁，
  所以没有 `detachHost()` 时兜底只能是"下次启动再清"。
- `Type` 大小写不敏感；其它值一律走工具扩展那条路。
- 插件必须与宿主**同一档构建**（Debug/Release、同一个 Qt 二进制、同一套 MSVC 工具链）——
  Qt 在加载期就会拒绝混用。
- 开发指引与可运行示例（往顶栏加按钮）：

➡️ **[`../QPluginExtension/README.md`](../QPluginExtension/README.md)**

### 宿主 C 导出与全局对象注册表

宿主 exe 导出两个 `extern "C"` 符号（`DshHubHostAbiVersion`、`DshHubHostRegistryFind`）。
插件在运行期用 `QLibrary::resolve` 取地址，所以插件 DLL 的**导入表里没有任何宿主符号**，
也不需要 `dllimport` / 导入库。

已登记的索引（每个进程一份）：`mainWindow`、`sidebar`、`topbar`、`themeManager`。查询必须在
GUI 线程调用；详见 `source/include/core/HostExports.h`。其中 `themeManager` 是样式表管理单例
（`ThemeManager`，实现接口 `VirtualTheme`）—— 插件转成 `VirtualTheme*` 后可以：

- `ExternalApplyToWindow(窗口)`：把当前主题的样式表挂到自己的窗口上；
- `ExternalReloadStyles()`：让宿主重新读一遍本地样式文件（`<exe>/styles/` 下的 `*.qss` 与
  `theme-*.json`，外部优先、qrc 兜底）并重挂到**所有**窗口。**插件自己覆盖完那些文件后调它**
  就能让整个界面立刻用上新样式；返回 `false` 表示重载后样式表为空（文件没读到：写失败、
  路径不对或权限不足），可直接当自检用。注意它不重建窗口，构造期就固化的东西（如按主题选的
  logo 资源）不会跟着变。

`VirtualTheme` 的虚方法**只允许在末尾追加**（宿主 vtable 槽位 = 声明顺序，插件 DLL 独立编译）。

## 使用方法

1. 启动 DSH 本地服务端。
2. 启动 DSH Hub。
3. 应用初始化时会自动连接 DSH 服务端。
4. 从侧边栏选择或创建一个会话。
5. 输入消息，按 `Enter` 或点击发送按钮。
