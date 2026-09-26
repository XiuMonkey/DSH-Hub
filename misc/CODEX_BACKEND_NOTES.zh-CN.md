# Codex 后端：本会话改动、踩坑与未完成工作（记录）

> **这份文件是什么**：本会话（"让 DSH Hub 用 Codex 当后端"）的改动清单、踩坑记录、以及**还没做完的事**。给下一位接手者/下一轮用；**要交给新会话的提示词见同目录的 `CODEX_BACKEND_HANDOFF.zh-CN.md`**。
>
> **先读这三份，再动代码**：
> - `misc/API_TAKEOVER_PLAN.zh-CN.md` —— 宿主侧接管机制的完整方案与决策记录（D0–D8、§3 必写点、§5 已排除结论）
> - `misc/API_TAKEOVER_HANDOFF.zh-CN.md` —— 宿主侧交接（§6 有构建陷阱，§8 有逐文件改动表）
> - 仓库外 `..\CodexBackendExtension\HOST_CONTRACT.zh-CN.md` —— **宿主期望的数据形状契约表**（逐条带 `文件:行号`，写映射代码前必读）
>
> 路径基准：宿主仓库根 `C:\Users\Playe\Documents\DSH hub\DSH Hub`；扩展在并列目录 `..\CodexBackendExtension\`（不在仓库里）。

---

## 0. 一句话现状

| 半 | 状态 |
|---|---|
| ① 宿主侧「后端接管机制」 | ✅ 已实现、整编通过、基线 209/0/3 未回归；仓库外冒烟 28/28 全过 |
| ② 扩展侧「Codex 后端」（仓库外） | ✅ 真客户端里跑通：装载 → 接管 → 会话列表（= codex threads）→ 新建会话 → 发提示 → 真 Codex 回合 → 流式回显 → 停止；`.ext` 已打包校验 |
| 拦路虎（已解决） | 本机 **Codex 不走 Windows 系统代理**，导致每轮重试 121 秒；设 `HTTP(S)_PROXY` 后 **≈10 秒**（实测回合 `durationMs=9605`） |

**没做完的不在"能不能接管"，而在**：接管分支没有自动化测试、两个正式客户端（Release/Debug）还没用新源码重建、单一 owner 校验缺失。（注入槽**已**从私有契约提到公共虚接口 `VirtualMain`，见 §8 追记——原第 ② 项已关闭。）

---

## 1. 宿主侧改动（DSH Hub 仓库，20 改 1 增）

| 文件 | 改了什么 |
|---|---|
| `source/include/VirtualClass/VirtualApiTakeover.h`（**新增**） | 两个公共纯虚接口 + 两个 IID：`VirtualApiHost`（宿主实现：`Takenover`/`CompleteCall`/`FailCall`）、`VirtualApiSink`（插件实现：`OnOutboundRequest`）。文件头写明出站下发形态、`$takeover/stream-open|cancel`、D7 错误码词汇表 |
| `source/include/network/DshApiClient.h` | 多继承 `VirtualApiHost` + `Q_INTERFACES`；D6 读接口语义；`takeoverChanged` 信号；`PendingCall` 加 `takeover`/`takeoverDeadlineMs`；接管辅助方法声明；`m_takenover`/`m_takeoverSweep` |
| `source/src/network/DshApiClient.cpp` | 8 个出站方法分流（一元 RPC 分流点在 `post()` 顶部、**认证队列之前**）；`Takenover`/`CompleteCall`/`FailCall`；120s 超时扫描；进/出接管态收尾；`respond` 绕过 `m_clientId` 门槛；`followSession`/`unfollowSession` 改通知扩展 |
| `source/include/core/HostExports.h` | 新增 `kApiClient`（插件取宿主接口）、`kApiSink`（宿主取插件接收端）两个 index |
| `source/include/core/ServerManager.h` + `.cpp` | `stopForTakeover()`（幂等 kill）；**进程级**接管标记 `setTakenover/isTakenover`；接管态下 `start()`/`restart()` no-op（`start()` 仍先填 `dshHome`） |
| `source/src/core/DSHHub.cpp` + `include/core/DSHHub.h` | 登记/注销 `kApiClient`；`DSHHub.045` 接管接线（停进程 + 切旁路）；`DSHHub.026` 拦市场入口；`applyTakeoverBypasses()` |
| `source/src/ExtensionSystem/ClientExtension.cpp` | 装载时 `qobject_cast<VirtualApiSink*>(root)` → 登记 `kApiSink`；拿不到只记日志（**不拒绝装载**，否则既有扩展全废），真正的拒绝在 `Takenover(true)` |
| `source/include/core/DshHostPlugin.h` | `detachHost()` 契约补"接管了后端必须在这里复位" |
| `source/include/ui/TopBar.h/.cpp`、`source/include/ui/Sidebar.h/.cpp` | 两个入口开关（工具过滤 / 插件市场）；**不动扩展管理** |
| `source/CMake/CMakeLists.txt` + 两个 `.vcxproj(.filters)` | 测试目标补 `CommonRegistry.cpp/.h`；新接口头进工程 |
| `misc/API_TAKEOVER_PLAN.zh-CN.md`、`misc/API_TAKEOVER_HANDOFF.zh-CN.md` | D6/D7 定案回填（§2.8）、构建陷阱（§6.6）、宿主侧实现状态表（§8） |

验证：`dshhub_tests` 整编后 **209 passed / 0 failed / 3 skipped**；`dshhub` 主目标编译链接通过；另有一个**仓库外**冒烟程序（假扩展接收端 + 直接链宿主 `.obj`）**28/28** 全过（含拒绝接管、D6、下发/回填三形态、`respond` 绕过、还台复位、幂等）。

---

## 2. 扩展侧改动（`..\CodexBackendExtension\`，不在仓库）

| 文件 | 作用 |
|---|---|
| `CodexAppServer.h/.cpp` | `codex app-server` 的行分隔 JSON-RPC 客户端（QProcess；请求/通知/服务端请求三分流；逐请求超时）。**`params` 即使为空也必须带**，否则服务端回 `-32600` |
| `CodexBackendPlugin.h/.cpp` | 插件根对象：`DshHostPlugin` + `VirtualApiSink` 双接口、接管、出站分发（session/list·page·create·prompt·cancel·modelCatalog·settings·workspace…）、入站映射、`detachHost` 复位、**后端自动重启** |
| `DshHostBridge.h/.cpp` | 入站注入 → `qobject_cast<VirtualMain*>` 直调 `HandleXxx()`（**已无字符串 `invokeMethod`**）；宿主信号订阅只走 `protectedConnection()`（白名单，目前 `clearRequested`）；`hideSessionLoading()` 走 `VirtualMessageHost` |
| `regulation.json5` | `Name: CodexBackend`、`Type: ClientExtension` |
| `CMakeLists.txt`、`build.ps1`、`pack-ext.ps1` | 构建/部署/打包（`.ext` = `regulation.json5` + `main.dll`，条目名正斜杠）。⚠️ 两个 `.ps1` 必须保留 **UTF-8 BOM** |
| `HOST_CONTRACT.zh-CN.md` | 宿主期望形状契约表（22KB，逐条 `文件:行号`） |
| `tools/probe.js`、`tools/schema_peek.js`、`tools/appserver_smoke.cpp` | 开发工具：协议探针、查 schema 字段形状、无 GUI 冒烟（都不进 `.ext`） |

产物：`CodexBackend-Release.ext`（71,929 字节，SHA256 `86A0D94A…43AB`，载荷与构建产物逐字节一致）。

映射要点（都是实测/契约表得来，别凭记忆改）：
- `session/list` → `thread/list`，回 **`{"items":[…]}`**（不是顶层数组）；每行 `sessionId`、`running`、**`projections.asOfSeq>0`**、`projections.values.title`、`modelSelection.next{provider,model}`
- `session/page` → 回空 `{records:[],hasMore:false}`（**必须应答**，否则初始化遮罩永久盖住界面）
- `session/prompt` → **先 `thread/resume`（若本进程未加载过该 thread）再 `turn/start`**；回执回 `{}`（宿主不读）
- `session/cancel` → `turn/interrupt{threadId,turnId}`
- 入站：`item/agentMessage/delta` → `assistant/chunk{chunk:{type:"text-delta",text}}`；`item/completed(agentMessage)` → `assistant/message`（**回合收尾**）；`willRetry=true` 的 `error` 每轮只提示一次
- 让 UI 起步的是扩展自己注入的 `handleConnected()`（接管态下宿主不会自己发第一次出站）

---

## 3. 本会话踩过的坑（按"最费时间"排序，别再踩）

| # | 坑 | 现象 / 判据 / 解法 |
|---|---|---|
| 1 | **`build/windows-ninja` 记不到头部依赖** | Ninja 规则是 `deps = msvc`，本机 MSVC 输出中文前缀 `注意: 包含文件:`，Ninja 只认英文 ⇒ 每个 `.obj` 依赖数 0、改头文件不重编 ⇒ **跨 TU 布局不一致（ODR）→ 随机 0xC0000005**。判据：`ninja -t deps` 里 `#deps 0`；`ninja -n` 在改头文件后什么都不列。解法：改头文件后整编（`Get-ChildItem source,tests -Recurse -Include *.cpp,*.h \| % { $_.LastWriteTime = Get-Date }`） |
| 2 | **Qt 插件必须与宿主同档** | Debug 宿主 + Release 插件 = 加载期被拒（`Cannot mix debug and release libraries`）⇒ 扩展"已安装但**未装载**"。`x64\Debug\dependence\Qt6Cored.dll` 就是 Debug 宿主的标志 |
| 3 | **僵尸进程握文件锁** | 客户端被强杀后进程条目残留（`HasExited=True`、1 线程、无窗口），仍锁着映射过的 DLL ⇒ 部署拷贝失败、`.ext` 装上却"未装载"。解法：`Get-CimInstance Win32_Process … Invoke-CimMethod Terminate`（`Stop-Process`/`taskkill` 对这类无效），或重启 |
| 4 | **日志写法会吞正文** | `qInfo(kLogTag, "fmt %s", x)` 是 Qt 的 "category + printf" 重载 ⇒ tag 当 category、正文丢失，日志里只剩 `[QPlugin] CodexBackend:` 空行。改用 `qInfo().noquote() << tag << text`（扩展里已封装 `logInfo/logWarn`） |
| 5 | **首屏要"follow 快照"，且要注意顺序** | 宿主选中会话后等 follow 游标，等不到走看门狗回落路径**收不掉初始化遮罩**；而 `HistoryLoader::seedFromSnapshot` 第一句是 `sessionId != m_sessionId → return`（`MessageQuery.cpp:866`，**静默丢弃**）⇒ 注入要"立刻 + 300ms + 900ms"各一次（幂等） |
| 6 | **`thread/resume` 必须在 `turn/start` 之前** | 反了（先 turn/start 撞 `-32600 thread not found`、再 resume 重试）**救不回来**；正确顺序实测有 delta 流式返回。另外 `thread/resume` 只要求 `threadId`、返回**同一个** thread id |
| 7 | **Codex 不读 Windows 系统代理** | 浏览器正常、Codex 每轮 `Reconnecting 2/5…5/5` ≈121 秒。Codex 是 Rust 实现，只认 `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY`。设上后 ≈10 秒 |
| 8 | **`regulation.json5`/`.ps1` 的编码** | 无 BOM 的 UTF-8 `.ps1` 会被 PowerShell 5.1 按 ANSI 读，中文注释把脚本解析带崩；`.ext` 载荷**必须叫 `main.dll`**、zip 条目路径必须是正斜杠 |

