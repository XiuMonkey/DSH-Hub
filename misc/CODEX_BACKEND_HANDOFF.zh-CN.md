# 交接提示词：Codex 后端扩展的收尾与加固

> **这份文件是一份提示词**，把工作交给新的会话 / agent / 开发者。接手者看不到之前的讨论，本文自成一体。
>
> **两个工作目录，别混**：
> - 宿主仓库：`C:\Users\Playe\Documents\DSH hub\DSH Hub`（下述相对路径都相对它）
> - **客户端扩展：`C:\Users\Playe\Documents\DSH hub\CodexBackendExtension\`（不在仓库里）**
>
> 找不到这些文件就是目录不对，先确认再动手。

---

## 0. 任务目的

**让 DSH Hub 客户端用 Codex 当后端，而不是内置的 DSH 服务端。**

两半**都已经做完**，你做的是**收尾与加固**，不是从零开始：

| 半 | 状态 | 位置 |
|---|---|---|
| ① 宿主侧「后端接管机制」 | ✅ 已实现、整编通过、基线未回归 | 宿主仓库 `source/` |
| ② Codex 后端的客户端扩展 | ✅ 真客户端跑通：会话列表 → 新建会话 → 发提示 → 流式回显 → 停止 | 仓库外 `..\CodexBackendExtension\` |

未完成的集中在三件事（详 §3）：**正式客户端还没重建**、**接管分支无自动化测试**、**扩展的历史/审批/模型目录还没做**。（原第 ② 项"宿主注入槽还是私有契约"**已关闭**——12 个入站槽已提到公共虚接口 `VirtualMain`。）

---

## 1. 先读什么

| 顺序 | 文件 | 读什么 |
|---|---|---|
| 1 | `misc/CODEX_BACKEND_NOTES.zh-CN.md` | 本会话完整记录：两侧改动清单、8 个坑、未完成工作、事实速查 |
| 2 | `misc/API_TAKEOVER_PLAN.zh-CN.md` | 接管机制方案与决策（D0–D8、§2.6 接口定案、§2.8 D6/D7、§3 必写点、§5 已排除结论） |
| 3 | `misc/API_TAKEOVER_HANDOFF.zh-CN.md` | 宿主侧交接：§6 构建陷阱、§8 逐文件改动表 |
| 4 | `..\CodexBackendExtension\HOST_CONTRACT.zh-CN.md` | **宿主期望的数据形状契约表**（逐条 `文件:行号`）——写映射代码前必读 |
| 5 | `source/include/VirtualClass/VirtualApiTakeover.h` | 两个接口的全部对外契约（出站形态、错误码词汇表） |
| 6 | 扩展的 `CodexBackendPlugin.cpp` / `CodexAppServer.cpp` / `DshHostBridge.cpp` | 反直觉处都有注释写明实测原因 |

---

## 2. 现状摘要（已定案，不要重新论证）

- **两个接口 + 两个 IID（已发布，改 = 改 ABI，不要碰）**：`VirtualApiHost`（宿主 `DshApiClient` 实现：`Takenover`/`CompleteCall`/`FailCall`）、`VirtualApiSink`（扩展根对象实现：`OnOutboundRequest`）；全内联、独立头文件。
- **出站分流**：`DshApiClient` 内按 `m_takenover` 分流；一元 RPC 的分流点统一在 **`post()` 顶部、认证队列之前**。
- **回填**：只把 `rpcId` 交给扩展，回调留在宿主 `m_pending`；三种成功形态（对象/裸值/裸标量）由宿主按调用点决定。
- **入站**：走**公共虚接口** —— `findObject(kMainWindow)` → `qobject_cast<VirtualMain*>` → `HandleXxx()`（12 个方法，见 `VirtualCommon.h:27-62`，宿主内联转发在 `DSHHub.h:99-134`）。**不再有字符串 `invokeMethod`**（原方案 §2.6 的私有槽名清单已作废）。唯一需要"新增订阅"的宿主信号走 `ProtectedRegisterConnection` + 硬编码白名单。
- **D6=B**：接管态 `baseUrl()`/`launchToken()` 空、`isConnected()` 恒 true。**D7=A**：宿主错误码统一 `takenover-` 前缀。
- **三条 DSH 旁路**：工具过滤、插件市场入口**禁用**；"启动 DSH 进程"改成**接管时停掉 + 之后不启动/不重启**；**绝不动"扩展管理"**。
- **扩展映射要点**：`session/list` 回 `{"items":[…]}`（不是顶层数组，每行必须 `projections.asOfSeq>0`）；`session/page` **必须应答**（空也收遮罩）；发提示**先 `thread/resume` 再 `turn/start`**；UI 起步靠扩展自己注入 `handleConnected()`。

**已验证程度**：宿主 209/0/3（整编后）；接管分支仓库外冒烟 28/28；真机端到端走代理时回合 `durationMs=9605`。
⚠️ **测试只覆盖未接管路径** —— 改 `DshApiClient` 分支不会有测试报警。

---

## 3. 工作清单（按优先级）

**① 让扩展能装进正式客户端（最高优先）**
`x64\Release`、`x64\Debug` 里的 exe 都是 2026/9/23 旧构建、**不含接管机制**（无 `kApiClient`/`kApiSink`）⇒ 扩展装进去拿不到接口。目前只有 `build\windows-ninja\DSH Hub.exe` 可用。
- 用 MSBuild 把当前源码重建到 `x64\Release`（`.vcxproj` 清单我加过但**从未用 MSBuild 验证**）；Debug 同样。
- **插件必须与宿主同档**（Qt 会拒 `Cannot mix debug and release`），Debug 宿主用 `build.ps1 -Config Debug` 另打一份。
- 部署：`build.ps1 -Deploy "<客户端>\clientExtensions"`，或界面「扩展管理」装 `.ext`。
- 验收：`logs\extension\info.log` 出现 `takeover requested` + `session/list -> N item(s)`，任务管理器多一个 `codex.exe`。

**② 把注入槽从「私有契约」公开 —— ✅ 已完成（本项关闭）**

> **状态：12 个入站槽已全部公开，扩展侧零字符串 `invokeMethod`。**
> `MessageHost::hideLoading` 走新增公共虚接口 `VirtualMessageHost`
> （`source/include/VirtualClass/VirtualCommon.h`）+ 登记名 `DshHostIndex::kMessageHost`
> （`HostExports.h`），`MessageHost` 继承它、内联转发到私有槽（`source/include/core/MessageHost.h`）。
> 其余 12 个入站注入点走公共虚接口 **`VirtualMain`**（`VirtualCommon.h:27-62` 的 `Handle*` 方法），
> 宿主实现内联在 `DSHHub.h:99-134`（每个方法一句转发到同名私有槽），
> 扩展侧入口是 `DshHostBridge.cpp` 的 `mainHost()` → `qobject_cast<VirtualMain*>`。
> 至此**方法名即 ABI，改名编译期就报错**，"静默失效"的通道被彻底掐掉。
> 剩下那条"新增订阅宿主信号"的路走 `ProtectedRegisterConnection` + `ConnectionManager.h`
> 类内的硬编码白名单 `ConnectionManager::m_publicSignals`（现公开三条：`"2clearRequested()"`、
> `"2aboutToClose()"`（宿主关窗广播）、`"2takeoverChanged(bool)"`（接管态广播））；
> ⚠️ 白名单比对的是 **signal 签名串**，不是调用方自起的 index（搞混会让白名单整个失效，踩过一次）。
> ⚠️ 注意 `VirtualCommon.h` 里带 Widgets 的那块（`VirtualTopBar`）现在是**无条件**包含
> `<qboxlayout.h>` —— 中间试过 `#if __has_include(...)`，已按决策撤销。扩展侧因此显式链
> `Qt6::Widgets`（未被调用的部分会被链接器丢掉，导入表里不会出现 Qt6Widgets）；代价是
> 新写的"只链 Qt Core"插件会报 `C1083: 无法打开包括文件 qboxlayout.h`。