---

## 4. ⚠️ 宿主侧还没做完的工作

| 项 | 为什么重要 | 建议做法 |
|---|---|---|
| **① 正式客户端还没重建** | `x64\Release`（2026/9/23）与 `x64\Debug` 都是**旧构建、没有接管机制**（无 `kApiClient`/`kApiSink`）⇒ 扩展装进去只会被当普通扩展装载、拿不到接口。当前只能跑 `build\windows-ninja\DSH Hub.exe` | 用 MSBuild（或 CMake）把当前源码重建到 `x64\Release` / `x64\Debug`；注意 `.vcxproj` 侧我加过文件清单但**从未用 MSBuild 构建验证过** |
| ~~**② 注入槽仍是私有契约**~~ **已关闭** | 12 个入站槽已全部提到公共虚接口 `VirtualMain`（`VirtualCommon.h:27-62`），宿主内联转发写在自己的头文件（`DSHHub.h:99-134`），扩展侧**零** `invokeMethod`；唯一需要"新增订阅"的 `Sidebar::clearRequested` 走 `ProtectedRegisterConnection` + 硬编码白名单 | 已实现。⚠️ 注意白名单比对的是 **signal 签名串**，不是调用方自起的 `index`——两者搞混会让白名单整个失效（踩过，2026-09-26 修） |
| **③ 接管分支零自动化测试** | 209 个用例**全部只走未接管路径**（方案 §4.1 明确暂缓）；我改的分支只靠仓库外冒烟 + 手工验证 | 把那份仓库外冒烟（假 `VirtualApiSink` + 直接链 `.obj`）整理成 `tests/TestDshApiTakeover.cpp` 纳进测试目标；基线会变成 209+N |
| **④ 单一 owner / 独占校验（部分补上）** | `Takenover` 返回 `void`（D0=A，已发布接口不能改签名），但已在 `VirtualApiHost` **末尾**追加 `virtual bool IsTakenover() const` 回执（`DshApiClient.h` 内联转发到实成员 `isTakenover()`），扩展 `Takenover(true)` 后能自查成败；`DshApiClient::takeoverChanged(bool)` 也进了白名单，扩展订阅它能察觉"被别的扩展顶掉"。**owner 记名 / 拒绝第二家**仍未做（方案 §4.2） | 完整独占仍需 owner 记名 + "该 owner 是否仍装载"回落 |
| **⑤ 宿主不加卸载兜底**（已决定接受） | 插件实现了 `detachHost()` 却忘了复位 ⇒ 标志卡 true、所有 RPC 无响应、界面永久加载中 | 按决定不加。若日后要加，位置不能照 `UiStage::releaseForOwner`（它在成功路径之后），必须覆盖全部退出路径 |
| **⑥ 工具过滤仍会在切会话时偷偷拉取** | 接管态下入口已隐藏，但 `TopBar::setSessionId` 仍会触发 `loadTools()` ⇒ 日志里 `[ToolsFilter] 读取工具目录失败 … 服务端地址还没就绪`（无害但吵） | 在"入口被关闭"时跳过这次拉取（`TopBar` 内加一个早退即可） |
| **⑦ 构建系统的依赖修复（可选）** | 坑 #1 的根因在构建配置/环境，现在是"靠人记得整编" | 让 Ninja 认本地化前缀（如注入 `msvc_deps_prefix = 注意: 包含文件:`），或固定英文诊断（`VSLANG=1033` 实测**无效**） |
| **⑧ CMake 构建目录不是可运行部署** | `build\windows-ninja` 缺 `resources/`；`platforms/`、`tls/`、`styles/`、`translations/` 是我手工从 `x64\Release` 拷进去的（不在仓库里，重建目录就没了） | CMakeLists 里那条"运行时资源尚未纳入构建"的注释就是这件事；要么补 `install()` 规则，要么继续用 MSBuild 产物做联调 |

**优先级建议**：①（让扩展能装进正式客户端）> ③（回归保护）> ⑥ > ④/⑤/⑦/⑧。（② 已关闭。）

---

## 5. 扩展侧还没做完的工作

> **追记（后续会话）**：下表前四项**已实现**（历史回放、模型目录、审批/提问面板、工具卡片）——
> 实现与验证记录见 `..\CodexBackendExtension\MAPPING.zh-CN.md`，以及本文件 §8。
> 未做的只剩：设置面板、工作区模型、错误码细化。

| 项 | 现状 | 说明 |
|---|---|---|
| **会话历史回放** | ~~`session/page` 回空~~ → ✅ 已做 | 用 `thread/turns/list`（**不是** `thread/read includeTurns=true`，后者已被 codex 标为 deprecated）翻成 DSH 事件喂 `handleSessionSnapshot`；`session/page` 也按 `throughSeq`/`beforeSeq` 真发一页 |
| **模型目录** | ~~回空壳~~ → ✅ 已做 | 接 codex 的 `model/list`，翻成 `{default,groups,routableProviders,failures}`；`session/selectModel` 的选择落到 `turn/start` 的 `model`/`effort` |
| **审批 / 提问面板** | ~~自动拒绝~~ → ✅ 已做 | `item/commandExecution/requestApproval`、`item/fileChange/requestApproval`、`item/permissions/requestApproval`、`item/tool/requestUserInput` → 宿主面板帧；`$events/result` 译回 codex 的 `decision`/`answers` |
| **工具调用卡片** | ~~未做~~ → ✅ 已做 | `item/started`/`item/completed` 里的命令、文件改动、MCP 调用翻成 `tool/call`/`tool/result` 事件 |
| **设置面板** | `settings/describe`、`llm/*`、`credentials/*` 回空壳 | 设置窗口打开是空的；按需接或明确禁用入口 |
| **工作区模型** | 现在"一个 cwd = 一个工作区" | 可按 codex 的 project/section 概念细化 |
| **错误码细化** | 只用 `codex-*` 前缀 | 上层不比较 code（已核实），但文案可更具体 |