以下是**当时**的私有契约清单，仅作历史对照，**不要再照它写代码**（`misc/API_TAKEOVER_PLAN.zh-CN.md` §2.6）：`handleConnected` / `forwardMuxFrame` / `handleSessionSnapshot` / `handleSessionProjections` / `handleSessionControlBaseline` / `handleSessionProjectionChanged` / `handleWorkspaceSnapshot` / `handleWorkspaceUpserted` / `handleWorkspaceRemoved` / `handleWorkspaceReordered` / `handleWorkspaceArchiveChanged` / `handleTransportError` —— 对应现在的 `VirtualMain::HandleConnected` / `ForwardMuxFrame` / `HandleSessionSnapshot` / … （首字母大写，其余同）。

**③ 给接管分支补自动化测试**
把仓库外冒烟（假 `VirtualApiSink` + 直接链 `.obj` 的 28 项）整理成 `tests/TestDshApiTakeover.cpp`。基线变 209+N，记得同步更新文档里的数字。

**④ 扩展功能补全**（用户最可能先要历史回放）

> **状态：已做（历史回放 / 模型目录 / 审批提问 / 工具卡片四项）**。实现与验证记录见
> `..\CodexBackendExtension\MAPPING.zh-CN.md`；只有"界面真的弹出来"这一步还没人肉眼确认
> （本机网络对 codex 长连接当前不可用，详见那份文档 §7）。**设置面板**与**工作区模型**仍未做。

| 项 | 现状 | 做法 |
|---|---|---|
| 会话历史回放 | ✅ 已做 | 用 `thread/turns/list{sortDirection:"asc"}`（**不要**用 `thread/read includeTurns=true`，codex 已把它标为 deprecated）翻成 `{type:"event",event:{seq,type,data}}`（只认 `user/message`/`assistant/message`/`tool/call`/`tool/result`）→ 喂 `handleSessionSnapshot`；`session/page` 也真答了 |
| 模型目录 | ✅ 已做 | 接 codex `model/list` → `{default,groups,routableProviders,failures}`；`session/selectModel` 落到 `turn/start` 的 `model`/`effort`，并同步进 `session/list` 行的模型投影 |
| 审批/提问面板 | ✅ 已做 | codex 的 4 种 server request → `{rpcId,payload:{type:"approval/requested"…}}` / `question/requested`；`$events/result` 应答译回 codex（v2 用 accept/decline，v1 用 approved/abort） |
| 工具卡片 | ✅ 已做 | `item/started`·`item/completed` 里的命令/文件改动/MCP 调用 → `tool/call` + `tool/result` 事件 |
| 设置面板 / 工作区模型 | 仍未做 | 空壳 / "一个 cwd 一个工作区"，按需 |

**⑤ 小收尾**：`TopBar::setSessionId` 在入口已关时仍 `loadTools()`（日志噪音）；构建依赖修复（§5 坑 1）；CMake 目录不是可运行部署（缺 `resources/`，`platforms/tls/styles/translations` 是手工拷的）。

---

## 4. 环境 / 构建 / 验证

**宿主**
```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
"D:\Qt\Tools\CMake_64\bin\cmake.exe" --build "<仓库>\build\windows-ninja" --target dshhub_tests
```
⚠️ **改过头文件必须整编**（§5 坑 1）：
```powershell
Get-ChildItem source,tests -Recurse -Include *.cpp,*.h | % { $_.LastWriteTime = (Get-Date) }
```
**跑测试**（WIN32 子系统抓不到 stdout 要用报告目录；必须从仓库根跑）：
```powershell
$env:DSHHUB_TEST_REPORT_DIR="$env:TEMP\dshhub-reports"; Set-Location "<仓库>"
& "<仓库>\build\windows-ninja\DSH Hub.Tests.exe"     # 基线 209/0/3
```

**扩展**
```powershell
cd "C:\Users\Playe\Documents\DSH hub\CodexBackendExtension"
.\build.ps1 [-Config Debug] [-Deploy "<客户端>\clientExtensions"] [-Pack]
node tools\schema_peek.js <方法名> [--server]   # 查 codex 协议字段（权威）
build\appserver_smoke.exe --turn "hi"           # 无 GUI 验 app-server 协议
```

**跑起来（代理是必须的）**
```powershell
& "<仓库>\build\windows-ninja\start-codex-client.cmd"    # 双击也行
```
启动器只做一件事：设 `HTTP_PROXY`/`HTTPS_PROXY=http://127.0.0.1:7892` 再启动。
**为什么必须**：Codex 是 Rust 实现，**不读 Windows/WinINET 系统代理**，只认这几个环境变量 —— 不设每轮 5 次重试 ≈**121 秒**，设了 ≈**10 秒**（DNS 污染 + 按 SNI 重置 TLS 的实测证据见 NOTES §3）。

**排错看这里**

| 症状 | 看 |
|---|---|
| 有没有接管 | `logs\extension\info.log`（`takeover requested`、`session/list ->`） |
| codex 报错/后端退出/重试 | `logs\extension\warn.log` |
| 界面卡初始化/首屏/遮罩 | `logs\session\warn.log`（`follow cursor timeout`、`loading overlay watchdog`） |
| 扩展没装载 | 界面标签来自 `ClientExtension::loadedNames()`，对照日志 `loaded: name=… dll=main.dll` |

---

## 5. 明确不要做的事