---

## 6. 现在怎么跑（最简步骤）

```powershell
# 1) 宿主（含接管机制）—— 就是我这几轮的测试构建
#    双击这个启动器即可（它只做一件事：设代理再启动客户端）
& "C:\Users\Playe\Documents\DSH hub\DSH Hub\build\windows-ninja\start-codex-client.cmd"

# 2) 扩展已经装在默认位置，不需要环境变量：
#    <exe>\clientExtensions\CodexBackend\{main.dll, regulation.json5}
```

- **代理必须给 Codex 而不是只给系统**：`HTTP_PROXY`/`HTTPS_PROXY=http://127.0.0.1:7892`（本机代理 `u1s1cloud`）。启动器里已设；我另外把用户级环境变量也设了（重启/重登录后任意方式启动都生效）。
- **验证接管生效**：`build\windows-ninja\logs\extension\info.log` 里出现 `takeover requested`、`session/list -> N item(s)`；任务管理器里多一个 `codex.exe`。
- **排错看这两个**：`logs\extension\warn.log`（后端退出/错误/重试）、`logs\session\warn.log`（宿主侧首屏/遮罩）。
- 重新构建扩展：`..\CodexBackendExtension\build.ps1`（`-Deploy <客户端>\clientExtensions` 直接部署，`-Pack` 打 `.ext`；Debug 宿主用 `-Config Debug`）。