| 不做 | 原因 |
|---|---|
| 改两个 IID / 在接口中间插虚方法 | 旧插件静默 nullptr / vtable 槽位错位 |
| 用信号做插件↔宿主通讯 | 已定：**只两条信道** —— ① 公共虚接口（`VirtualMain` / `VirtualMessageHost` / `VirtualApiHost`…，`findObject` + `qobject_cast`）；② 信号登记表（有现成连接用 `TakeoverConnection` 匿名接管，要新增订阅只走 `ProtectedRegisterConnection` + 白名单）。**字符串 `invokeMethod` 已全面废弃** |
| 把虚方法加进 `DshApiClient.h` | 该类 out-of-line，插件一碰 `LNK2019` |
| 连"扩展管理"入口一起关 | 它是客户端扩展唯一的装载通道 |
| 用 `FailCall` 打断启动链 | 拿不到数据应回"**成功 + 空 items**"，让宿主走"无会话→自动建会话" |
| 宿主侧加卸载兜底 / owner 记名 | 本期已决定不做（方案 §2.5-C、§4.2）；复位由插件 `detachHost()` 负责 |
| 复用 `handleParsedResponse` 做接管回填 | 它查 `m_parsing`，接管条目只在 `m_pending` ⇒ 静默丢回调 |
| 让接管请求排队等认证 | 接管分支必须在认证队列**之前** |
| 假设 resume 会换 id / 先 turn 再补 resume | 实测：resume 返回同一 id；顺序反了救不回来 |

---

## 6. 最容易踩的坑（本会话最耗时的）

1. **`build/windows-ninja` 记不到头部依赖** → 改头文件不重编 ⇒ 跨 TU 布局不一致（ODR）⇒ **随机 `0xC0000005`**。判据：`ninja -t deps` 里 `#deps 0`、`ninja -n` 什么都不列。解法：整编。
2. **Qt 插件必须与宿主同档** → 否则加载期被拒、界面显示"已安装但**未装载**"（`dependence\Qt6Cored.dll` = Debug 宿主的标志）。
3. **僵尸进程握文件锁** → 强杀客户端后条目残留（`HasExited=True`、1 线程、无窗口），仍锁着 DLL ⇒ 部署失败、`.ext` 装上却"未装载"。解法：`Get-CimInstance Win32_Process -Filter "ProcessId=X" | Invoke-CimMethod -MethodName Terminate`（`Stop-Process`/`taskkill` 无效），或重启。
4. **`qInfo(tag, "fmt %s", x)` 会吞正文**（Qt 的 "category + printf" 重载）→ 用 `qInfo().noquote() << tag << text`。
5. **首屏快照要避开静默丢弃** → `HistoryLoader::seedFromSnapshot` 第一句 `sessionId != m_sessionId → return`（`MessageQuery.cpp:866`）⇒ 注入要"立刻 + 300ms + 900ms"各一次（幂等）。
6. **`thread/resume` 必须在 `turn/start` 之前**。
7. **Codex 不读 Windows 系统代理**（见 §4）。
8. **编码细节**：`.ps1` 必须 UTF-8 **带 BOM**（否则 PowerShell 5.1 按 ANSI 读、中文注释解析带崩）；`.ext` 载荷必须叫 `main.dll`、zip 条目路径正斜杠。

---

## 7. 关键事实速查

| 事实 | 位置 |
|---|---|
| 两个接口与 IID | `source/include/VirtualClass/VirtualApiTakeover.h` |
| 出站分流点 | `source/src/network/DshApiClient.cpp` 的 `post()` 顶部 |
| 注册表两个 index | `source/include/core/HostExports.h`：`kApiClient` / `kApiSink` |
| 停 DSH 进程 / 接管态不重启 | `source/src/core/ServerManager.cpp`：`stopForTakeover()` + 进程级标记 |
| 接管接线与旁路开关 | `source/src/core/DSHHub.cpp`：`DSHHub.045`、`applyTakeoverBypasses()` |
| 可注入的 11 个槽 | 方案 §2.6 + 扩展 `DshHostBridge.cpp` |
| 宿主数据形状（逐字段） | `..\CodexBackendExtension\HOST_CONTRACT.zh-CN.md` |
| 扩展映射实现 | `dispatchOutbound` / `onNotification`（`CodexBackendPlugin.cpp`） |
| 部署好的扩展 | `<客户端 exe>\clientExtensions\CodexBackend\{main.dll, regulation.json5}` |
| 测试基线 | 209 passed / 0 failed / 3 skipped（**只覆盖未接管路径**） |
| 网络实测 | 直连 121 秒（5 次重试）；走代理 ≈10 秒，回合 `durationMs=9605` |

---

## 8. 第一条行动建议

**先做 §3 的 ①** —— 它一步就能把整条路从"只能跑我的测试构建目录"变成"用户平时那个客户端直接可用"：

1. 用 MSBuild 重建 `x64\Release`（当前源码，含接管机制），顺手确认 `.vcxproj` 里加过的文件清单没问题。
2. `cd ..\CodexBackendExtension; .\build.ps1 -Deploy "<仓库>\x64\Release\clientExtensions"`。
3. 启动 `x64\Release\DSH Hub.exe`（记得带代理环境变量），按 §4 的日志判据验收接管生效。
4. 接着做 §3 的 ③（给接管分支补自动化测试）—— ② 已关闭，不用再动。