---

## 7. 关键事实速查（本会话新增）

| 事实 | 位置 |
|---|---|
| 两个接口与 IID（**对外发布后不可改**） | `source/include/VirtualClass/VirtualApiTakeover.h` |
| 出站咽喉的分流点（一元 RPC） | `source/src/network/DshApiClient.cpp` 的 `post()` 顶部 |
| 可注入的 11 个槽清单与签名 | `misc/API_TAKEOVER_PLAN.zh-CN.md` §2.6 + 扩展的 `DshHostBridge.cpp` |
| 宿主期望的数据形状（逐字段） | `..\CodexBackendExtension\HOST_CONTRACT.zh-CN.md` |
| codex 协议查形状 | `node ..\CodexBackendExtension\tools\schema_peek.js <方法名> [--server]` |
| 无 GUI 验协议 | `..\CodexBackendExtension\build\appserver_smoke.exe --turn "hi"` |
| 测试基线 | 209 passed / 0 failed / 3 skipped（**只覆盖未接管路径**） |
| 网络实测 | 直连 121 秒（5×重试）；走代理 ≈10 秒，回合 `durationMs=9605` |

---

## 8. 追记：扩展功能补全（历史回放 / 模型目录 / 审批 / 工具卡片）

这一轮只动**仓库外**的扩展（`..\CodexBackendExtension\`），宿主侧一行未改。改动清单、字段映射与
验证方法记在扩展的 `MAPPING.zh-CN.md`，这里只留"接手时要先知道的"：

| 事实 | 说明 |
|---|---|
| 历史回放用 `thread/turns/list{threadId,sortDirection:"asc"}` | **不要**用 `thread/read includeTurns=true`：codex 会对它发 `deprecationNotice`（"Full-history hydration is deprecated for paginated threads"）。实测 asc 且不带 limit 时一页就给全部 turn 且每个 turn 带 `items` |
| 历史映射只产四类事件 | `user/message`、`assistant/message`、`tool/call`、`tool/result`。宿主 `compactHistoryEvents` 会把其余类型整类丢掉（`source/src/chat/MessageQuery.cpp:41-44`），喂 chunk 只是白占 200 条配额 |
| `seq` 由扩展自己编 | 宿主只拿它当上翻分页的游标（取**第一项**的 seq 当 `oldestSeq`）。一个工具 item 会产出 call+result 两条，**两条必须用不同的 seq** |
| 快照 `cursor` 必须 > 0 且 > 已有内容的最新 seq | 否则 `seedFromSnapshot` 整份丢弃（`MessageQuery.cpp:832-840`）；`setStreamCursor` 对 `<=0` 直接 return |
| 模型 chip 只在 `groups` 非空时出现 | 宿主 `ModelSelector::updateChip` 里 `visible = hasDirectory && !groups.isEmpty()`：回空壳 ⇒ 整个 chip 不显示，没有任何占位提示。`models[].reasoning` 缺席 = 不公布档位 |
| 审批面板的 `rpcId` 就是 `$events/result` 的 `eventId` | 扩展把 codex 的 `approvalId`/`itemId` 当 eventId 发出去，自己留一张 `eventId → codex 请求 id` 的表再回话 |
| `item/permissions/requestApproval` **不授予额外权限** | 宿主的审批面板只有「允许一次/拒绝」两个按钮，还原不出 codex 的权限档位。两个分支都回"不加权限、按当前沙箱继续"（点"允许"也不会放开） |
| 验证工具（都不需要 GUI） | `appserver_smoke.exe --map-selftest`（离线验映射字段，**52 项**，含用真实载荷校对）、`--plugin-load`（验宿主认不认得出这个 DLL，**11 项**）、`--history [id]`、`--models`；再加 `node tools\probe_turn_stream.js` / `probe_approval.js`（真机回合与审批往返）、`probe_approval_capture.js`（抓真实审批载荷落盘给自检用） |
| **⚠️ 启动 Release 客户端必须带代理** | `x64\Release\DSH Hub.exe` 直接双击 = 没有 `HTTP_PROXY`/`HTTPS_PROXY` ⇒ codex 连不上模型、界面**没有任何输出**（它自己的重试不一定会以 `error` 通知形式冒出来）。已在该目录补 `start-codex-client.cmd`（与 `build\windows-ninja\` 那份同款）。走代理实测：一轮 **11.6 秒**跑完 |
| 真实审批载荷的两个细节 | ① `item/commandExecution/requestApproval` 与 `item/fileChange/requestApproval` **都没有 `approvalId`**（只有 `itemId`）⇒ eventId 必须回退 `itemId`；② 命令审批的 `availableDecisions` 是 `["accept", {amendment}, "cancel"]`，**没有 `decline`**，但我们回 `decline` 服务端**接受**（实测：命令被拒、回合仍 completed；`cancel` 会连整个回合一起打断，语义太重） |
| 日志别把错误文本拼两遍 | `CodexAppServer` 出错时自己已记 `xxx failed: code=… message`，而 onError 的第二参就是**纯错误文本**；回调里再拼一遍会出两行怪日志。`thread/turns/list` 对"还没 materialize 的新会话"回 `-32600 not materialized yet`，那是**正常**情况（该回空历史），已降到 info |
| **"正在载入会话"提示要宿主开接口给扩展收** | 宿主只在"缓存命中"（`MessageHost.cpp:289`）与"历史出错"（`:88-93`）时 `hideLoading()`；follow 快照**成功**那条路只 `emit contentReady`（`:83-85`，收的是启动遮罩），所以那层提示一直要等 6 秒看门狗 —— 日志里的 `loading overlay watchdog fired` 就是它。已在宿主侧开公共虚接口 `VirtualMessageHost`（`VirtualCommon.h` + `DshHostIndex::kMessageHost` + `MessageHost` 继承内联转发），扩展 `DshBridge::hideSessionLoading()` 直接调它，**不用字符串 invokeMethod** |
| 宿主侧新增契约的位置（照抄这个模式） | 纯虚类写在 `source/include/VirtualClass/VirtualCommon.h` + `Q_DECLARE_INTERFACE`；对应类继承 + `Q_INTERFACES(...)`，实现**内联写在自己的头文件里**（转发到私有槽）；登记名加在 `source/include/core/HostExports.h` 的 `DshHostIndex`；`DSHHub::registerHostObjects()` 登记、`~DSHHub` 带身份校验注销 |
| ⚠️ `VirtualCommon.h` 的 Widgets 依赖（**已是无条件包含，勿再改回条件编译**） | 该头无条件 `#include <qboxlayout.h>`（`VirtualTopBar` 用）。中间曾试过 `#if __has_include(<qboxlayout.h>)` 条件编译以让插件只链 Qt Core，**已按决策改回无条件**：扩展侧 CMake 相应加了 `Qt6::Widgets`。实测链接器会把未被调用的 Widgets 丢掉（`dumpbin /dependents build\codex_backend.dll` 仍只有 Qt6Core + CRT + KERNEL32）。代价：以后新写的 Core-only 插件会报 `C1083: 无法打开包括文件 qboxlayout.h` —— 必须链 Widgets 才能 include 这份头 |
| 重试提示不能每条都弹 | codex 一轮会发**五条** `willRetry`（`Reconnecting... 1/5`…`5/5`，每条间隔约 16s）；宿主 `handleTransportError` 直接 `addSystemMessage`（`DSHHub.cpp:1185-1191`），所以扩展必须自己做到"一轮只提示一次" |
| **⚠️ `dshRegister` 的 index 必须逐对象唯一** | `ConnectionManager::RegisterConnection` 会**先 `PublicRemoveConnection(index)`** 再 connect ⇒ 同一个 index 用第二次就把前一条连接断掉（不报错、不警告）。踩过：`Sidebar::addSessionButton` 每个会话按钮都用 `"Sidebar.004"/"Sidebar.005"`、`createWorkspaceGroup` 每个分组都用 `"Sidebar.002"/"Sidebar.003"` ⇒ **只有最后创建的那个**按钮/分组还连着信号（现象：点其它会话只变灰不切换、右键删除也失效）。已改成 `Sidebar.004.<sessionId>` 这类带 id 的索引 |
| 删除会话要真归档 | `workspace/archiveSession` 回 `{"archivedSessionIds": []}` = "什么都没归档"，而宿主按这个集合过滤侧栏（`SessionCatalog::visibleSessions()` 跳过 archived）⇒ 会话下次刷新冒回来。正确做法：调 codex 的 `thread/archive{threadId}`（**持久**，实测归档后 `thread/list` 21→20、换进程仍在），回包给完整归档集合 |
| 「清空会话」是宿主**本地**操作，接管态靠**订阅信号**实现 | `Sidebar::clearAllSessions` → `SessionService::clearAllSessionData` 只做 `rm -rf <dshHome>/sessions` + 删 `workspace.json`，**不发任何 RPC**。判据折腾了 6 版，前 5 版（相邻方法 / 空列表 / 空当前会话 / 信号时间戳比对）全被实测推翻；**最终版**：订阅 `Sidebar::clearRequested`（`Sidebar.cpp:528` 直连），收到后**延时 1.5 秒归档全部会话**。⚠️ 实测信号**比 session/create 晚约 40ms**，所以不能在 session/create 里判断；且**看顺序要看日志行号**，时间戳会被 Logger 的多条写盘路径搞乱。归档**可撤销**（`thread/unarchive`） |
| 本次会话的完整改动记录 | `..\CodexBackendExtension\SESSION_CHANGES.zh-CN.md`（逐文件 + 逐个坑 + 怎么复查） |
